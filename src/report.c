#include "report.h"
#include <inttypes.h>
#include <string.h>
#include <unistd.h>

static void line(FILE *out, const char *text) { fprintf(out, "%s\n", text); }

static void escaped_bytes(FILE *out, const unsigned char *text, size_t length) {
	for (const unsigned char *p = text; p < text + length; ++p) {
		if (*p < 32 || *p >= 127 || *p == '\\')
			fprintf(out, "\\x%02x", (unsigned)*p);
		else
			fputc(*p, out);
	}
}

static void escaped(FILE *out, const char *text) {
	escaped_bytes(out, (const unsigned char *)text, strlen(text));
}

static const char *field_status(WhyFieldStatus status) {
	switch (status) {
	case WHY_FIELD_OK:
		return "available";
	case WHY_FIELD_EMPTY:
		return "empty";
	case WHY_FIELD_MISSING:
		return "missing";
	case WHY_FIELD_DENIED:
		return "permission denied";
	case WHY_FIELD_ERROR:
		return "read error";
	case WHY_FIELD_UNVERIFIED:
		return "identity recheck failed";
	case WHY_FIELD_BUDGET:
		return "metadata budget reached";
	}
	return "unknown";
}

static void display_metadata(FILE *out, const WhyProcess *p) {
	if (!p->metadata) {
		fprintf(out, "  Context: unavailable (%s)\n",
				field_status(p->metadata_status));
		return;
	}
	const char *names[] = {"Arguments (NUL-separated)", "Working directory",
						   "Executable"};
	fprintf(out, "  Context observed at monotonic %.3f s\n",
			(double)p->metadata->end_ns / (double)WHY_SECOND);
	for (size_t i = 0; i < WHY_METADATA_FIELDS; ++i) {
		const WhyMetadataField *f = &p->metadata->fields[i];
		fprintf(out, "  %s: ", names[i]);
		if (f->status == WHY_FIELD_OK)
			escaped_bytes(out, p->metadata->data + f->offset, f->length);
		else
			fprintf(out, "[%s]", field_status(f->status));
		if (f->truncated)
			fprintf(out, " [truncated]");
		fputc('\n', out);
	}
}

void why_report_history(FILE *out, const WhyHistory *history, bool timeline) {
	WhyHistoryStats stats = why_history_stats(history);
	fprintf(out,
			"\nHistory: %zu frames, %.1f seconds, %zu / %zu charged bytes; "
			"%zu evicted, %zu untracked observations\n",
			stats.frames,
			stats.frames ? (double)(stats.newest_ns - stats.oldest_ns) /
							   (double)WHY_SECOND
						 : 0,
			stats.bytes, stats.budget, stats.evicted_frames,
			stats.tracking_dropped);
	if (!timeline)
		return;
	line(out,
		 "Recorded lifecycle observations (monotonic seconds; disappearance is "
		 "not an exact exit):");
	for (const WhyHistoryEntry *entry = why_history_first(history); entry;
		 entry = entry->next) {
		if (entry->tracking_dropped)
			fprintf(
				out,
				"  %zu new observations could not be tracked in this frame\n",
				entry->tracking_dropped);
		if (entry->sampling_gap)
			fprintf(out, "  %.3f  sampling gap\n",
					(double)entry->frame.end_ns / (double)WHY_SECOND);
		for (size_t i = 0; i < entry->event_count; ++i) {
			const WhyLifecycle *event = &entry->events[i];
			fprintf(out, "  %.3f..%.3f  %u:%" PRIu64 "  %s\n",
					(double)event->earliest_ns / (double)WHY_SECOND,
					(double)event->latest_ns / (double)WHY_SECOND,
					event->identity.pid, event->identity.start_ticks,
					why_lifecycle_name(event->type));
		}
	}
}

static void display_rank(FILE *out, const WhyAttribution *r, bool increment) {
	fprintf(out, "    %s (CPU seconds):\n",
			increment ? "Increase above own baseline"
					  : "Total observed activity");
	double shown = 0;
	for (size_t j = 0; j < r->count && j < 3; ++j) {
		size_t i = increment ? r->increment_order[j] : j;
		const WhyContributor *row = &r->rows[i];
		fprintf(out, "      %zu. ", j + 1);
		if (row->context)
			escaped(out, row->context->comm);
		else
			fprintf(out, "PID %u", row->identity.pid);
		if (row->members > 1)
			fprintf(out, " x %zu%s", row->members,
					row->weak_group ? " [name-based group]" : "");
		else
			fprintf(out, " [%u:%" PRIu64 "]", row->identity.pid,
					row->identity.start_ticks);
		fprintf(out, "  total=%.3f", row->cpu_seconds);
		if (r->end_ns > r->start_ns)
			fprintf(out, "  avg_cores=%.3f",
					row->cpu_seconds * (double)WHY_SECOND /
						(double)(r->end_ns - r->start_ns));
		if (!row->context_stable)
			fprintf(out, " [context changed; kept separate]");
		if (r->has_baseline) {
			if (row->baseline_known)
				fprintf(out, "  increase=%.3f", row->excess_seconds);
			else
				fprintf(out, "  increase=unknown/partial");
		}
		if (row->context && row->context->parent_known)
			fprintf(out, "  observed parent=%u:%" PRIu64,
					row->context->parent.pid, row->context->parent.start_ticks);
		if (row->identity.pid == (uint32_t)getpid())
			fprintf(out, " [why recorder]");
		if (row->likely)
			fprintf(out, " [primary observed CPU increase contributor]");
		fputc('\n', out);
		shown += row->cpu_seconds;
	}
	if (r->count > 3)
		fprintf(out, "      Other %zu rows: %.3f total CPU seconds\n",
				r->count - 3, r->process_seconds - shown);
	if (!r->count)
		line(out, "      No measurable process CPU in this window.");
}

static void attribution_result(FILE *out, const WhyAttribution *r, bool event) {
	if (event)
		display_rank(out, r, true);
	display_rank(out, r, false);
	if (r->system_covered_seconds <= 0)
		line(out,
			 "    No comparable system intervals: CPU totals are unavailable, "
			 "not measured zero.");
	else
		fprintf(out,
				"    Comparable retained CPU: system=%.3f, processes=%.3f, "
				"signed residual=%.3f seconds\n",
				r->system_seconds, r->process_seconds, r->residual_seconds);
	if (r->system_seconds > 0)
		fprintf(out,
				"    Accounting ratio=%.1f%%; observed-time coverage=%.1f%%\n",
				100 * r->accounted_ratio, 100 * r->coverage_ratio);
	size_t unknown_rows = 0;
	for (size_t i = 0; i < r->count; ++i)
		if (!r->rows[i].baseline_known)
			++unknown_rows;
	if (event && unknown_rows)
		fprintf(out, "    %zu rows have unknown/partial increment baselines.\n",
				unknown_rows);
	if (r->issues &&
		(event || (r->issues & ~(unsigned)WHY_ATTR_UNKNOWN_BASELINE))) {
		line(out, event ? "    Primary-contributor label withheld:"
						: "    Ranking limitations:");
		const struct {
			unsigned bit;
			const char *text;
		} reasons[] = {
			{WHY_ATTR_PROVISIONAL, "event ongoing or interrupted"},
			{WHY_ATTR_TRUNCATED, "baseline/event history incomplete"},
			{WHY_ATTR_GAP, "system sampling gap"},
			{WHY_ATTR_PARTIAL_SCAN, "partial scan or lifecycle tracking"},
			{WHY_ATTR_SLOW_SCAN, "scan exceeds 200 ms"},
			{WHY_ATTR_LOW_COVERAGE, "less than 90% observed-time coverage"},
			{WHY_ATTR_ACCOUNTING,
			 "CPU accounting outside 80–105% or undefined"},
			{WHY_ATTR_UNKNOWN_BASELINE, "insufficient per-process baselines"},
			{WHY_ATTR_CAPACITY, "analysis identity capacity reached"}};
		for (size_t i = 0; i < sizeof reasons / sizeof *reasons; ++i)
			if ((event || reasons[i].bit != WHY_ATTR_UNKNOWN_BASELINE) &&
				(r->issues & reasons[i].bit))
				fprintf(out, "      %s\n", reasons[i].text);
	}
	if (r->dropped)
		fprintf(out, "    %zu observations omitted at the identity cap.\n",
				r->dropped);
	line(out, "    Residual includes missing observations and accounting "
			  "differences; this is not proof of causality.");
}

static void display_attribution(FILE *out, const WhyHistory *history,
								const WhyCpuIncident *event, long hz) {
	WhyAttribution *r =
		why_attribute(history, event, hz, WHY_ATTRIBUTION_CAPACITY);
	if (!r) {
		line(out,
			 "    Contributor analysis unavailable (insufficient observations "
			 "or allocation failure).");
		return;
	}
	attribution_result(out, r, event != NULL);
	why_attribution_destroy(r);
}

static void display_incident(FILE *out, const WhyHistory *history,
							 const WhyCpuIncident *event, long hz) {
	const char *status = event->status == WHY_INCIDENT_RECOVERED ? "recovered"
						 : event->status == WHY_INCIDENT_INTERRUPTED
							 ? "interrupted"
							 : "ongoing (provisional)";
	fprintf(
		out,
		"  CPU spike %.3f..%.3f s: %s; baseline %.1f%%, sampled peak %.1f%%\n",
		(double)event->start_ns / (double)WHY_SECOND,
		(double)event->end_ns / (double)WHY_SECOND, status, event->baseline_pct,
		event->peak_pct);
	fprintf(out, "    Peak interval %.3f..%.3f s; system CPU %.3f seconds\n",
			(double)event->peak_start_ns / (double)WHY_SECOND,
			(double)event->peak_end_ns / (double)WHY_SECOND,
			event->cpu_seconds);
	display_attribution(out, history, event, hz);
}

void why_report_spikes(FILE *out, const WhyHistory *history, long hz) {
	line(out, "\nCPU spike analysis (whole-machine CPU; sampled intervals):");
	size_t count = 0;
	for (const WhyHistoryEntry *entry = why_history_first(history); entry;
		 entry = entry->next) {
		if (entry->incident_completed) {
			display_incident(out, history, &entry->incident, hz);
			++count;
		}
	}
	const WhyCpuDetector *detector = why_history_detector(history);
	WhyCpuIncident current;
	if (why_detector_current(detector, &current)) {
		display_incident(out, history, &current, hz);
		++count;
	}
	if (!count)
		line(out,
			 "  No confirmed CPU spike in retained summaries; this does not "
			 "rule out other problems.");
	if (detector->state == WHY_WARMUP)
		fprintf(out, "  Baseline warming up: %zu/%u valid intervals.\n",
				detector->baseline_count, WHY_BASELINE_MIN);
	if (detector->state == WHY_CANDIDATE)
		line(out, "  One high interval observed; waiting for confirmation.");
	if (!count)
		display_attribution(out, history, NULL, hz);
}

void why_report_sample(FILE *out, const WhyFrame *previous,
					   const WhyFrame *current, long hz, size_t sequence,
					   bool details) {
	bool gap = previous && why_frame_discontinuity(previous, current);
	WhyCpu system = why_system_cpu(previous ? &previous->system : NULL,
								   &current->system, hz);
	if (gap)
		system.validity = WHY_CLOCK_GAP;
	fprintf(out, "\nSample %zu | %zu visible entries | scan %.2f ms\n",
			sequence, current->count,
			(double)(current->end_ns - current->begin_ns) / 1e6);
	if (system.validity == WHY_OK)
		fprintf(out,
				"System CPU: %.1f%% busy (whole machine), %.3f CPU seconds\n",
				system.busy_pct, system.cpu_seconds);
	else
		fprintf(out, "System CPU: unavailable (%s)\n",
				why_validity_name(system.validity));
	fprintf(out,
			"Scan: %s; skipped=%zu gone=%zu denied=%zu malformed=%zu I/O "
			"errors=%zu\n",
			current->complete ? "complete" : "partial", current->skipped,
			current->gone, current->denied, current->malformed,
			current->io_errors);
	if (current->metadata_skipped)
		fprintf(out, "Context unavailable for %zu readable processes.\n",
				current->metadata_skipped);
	if (current->end_ns - current->begin_ns > WHY_SECOND / 5)
		line(out, "Timing quality: scan exceeds 200 ms; attribution would be "
				  "degraded.");
	line(out, "PID\tPPID\tSTART_TICKS\tCPU_SECONDS\tCORES\tSTATE\tPARENT_"
			  "ID\tCOMMAND");
	for (size_t i = 0; i < current->count; ++i) {
		const WhyProcess *p = &current->processes[i];
		if (p->status != WHY_READ_OK)
			continue;
		const WhyProcess *old =
			previous ? why_find_process(previous, p->id.pid) : NULL;
		WhyCpu cpu = why_process_cpu(old, p, hz);
		if (gap || system.validity == WHY_TOPOLOGY_CHANGED)
			cpu.validity = WHY_CLOCK_GAP;
		fprintf(out, "%u\t%u\t%" PRIu64 "\t", p->id.pid, p->ppid,
				p->id.start_ticks);
		if (cpu.validity == WHY_OK)
			fprintf(out, "%.3f\t%.3f\t", cpu.cpu_seconds, cpu.cores);
		else
			fprintf(out, "n/a\tn/a\t");
		fprintf(out, "%c\t", p->state);
		if (p->parent_known)
			fprintf(out, "%u:%" PRIu64 "\t", p->parent.pid,
					p->parent.start_ticks);
		else
			fprintf(out, "unknown\t");
		escaped(out, p->comm);
		if (p->comm_truncated)
			fprintf(out, " [name truncated]");
		if (p->id.pid == (uint32_t)getpid())
			fprintf(out, " [why recorder]");
		if (cpu.validity != WHY_OK)
			fprintf(out, " [%s]", why_validity_name(cpu.validity));
		fputc('\n', out);
		if (details)
			display_metadata(out, p);
	}
	fflush(out);
}

static uint64_t system_mid(const WhyHistoryEntry *e) {
	return e->frame.system.begin_ns +
		   (e->frame.system.end_ns - e->frame.system.begin_ns) / 2;
}

void why_report_query(FILE *out, const WhyHistory *history, long hz,
					  unsigned seconds, uint64_t now) {
	const WhyHistoryEntry *first = why_history_first(history);
	const WhyHistoryEntry *last = why_history_latest(history);
	fprintf(out,
			"Recent CPU activity: requested %u seconds; nominal sampling 1 "
			"second\n",
			seconds);
	if (!first || !last) {
		line(out, "Recorder is warming up; no observations yet.");
		return;
	}
	uint64_t end = system_mid(last), earliest = system_mid(first);
	uint64_t span = (uint64_t)seconds * WHY_SECOND;
	uint64_t requested = now > span ? now - span : 0;
	uint64_t start = requested > earliest ? requested : earliest;
	fprintf(out, "Latest observation age: %.3f seconds\n",
			now > end ? (double)(now - end) / (double)WHY_SECOND : 0);
	if (requested < earliest)
		line(out, "Requested prefix unavailable: recording started later or "
				  "history expired.");
	if (start >= end) {
		line(out, "No comparable observations in the requested window; CPU is "
				  "unavailable.");
		return;
	}
	fprintf(out,
			"Query window: %.3f..%.3f monotonic seconds (%.3f seconds); gaps "
			"remain unknown\n",
			(double)start / (double)WHY_SECOND,
			(double)end / (double)WHY_SECOND,
			(double)(end - start) / (double)WHY_SECOND);
	double busy = 0, covered = 0, peak = 0;
	for (const WhyHistoryEntry *e = first; e && e->next; e = e->next) {
		WhySystemInterval interval =
			why_system_interval(&e->frame, &e->next->frame, hz);
		uint64_t left = interval.start_ns > start ? interval.start_ns : start;
		uint64_t right = interval.end_ns < end ? interval.end_ns : end;
		if (interval.validity != WHY_OK || e->next->sampling_gap ||
			right <= left)
			continue;
		double time = (double)(right - left) / (double)WHY_SECOND;
		busy += interval.busy_pct * time;
		covered += time;
		if (interval.busy_pct > peak)
			peak = interval.busy_pct;
	}
	if (covered > 0)
		fprintf(out,
				"System CPU: average %.1f%% over %.3f valid seconds; sampled "
				"peak %.1f%%\n",
				busy / covered, covered, peak);
	else
		line(out, "System CPU: unavailable (no valid intervals).");
	WhyAttribution *total =
		why_attribute_window(history, start, end, hz, WHY_ATTRIBUTION_CAPACITY);
	if (total) {
		line(out, "Query-window totals (not full-event totals):");
		attribution_result(out, total, false);
		why_attribution_destroy(total);
	} else
		line(out, "Query-window contributor analysis unavailable.");
	/* At most eight full events keep rendering work and output bounded. */
	WhyCpuIncident events[8];
	size_t count = 0;
	for (const WhyHistoryEntry *e = first; e; e = e->next) {
		if (!e->incident_completed || e->incident.end_ns <= start ||
			e->incident.start_ns >= end)
			continue;
		events[count % 8] = e->incident;
		++count;
	}
	WhyCpuIncident current;
	if (why_detector_current(why_history_detector(history), &current) &&
		current.end_ns > start && current.start_ns < end) {
		events[count % 8] = current;
		++count;
	}
	if (count) {
		line(out, "Overlapping spikes: FULL EVENT results below, not "
				  "query-window increments.");
		size_t skip = count > 8 ? count - 8 : 0;
		if (skip)
			fprintf(out,
					"  %zu older overlapping events omitted; showing latest "
					"eight.\n",
					skip);
		for (size_t i = skip; i < count; ++i)
			display_incident(out, history, &events[i % 8], hz);
	} else
		line(out, "No confirmed CPU spike overlaps this window; steady high "
				  "CPU or missing earlier baseline can hide an increase.");
	const WhyCpuDetector *detector = why_history_detector(history);
	if (detector->state == WHY_WARMUP)
		fprintf(out, "Baseline warming up: %zu/%u valid intervals.\n",
				detector->baseline_count, WHY_BASELINE_MIN);
	if (detector->state == WHY_CANDIDATE)
		line(out, "One high interval observed; waiting for confirmation.");
}
