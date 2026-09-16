#include "attribution.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	WhyIdentity id;
	const WhyProcess *context;
	uint64_t first_ns, last_ns, last_interval;
	double base_cpu, base_cover, rate, cpu, excess, cover;
	double bin_cpu, bin_cover;
	size_t intervals;
	bool known, stable;
} Item;

typedef struct {
	WhyAttribution *result;
	Item *items;
	size_t *slots, *order;
	size_t capacity, slot_count, count;
	uint64_t baseline_start, baseline_end;
	long hz;
} Work;

static uint64_t mid(const WhySystem *s) {
	return s->begin_ns + (s->end_ns - s->begin_ns) / 2;
}
static uint64_t process_time(const WhyProcess *p) {
	return p->begin_ns + (p->end_ns - p->begin_ns) / 2;
}
static double duration(uint64_t a, uint64_t b) {
	return b > a ? (double)(b - a) / (double)WHY_SECOND : 0;
}
static int identity_compare(WhyIdentity a, WhyIdentity b) {
	if (a.pid != b.pid)
		return (a.pid > b.pid) - (a.pid < b.pid);
	return (a.start_ticks > b.start_ticks) - (a.start_ticks < b.start_ticks);
}
static size_t lookup(Work *w, WhyIdentity id, bool create) {
	uint64_t hash =
		id.start_ticks ^ ((uint64_t)id.pid * UINT64_C(11400714819323198485));
	size_t at = (size_t)(hash % w->slot_count);
	while (w->slots[at]) {
		size_t i = w->slots[at] - 1;
		if (why_identity_equal(w->items[i].id, id))
			return i;
		at = (at + 1) % w->slot_count;
	}
	if (!create || w->count == w->capacity)
		return SIZE_MAX;
	size_t i = w->count++;
	w->slots[at] = i + 1;
	w->items[i] = (Item){.id = id, .first_ns = UINT64_MAX, .stable = true};
	return i;
}
static const char *executable(const WhyProcess *p) {
	if (!p || !p->metadata)
		return NULL;
	const WhyMetadataField *f = &p->metadata->fields[WHY_EXE];
	return f->status == WHY_FIELD_OK && !f->truncated
			   ? (const char *)p->metadata->data + f->offset
			   : NULL;
}
static bool same_context(const WhyProcess *a, const WhyProcess *b) {
	if (a->parent_known != b->parent_known ||
		(a->parent_known && !why_identity_equal(a->parent, b->parent)) ||
		strcmp(a->comm, b->comm) || a->comm_truncated != b->comm_truncated)
		return false;
	const char *x = executable(a), *y = executable(b);
	return x && y ? !strcmp(x, y) : !x && !y;
}
static void context_add(Item *item, const WhyProcess *p) {
	if (!item->context)
		item->context = p;
	else if (!same_context(item->context, p))
		item->stable = false;
}
static void collect(Work *w, const WhyHistory *history) {
	uint64_t begin =
		w->result->has_baseline ? w->baseline_start : w->result->start_ns;
	for (const WhyHistoryEntry *e = why_history_first(history); e;
		 e = e->next) {
		/* Include immediate bracketing observations for exposure and CPU
		 * deltas. */
		if ((e->frame.end_ns < begin &&
			 begin - e->frame.end_ns > 2 * WHY_SECOND) ||
			(e->frame.begin_ns > w->result->end_ns &&
			 e->frame.begin_ns - w->result->end_ns > 2 * WHY_SECOND))
			continue;
		for (size_t j = 0; j < e->frame.count; ++j) {
			const WhyProcess *p = &e->frame.processes[j];
			if (p->status != WHY_READ_OK)
				continue;
			size_t i = lookup(w, p->id, true);
			if (i == SIZE_MAX) {
				++w->result->dropped;
				w->result->issues |= WHY_ATTR_CAPACITY;
				continue;
			}
			Item *item = &w->items[i];
			uint64_t t = process_time(p);
			if (t < item->first_ns)
				item->first_ns = t;
			if (t > item->last_ns)
				item->last_ns = t;
		}
	}
}
static void quality(Work *w, const WhyHistoryEntry *e) {
	if (!e->frame.complete || e->tracking_dropped)
		w->result->issues |= WHY_ATTR_PARTIAL_SCAN;
	if (e->frame.end_ns - e->frame.begin_ns > WHY_SECOND / 5)
		w->result->issues |= WHY_ATTR_SLOW_SCAN;
}
static void pair(Work *w, const WhyHistoryEntry *a, const WhyHistoryEntry *b,
				 uint64_t begin, uint64_t end, bool baseline) {
	if (!a || b->sampling_gap)
		return;
	for (size_t j = 0; j < b->frame.count; ++j) {
		const WhyProcess *p = &b->frame.processes[j];
		if (p->status != WHY_READ_OK)
			continue;
		const WhyProcess *old = why_find_process(&a->frame, p->id.pid);
		WhyCpuOverlap part = why_cpu_overlap(old, p, w->hz, begin, end);
		if (part.validity != WHY_OK || part.covered_seconds <= 0)
			continue;
		size_t i = lookup(w, p->id, false);
		if (i == SIZE_MAX)
			continue;
		Item *item = &w->items[i];
		item->bin_cpu += part.cpu_seconds;
		item->bin_cover += part.covered_seconds;
		if (!baseline) {
			context_add(item, old);
			context_add(item, p);
			uint64_t source_end = process_time(p);
			if (source_end != item->last_interval) {
				++item->intervals;
				item->last_interval = source_end;
			}
		}
	}
}
static void bins(Work *w, const WhyHistory *history, bool baseline) {
	uint64_t begin = baseline ? w->baseline_start : w->result->start_ns;
	uint64_t end = baseline ? w->baseline_end : w->result->end_ns;
	const WhyHistoryEntry *previous = NULL;
	double valid_seconds = 0;
	for (const WhyHistoryEntry *a = why_history_first(history); a && a->next;
		 previous = a, a = a->next) {
		const WhyHistoryEntry *b = a->next;
		uint64_t left = mid(&a->frame.system), right = mid(&b->frame.system);
		if (left < begin)
			left = begin;
		if (right > end)
			right = end;
		if (right <= left)
			continue;
		quality(w, a);
		quality(w, b);
		WhySystemInterval system =
			why_system_interval(&a->frame, &b->frame, w->hz);
		if (system.validity != WHY_OK || b->sampling_gap) {
			w->result->issues |= WHY_ATTR_GAP;
			continue;
		}
		valid_seconds += duration(left, right);
		if (!baseline)
			w->result->system_seconds +=
				system.cpu_seconds * (duration(left, right) /
									  duration(system.start_ns, system.end_ns));
		for (size_t i = 0; i < w->count; ++i)
			w->items[i].bin_cpu = w->items[i].bin_cover = 0;
		/* Sequential procfs scans put each process read after its system read.
		 * A system bin receives the tail of (previous,a) and head of (a,b). */
		pair(w, previous, a, left, right, baseline);
		pair(w, a, b, left, right, baseline);
		for (size_t i = 0; i < w->count; ++i) {
			Item *item = &w->items[i];
			if (baseline) {
				item->base_cpu += item->bin_cpu;
				item->base_cover += item->bin_cover;
			} else {
				item->cpu += item->bin_cpu;
				item->cover += item->bin_cover;
				double increase = item->bin_cpu - item->rate * item->bin_cover;
				if (item->known && increase > 0)
					item->excess += increase;
			}
		}
	}
	if (!baseline)
		w->result->system_covered_seconds = valid_seconds;
	if (valid_seconds + 1e-6 < duration(begin, end))
		w->result->issues |= WHY_ATTR_TRUNCATED;
}
static bool born_after_baseline(const Work *w, const WhyHistory *h,
								WhyIdentity id) {
	for (const WhyHistoryEntry *e = why_history_first(h); e; e = e->next) {
		uint64_t t = mid(&e->frame.system);
		if (t >= w->baseline_end && t - w->baseline_end <= 2 * WHY_SECOND &&
			e->frame.boot_ns >= e->frame.begin_ns) {
			long double boundary =
				(long double)w->baseline_end +
				(long double)(e->frame.boot_ns - e->frame.begin_ns);
			return (long double)id.start_ticks * (long double)WHY_SECOND /
					   (long double)w->hz >
				   boundary;
		}
	}
	return false;
}

/* Context sort is deterministic; only stable same-parent siblings can merge. */
static int group_compare_items(const Item *a, const Item *b) {
	bool ag = a->stable && a->context && a->context->parent_known &&
			  !a->context->comm_truncated;
	bool bg = b->stable && b->context && b->context->parent_known &&
			  !b->context->comm_truncated;
	if (!ag || !bg) {
		if (ag != bg)
			return ag ? -1 : 1;
		return identity_compare(a->id, b->id);
	}
	int parent = identity_compare(a->context->parent, b->context->parent);
	if (parent)
		return parent;
	const char *x = executable(a->context), *y = executable(b->context);
	if ((x != NULL) != (y != NULL))
		return x ? -1 : 1;
	int name = strcmp(x ? x : a->context->comm, y ? y : b->context->comm);
	return name;
}
/* In-place heapsort avoids global comparator state and unbounded recursion. */
static bool higher(const Work *w, size_t a, size_t b, int mode) {
	if (mode == 0) {
		int cmp = group_compare_items(&w->items[a], &w->items[b]);
		return cmp ? cmp > 0
				   : identity_compare(w->items[a].id, w->items[b].id) > 0;
	}
	const WhyContributor *x = &w->result->rows[a], *y = &w->result->rows[b];
	if (mode == 2 && x->baseline_known != y->baseline_known)
		return !x->baseline_known;
	double xs = mode == 2 ? x->excess_seconds : x->cpu_seconds;
	double ys = mode == 2 ? y->excess_seconds : y->cpu_seconds;
	if (xs != ys)
		return xs < ys;
	if (x->cpu_seconds != y->cpu_seconds)
		return x->cpu_seconds < y->cpu_seconds;
	return identity_compare(x->identity, y->identity) > 0;
}
static void sift(const Work *w, size_t *order, size_t root, size_t count,
				 int mode) {
	while (root * 2 + 1 < count) {
		size_t child = root * 2 + 1;
		if (child + 1 < count &&
			higher(w, order[child + 1], order[child], mode))
			++child;
		if (!higher(w, order[child], order[root], mode))
			break;
		size_t tmp = order[root];
		order[root] = order[child];
		order[child] = tmp;
		root = child;
	}
}
static void sort(const Work *w, size_t *order, size_t count, int mode) {
	for (size_t i = count / 2; i > 0; --i)
		sift(w, order, i - 1, count, mode);
	for (size_t i = count; i > 1; --i) {
		size_t tmp = order[0];
		order[0] = order[i - 1];
		order[i - 1] = tmp;
		sift(w, order, 0, i - 1, mode);
	}
}
static int total_compare(const void *a, const void *b) {
	const WhyContributor *x = a, *y = b;
	if (x->cpu_seconds != y->cpu_seconds)
		return x->cpu_seconds > y->cpu_seconds ? -1 : 1;
	return identity_compare(x->identity, y->identity);
}
static void rows(Work *w) {
	size_t used = 0;
	for (size_t i = 0; i < w->count; ++i)
		if (w->items[i].cpu > 0)
			w->order[used++] = i;
	sort(w, w->order, used, 0);
	Item *prior = NULL;
	for (size_t j = 0; j < used; ++j) {
		Item *item = &w->items[w->order[j]];
		bool merge = prior && !group_compare_items(prior, item);
		WhyContributor *row;
		if (!merge) {
			row = &w->result->rows[w->result->count++];
			*row = (WhyContributor){.identity = item->id,
									.context = item->context,
									.baseline_known = true,
									.context_stable = true,
									.weak_group = !executable(item->context)};
		} else
			row = &w->result->rows[w->result->count - 1];
		++row->members;
		row->cpu_seconds += item->cpu;
		row->covered_seconds += item->cover;
		row->excess_seconds += item->excess;
		row->intervals += item->intervals;
		row->baseline_known &= item->known;
		row->context_stable &= item->stable;
		prior = item;
	}
	qsort(w->result->rows, w->result->count, sizeof *w->result->rows,
		  total_compare);
	for (size_t i = 0; i < w->result->count; ++i)
		w->result->increment_order[i] = i;
	sort(w, w->result->increment_order, w->result->count, 2);
}

void why_attribution_destroy(WhyAttribution *result) {
	if (!result)
		return;
	free(result->rows);
	free(result->increment_order);
	free(result);
}
static WhyAttribution *attribute(const WhyHistory *h,
								 const WhyCpuIncident *event, long hz,
								 size_t capacity, uint64_t start,
								 uint64_t end) {
	const WhyHistoryEntry *first = why_history_first(h),
						  *last = why_history_latest(h);
	if (!first || !last || hz <= 0 || (!event && end <= start) || !capacity ||
		capacity > WHY_ATTRIBUTION_CAPACITY ||
		(event && (event->end_ns <= event->start_ns ||
				   event->baseline_end_ns <= event->baseline_start_ns ||
				   event->baseline_end_ns > event->start_ns))) {
		errno = EINVAL;
		return NULL;
	}
	size_t bytes =
		sizeof(WhyAttribution) +
		capacity * (sizeof(Item) + sizeof(WhyContributor) + 4 * sizeof(size_t));
	if (bytes > WHY_ATTRIBUTION_BYTES) {
		errno = ENOSPC;
		return NULL;
	}
	Work w = {.capacity = capacity, .slot_count = 2 * capacity, .hz = hz};
	w.result = calloc(1, sizeof *w.result);
	w.items = calloc(capacity, sizeof *w.items);
	w.slots = calloc(w.slot_count, sizeof *w.slots);
	w.order = calloc(capacity, sizeof *w.order);
	if (w.result) {
		w.result->rows = calloc(capacity, sizeof *w.result->rows);
		w.result->increment_order =
			calloc(capacity, sizeof *w.result->increment_order);
	}
	if (!w.result || !w.items || !w.slots || !w.order || !w.result->rows ||
		!w.result->increment_order) {
		free(w.items);
		free(w.slots);
		free(w.order);
		why_attribution_destroy(w.result);
		return NULL;
	}
	WhyAttribution *r = w.result;
	r->allocation_bytes = bytes;
	r->has_baseline = event != NULL;
	r->start_ns = event ? event->start_ns : start;
	r->end_ns = event ? event->end_ns : end;
	w.baseline_start = event ? event->baseline_start_ns : 0;
	w.baseline_end = event ? event->baseline_end_ns : 0;
	if (event && event->status != WHY_INCIDENT_RECOVERED)
		r->issues |= WHY_ATTR_PROVISIONAL;
	collect(&w, h);
	r->identities = w.count;
	if (event)
		bins(&w, h, true);
	for (size_t i = 0; i < w.count; ++i) {
		Item *item = &w.items[i];
		if (event &&
			item->base_cover >=
				0.8 * duration(w.baseline_start, w.baseline_end) &&
			item->base_cover > 0) {
			item->known = true;
			item->rate = item->base_cpu / item->base_cover;
		} else if (event && item->base_cover == 0 &&
				   item->first_ns >= w.baseline_end &&
				   born_after_baseline(&w, h, item->id))
			item->known = true;
	}
	bins(&w, h, false);
	double exposure = 0, coverage = 0, known_cpu = 0;
	for (size_t i = 0; i < w.count; ++i) {
		Item *item = &w.items[i];
		uint64_t a =
			item->first_ns > r->start_ns ? item->first_ns : r->start_ns;
		uint64_t b = item->last_ns < r->end_ns ? item->last_ns : r->end_ns;
		exposure += duration(a, b);
		coverage += item->cover;
		r->process_seconds += item->cpu;
		if (item->known) {
			known_cpu += item->cpu;
			r->excess_seconds += item->excess;
		}
	}
	r->coverage_ratio = exposure > 0 ? coverage / exposure : 0;
	r->accounted_ratio =
		r->system_seconds > 0 ? r->process_seconds / r->system_seconds : 0;
	r->residual_seconds = r->system_seconds - r->process_seconds;
	if (r->coverage_ratio < 0.9)
		r->issues |= WHY_ATTR_LOW_COVERAGE;
	if (r->system_seconds <= 0 || r->accounted_ratio < 0.8 ||
		r->accounted_ratio > 1.05)
		r->issues |= WHY_ATTR_ACCOUNTING;
	if (!event || known_cpu < 0.8 * r->process_seconds)
		r->issues |= WHY_ATTR_UNKNOWN_BASELINE;
	rows(&w);
	size_t likely = SIZE_MAX, candidates = 0;
	for (size_t i = 0; i < r->count; ++i) {
		WhyContributor *row = &r->rows[i];
		if (!r->issues && row->baseline_known && row->context_stable &&
			row->intervals >= 2 && r->excess_seconds > 0 &&
			row->excess_seconds >= 0.5 * r->excess_seconds &&
			row->excess_seconds >= 0.1 * r->system_seconds) {
			likely = i;
			++candidates;
		}
	}
	if (candidates == 1)
		r->rows[likely].likely = true;
	free(w.items);
	free(w.slots);
	free(w.order);
	return r;
}

WhyAttribution *why_attribute_window(const WhyHistory *h, uint64_t start,
									 uint64_t end, long hz, size_t capacity) {
	return attribute(h, NULL, hz, capacity, start, end);
}
WhyAttribution *why_attribute(const WhyHistory *h, const WhyCpuIncident *event,
							  long hz, size_t capacity) {
	const WhyHistoryEntry *first = why_history_first(h),
						  *last = why_history_latest(h);
	return attribute(h, event, hz, capacity,
					 first ? mid(&first->frame.system) : 0,
					 last ? mid(&last->frame.system) : 0);
}
