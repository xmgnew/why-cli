/* Opt-in measurement harness. Prints aggregate JSON, never process details. */
#include "report.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define MAX_SAMPLES 3600U
#define MAX_QUERIES 100U
#define REPORT_BYTES (128U * 1024U)
static volatile sig_atomic_t stopped;
static void stop(int sig) {
	(void)sig;
	stopped = 1;
}
static uint64_t cpu_ns(void) {
	struct timespec ts;
	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * WHY_SECOND + (uint64_t)ts.tv_nsec;
}
static int compare(const void *a, const void *b) {
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}
static void distribution(const char *name, double *values, size_t count) {
	qsort(values, count, sizeof *values, compare);
	printf("  \"%s_ms\": {\"count\": %zu, \"p50\": %.6f, \"p95\": %.6f, "
		   "\"p99\": %.6f},\n",
		   name, count, values[(count * 50 + 99) / 100 - 1],
		   values[(count * 95 + 99) / 100 - 1],
		   values[(count * 99 + 99) / 100 - 1]);
}
static bool number(const char *text, unsigned limit, unsigned *value) {
	char *end;
	errno = 0;
	unsigned long n = strtoul(text, &end, 10);
	if (text[0] < '1' || text[0] > '9' || errno || *end || n > limit)
		return false;
	*value = (unsigned)n;
	return true;
}
static bool synthetic_init(WhyFrame *frame, unsigned count) {
	frame->count = count;
	for (unsigned i = 0; i < count; ++i) {
		WhyProcess *p = &frame->processes[i];
		p->id = (WhyIdentity){i + 1, 0};
		p->status = WHY_READ_OK;
		strcpy(p->comm, i ? "worker" : "supervisor");
		p->ppid = i ? 1 : 0;
		p->parent_known = i != 0;
		p->parent = (WhyIdentity){1, 0};
		p->metadata = calloc(1, sizeof(WhyMetadata) + 256);
		if (!p->metadata)
			return false;
		p->metadata->references = 1;
		p->metadata->allocation_bytes = sizeof(WhyMetadata) + 256;
		p->metadata->verified = true;
		const char *path = i ? "/synthetic/worker" : "/synthetic/supervisor";
		strcpy((char *)p->metadata->data, path);
		p->metadata->fields[WHY_EXE].length = strlen(path);
		p->metadata->fields[WHY_CMDLINE].status = WHY_FIELD_MISSING;
		p->metadata->fields[WHY_CWD].status = WHY_FIELD_MISSING;
	}
	why_resolve_parents(frame);
	return true;
}
static void synthetic_sample(WhyFrame *f, unsigned sequence) {
	uint64_t t = (uint64_t)(sequence + 1) * WHY_SECOND;
	f->begin_ns = f->end_ns = f->boot_ns = f->system.begin_ns =
		f->system.end_ns = t;
	f->complete = true;
	f->system.cpu_count = 8;
	for (unsigned i = 0; i < 8; ++i)
		f->system.cpus[i] = true;
	uint64_t busy = (f->count + 9) / 10;
	uint64_t capacity = busy > 800 ? busy : 800;
	f->system.ticks[0] = (uint64_t)sequence * busy;
	f->system.ticks[3] = (uint64_t)sequence * (capacity - busy);
	for (size_t i = 0; i < f->count; ++i) {
		f->processes[i].begin_ns = f->processes[i].end_ns = t;
		f->processes[i].user_ticks = i % 10 == 0 ? sequence : 0;
	}
}
int main(int argc, char **argv) {
	bool synthetic = argc > 1 && !strcmp(argv[1], "synthetic");
	bool live = argc > 1 && !strcmp(argv[1], "live");
	unsigned processes = 1000, samples = synthetic ? 360 : 30, queries = 20;
	int at = 2;
	if ((!synthetic && !live) || argc > (synthetic ? 5 : 4))
		goto usage;
	if (synthetic && at < argc &&
		!number(argv[at++], WHY_MAX_PROCESSES, &processes))
		goto usage;
	if (at < argc && !number(argv[at++], MAX_SAMPLES, &samples))
		goto usage;
	if (at < argc && !number(argv[at++], MAX_QUERIES, &queries))
		goto usage;
	if (samples < 2)
		goto usage;
#ifndef __linux__
	if (live) {
		fputs("Live measurement requires Linux.\n", stderr);
		return 1;
	}
#endif
	long hz = synthetic ? 100 : sysconf(_SC_CLK_TCK);
	if (hz <= 0)
		return 1;
	struct sigaction action = {0};
	action.sa_handler = stop;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	WhyFrame frames[2] = {0};
	WhyHistory *history = NULL;
	double scan[MAX_SAMPLES], append[MAX_SAMPLES], query[MAX_QUERIES];
	size_t entries_min = SIZE_MAX, entries_max = 0, gaps = 0, partial = 0,
		   slow = 0;
	size_t skipped = 0, denied = 0, malformed = 0, io_errors = 0,
		   metadata_skipped = 0;
	int status = 1;
	char *buffer = NULL;
	if (!why_frame_init(&frames[0]) || !why_frame_init(&frames[1]))
		goto cleanup;
	if (synthetic && (!synthetic_init(&frames[0], processes) ||
					  !synthetic_init(&frames[1], processes)))
		goto cleanup;
	history = why_history_create(WHY_HISTORY_SECONDS * WHY_SECOND,
								 WHY_HISTORY_BYTES - WHY_ATTRIBUTION_BYTES,
								 WHY_MAX_PROCESSES);
	if (!history)
		goto cleanup;
	buffer = malloc(REPORT_BYTES);
	if (!buffer)
		goto cleanup;
	uint64_t begin = why_now_ns(), cpu_begin = cpu_ns(), deadline = begin;
	uint32_t cursor = 0;
	for (unsigned i = 0; i < samples; ++i) {
		if (stopped)
			goto cleanup;
		WhyFrame *current = &frames[i % 2];
		const WhyFrame *previous = i ? &frames[(i - 1) % 2] : NULL;
		uint64_t start = why_now_ns();
		if (synthetic)
			synthetic_sample(current, i);
		else if (!why_collect("/proc", &cursor, previous, current))
			goto cleanup;
		uint64_t collected = why_now_ns();
		if (!why_history_append(history, current, hz))
			goto cleanup;
		uint64_t appended = why_now_ns();
		scan[i] = (double)(collected - start) / 1e6;
		append[i] = (double)(appended - collected) / 1e6;
		if (current->count < entries_min)
			entries_min = current->count;
		if (current->count > entries_max)
			entries_max = current->count;
		partial += !current->complete;
		slow += current->end_ns - current->begin_ns > WHY_SECOND / 5;
		gaps += why_history_latest(history)->sampling_gap;
		skipped += current->skipped;
		denied += current->denied;
		malformed += current->malformed;
		io_errors += current->io_errors;
		metadata_skipped += current->metadata_skipped;
		if (!synthetic && i + 1 < samples) {
			deadline += WHY_SECOND;
			uint64_t now = why_now_ns();
			if (now >= deadline)
				deadline += ((now - deadline) / WHY_SECOND + 1) * WHY_SECOND;
			while (!stopped && (now = why_now_ns()) < deadline) {
				uint64_t delta = deadline - now;
				struct timespec wait = {.tv_sec = (time_t)(delta / WHY_SECOND),
										.tv_nsec = (long)(delta % WHY_SECOND)};
				if (nanosleep(&wait, NULL) < 0 && errno != EINTR)
					goto cleanup;
			}
		}
	}
	uint64_t cpu_end = cpu_ns(), end = why_now_ns();
	WhyHistoryStats stats = why_history_stats(history);
	if (stats.bytes > stats.budget || stats.frames < 2)
		goto cleanup;
	uint64_t report_now = synthetic ? (uint64_t)samples * WHY_SECOND : end;
	size_t report_bytes = 0;
	for (unsigned i = 0; i < queries; ++i) {
		if (stopped)
			goto cleanup;
		FILE *out = fmemopen(buffer, REPORT_BYTES, "w");
		if (!out)
			goto cleanup;
		uint64_t start = why_now_ns();
		why_report_query(out, history, hz, 60, report_now);
		bool ok = fflush(out) == 0 && !ferror(out);
		long length = ftell(out);
		if (fclose(out) != 0)
			ok = false;
		query[i] = (double)(why_now_ns() - start) / 1e6;
		if (!ok || length < 0 || (size_t)length >= REPORT_BYTES)
			goto cleanup;
		report_bytes = (size_t)length;
	}
	WhyAttribution *totals =
		why_attribute(history, NULL, hz, WHY_ATTRIBUTION_CAPACITY);
	if (!totals)
		goto cleanup;
	/* Synthetic counters have exact CPU accounting; catches harness mistakes.
	 */
	if (synthetic &&
		(totals->residual_seconds > 1e-6 || totals->residual_seconds < -1e-6)) {
		why_attribution_destroy(totals);
		goto cleanup;
	}
	struct rusage usage;
	if (getrusage(RUSAGE_SELF, &usage) < 0) {
		why_attribution_destroy(totals);
		goto cleanup;
	}
	double rss = (double)usage.ru_maxrss;
#ifndef __APPLE__
	rss *= 1024;
#endif
	printf("{\n  \"schema\": 1, \"mode\": \"%s\", \"samples\": %u, "
		   "\"query_repetitions\": %u,\n",
		   synthetic ? "synthetic" : "live", samples, queries);
	printf("  \"entries_min\": %zu, \"entries_max\": %zu,\n", entries_min,
		   entries_max);
	distribution(synthetic ? "generate" : "collect", scan, samples);
	distribution("append", append, samples);
	distribution("render_60s", query, queries);
	printf("  \"recording_wall_seconds\": %.6f, \"recording_cpu_seconds\": "
		   "%.6f, \"recording_one_core_pct\": ",
		   (double)(end - begin) / 1e9, (double)(cpu_end - cpu_begin) / 1e9);
	if (synthetic)
		fputs("null,\n", stdout);
	else
		printf("%.6f,\n",
			   100.0 * (double)(cpu_end - cpu_begin) / (double)(end - begin));
	printf("  \"peak_rss_bytes\": %.0f, \"retained_seconds\": %.6f, "
		   "\"retained_frames\": %zu,\n",
		   rss, (double)(stats.newest_ns - stats.oldest_ns) / 1e9,
		   stats.frames);
	printf("  \"history_charged_bytes\": %zu, \"history_budget_bytes\": %zu, "
		   "\"evicted_frames\": %zu,\n",
		   stats.bytes, stats.budget, stats.evicted_frames);
	printf("  \"history_frame_bytes\": %zu, \"history_metadata_bytes\": %zu, "
		   "\"history_bookkeeping_bytes\": %zu, "
		   "\"history_metadata_allocations\": %zu,\n",
		   stats.frame_bytes, stats.metadata_bytes, stats.bookkeeping_bytes,
		   stats.metadata_allocations);
	printf("  \"gaps\": %zu, \"partial_scans\": %zu, \"slow_scans\": %zu, "
		   "\"skipped_entries\": %zu,\n",
		   gaps, partial, slow, skipped);
	printf("  \"denied_reads\": %zu, \"malformed_reads\": %zu, \"io_errors\": "
		   "%zu, \"metadata_skipped\": %zu,\n",
		   denied, malformed, io_errors, metadata_skipped);
	printf("  \"system_cpu_seconds\": %.6f, \"process_cpu_seconds\": %.6f, "
		   "\"signed_residual_seconds\": %.6f, \"report_bytes\": %zu\n}\n",
		   totals->system_seconds, totals->process_seconds,
		   totals->residual_seconds, report_bytes);
	why_attribution_destroy(totals);
	status = 0;
cleanup:
	if (status)
		fprintf(stderr, "Benchmark incomplete: %s%s\n",
				stopped ? "interrupted"
						: "allocation, collection or measurement failure",
				stopped ? "" : strerror(errno));
	free(buffer);
	why_history_destroy(history);
	why_frame_destroy(&frames[0]);
	why_frame_destroy(&frames[1]);
	return status;
usage:
	fputs("Usage: why_bench synthetic [processes=1000] [samples=360] "
		  "[queries=20]\n       why_bench live [samples=30] "
		  "[queries=20]\nBounds: processes 1..32768, samples 2..3600, queries "
		  "1..100.\n",
		  stderr);
	return 2;
}
