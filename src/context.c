#include "context.h"

static const WhyHistoryEntry *snapshot(const WhyHistory *history,
									   const WhyProcess *process) {
	if (!process)
		return NULL;
	for (const WhyHistoryEntry *e = why_history_first(history); e;
		 e = e->next) {
		if (why_find_process(&e->frame, process->id.pid) == process)
			return e;
	}
	return NULL;
}

WhyParentChain why_parent_chain(const WhyHistory *history,
								const WhyProcess *process) {
	WhyParentChain result = {.end = WHY_CHAIN_MISSING};
	const WhyHistoryEntry *entry = snapshot(history, process);
	if (!entry || process->status != WHY_READ_OK)
		return result;
	result.available = true;
	result.observed_ns = process->end_ns;
	const WhyProcess *current = process;
	for (;;) {
		if (!current->ppid) {
			result.end = WHY_CHAIN_ROOT;
			break;
		}
		if (!current->parent_known) {
			result.end = WHY_CHAIN_UNKNOWN;
			break;
		}
		if (why_identity_equal(current->parent, process->id)) {
			result.end = WHY_CHAIN_CYCLE;
			break;
		}
		for (size_t i = 0; i < result.count; ++i) {
			if (why_identity_equal(current->parent, result.ancestors[i]->id)) {
				result.end = WHY_CHAIN_CYCLE;
				return result;
			}
		}
		if (result.count == WHY_ANCESTOR_LIMIT) {
			result.end = WHY_CHAIN_LIMIT;
			break;
		}
		const WhyProcess *parent =
			why_find_process(&entry->frame, current->parent.pid);
		if (!parent || parent->status != WHY_READ_OK ||
			!why_identity_equal(parent->id, current->parent)) {
			result.end = WHY_CHAIN_MISSING;
			break;
		}
		result.ancestors[result.count++] = parent;
		current = parent;
	}
	return result;
}

static uint64_t subtract(uint64_t value, uint64_t delta) {
	return value > delta ? value - delta : 0;
}
static uint64_t add(uint64_t value, uint64_t delta) {
	return UINT64_MAX - value < delta ? UINT64_MAX : value + delta;
}
static bool continuous(const WhyHistory *history, uint64_t left,
					   uint64_t right) {
	const WhyHistoryEntry *previous = NULL;
	for (const WhyHistoryEntry *e = why_history_first(history); e;
		 e = e->next) {
		if (previous && e->frame.end_ns >= left &&
			previous->frame.begin_ns <= right &&
			(e->sampling_gap ||
			 why_frame_discontinuity(&previous->frame, &e->frame)))
			return false;
		previous = e;
	}
	return true;
}
static bool birth(const WhyFrame *frame, const WhyProcess *p,
				  uint64_t run_boot_start, long hz, WhyTimingContext *result) {
	/* BOOTTIME was read between frame.begin and system.begin. Keep that whole
	 * bracket, plus one starttime tick; never treat first-seen as a birth time.
	 */
	if (hz <= 0 || frame->boot_ns < frame->begin_ns ||
		frame->system.begin_ns < frame->begin_ns ||
		p->end_ns < frame->system.begin_ns)
		return false;
	long double tick = (long double)WHY_SECOND / (long double)hz;
	long double boot_left = (long double)p->id.start_ticks * tick;
	long double boot_right = ((long double)p->id.start_ticks + 1) * tick;
	if (boot_left < (long double)run_boot_start)
		return false;
	long double left = boot_left - ((long double)frame->boot_ns -
									(long double)frame->begin_ns);
	long double right = boot_right - ((long double)frame->boot_ns -
									  (long double)frame->system.begin_ns);
	if (left < 0 || right < left || right >= (long double)UINT64_MAX ||
		right > (long double)p->end_ns)
		return false;
	result->start_earliest_ns = (uint64_t)left;
	result->start_latest_ns = (uint64_t)right;
	if ((long double)result->start_latest_ns < right)
		++result->start_latest_ns;
	return true;
}
WhyTimingContext why_timing_context(const WhyHistory *history,
									WhyIdentity identity,
									const WhyCpuIncident *event, long hz) {
	WhyTimingContext result = {0};
	if (!event || hz <= 0)
		return result;
	uint64_t near_left = subtract(event->start_ns, 2 * WHY_SECOND);
	uint64_t near_right = add(event->start_ns, 2 * WHY_SECOND);
	const WhyHistoryEntry *previous = NULL;
	uint64_t run_boot_start = 0;
	bool seen = false;
	for (const WhyHistoryEntry *e = why_history_first(history); e;
		 e = e->next) {
		if (!previous || e->sampling_gap ||
			why_frame_discontinuity(&previous->frame, &e->frame))
			run_boot_start = e->frame.boot_ns;
		const WhyProcess *p = why_find_process(&e->frame, identity.pid);
		if (!seen && p && p->status == WHY_READ_OK &&
			why_identity_equal(p->id, identity)) {
			seen = true;
			result.start_known =
				birth(&e->frame, p, run_boot_start, hz, &result);
			if (result.start_known) {
				uint64_t left = result.start_earliest_ns < event->start_ns
									? result.start_earliest_ns
									: event->start_ns;
				uint64_t right =
					p->end_ns > event->start_ns ? p->end_ns : event->start_ns;
				result.start_near_rise =
					result.start_earliest_ns >= near_left &&
					result.start_latest_ns <= near_right &&
					continuous(history, left, right);
			}
		}
		if (event->status == WHY_INCIDENT_RECOVERED) {
			for (size_t i = 0; i < e->event_count; ++i) {
				const WhyLifecycle *loss = &e->events[i];
				if (loss->type != WHY_NO_LONGER_OBSERVED ||
					!why_identity_equal(loss->identity, identity))
					continue;
				if (loss->earliest_ns <= add(event->end_ns, 2 * WHY_SECOND) &&
					loss->latest_ns >=
						subtract(event->end_ns, 2 * WHY_SECOND)) {
					result.loss_near_recovery = true;
					result.loss_earliest_ns = loss->earliest_ns;
					result.loss_latest_ns = loss->latest_ns;
				}
			}
		}
		previous = e;
	}
	return result;
}
