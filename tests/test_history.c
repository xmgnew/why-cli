#include "history.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition)                                                       \
	do {                                                                       \
		if (!(condition)) {                                                    \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);    \
			exit(1);                                                           \
		}                                                                      \
	} while (0)

static WhyProcess process(uint32_t pid, uint64_t start, uint64_t at) {
	WhyProcess p = {.id = {pid, start},
					.begin_ns = at,
					.end_ns = at,
					.status = WHY_READ_OK};
	strcpy(p.comm, "worker");
	return p;
}

static WhyFrame frame(WhyProcess *processes, size_t count, unsigned tick,
					  bool complete) {
	uint64_t now = (uint64_t)tick * WHY_SECOND;
	for (size_t i = 0; i < count; ++i)
		processes[i].begin_ns = processes[i].end_ns = now;
	WhyFrame f = {.processes = processes,
				  .count = count,
				  .capacity = count,
				  .begin_ns = now,
				  .end_ns = now,
				  .boot_ns = now,
				  .complete = complete};
	f.system.begin_ns = f.system.end_ns = now;
	f.system.cpus[0] = true;
	f.system.cpu_count = 1;
	f.system.ticks[0] = tick * 20;
	f.system.ticks[3] = tick * 80;
	return f;
}

static bool has_event(const WhyHistoryEntry *e, WhyLifecycleType type,
					  uint64_t start) {
	for (size_t i = 0; i < e->event_count; ++i)
		if (e->events[i].type == type &&
			e->events[i].identity.start_ticks == start)
			return true;
	return false;
}

static void test_lifecycle(void) {
	WhyHistory *h = why_history_create(300 * WHY_SECOND, 1024 * 1024, 8);
	CHECK(h);
	WhyProcess p[] = {process(10, 100, 0)};
	WhyFrame f = frame(p, 1, 1, true);
	CHECK(why_history_append(h, &f, 100));
	CHECK(has_event(why_history_latest(h), WHY_FIRST_SEEN, 100));
	f = frame(p, 1, 2, true);
	p[0].status = WHY_DENIED;
	CHECK(why_history_append(h, &f, 100));
	CHECK(has_event(why_history_latest(h), WHY_VISIBILITY_LOST, 100));
	CHECK(!has_event(why_history_latest(h), WHY_NO_LONGER_OBSERVED, 100));
	f = frame(NULL, 0, 3, false);
	CHECK(why_history_append(h, &f, 100));
	CHECK(why_history_latest(h)->event_count == 0);
	p[0].status = WHY_READ_OK;
	p[0].metadata_changed = true;
	f = frame(p, 1, 4, true);
	CHECK(why_history_append(h, &f, 100));
	CHECK(has_event(why_history_latest(h), WHY_VISIBILITY_RESTORED, 100));
	CHECK(has_event(why_history_latest(h), WHY_METADATA_CHANGED, 100));
	CHECK(why_history_latest(h)->event_count == 2);
	CHECK(!has_event(why_history_latest(h), WHY_FIRST_SEEN, 100));
	p[0].metadata_changed = false;
	p[0].id.start_ticks = 200;
	f = frame(p, 1, 5, true);
	CHECK(why_history_append(h, &f, 100));
	CHECK(has_event(why_history_latest(h), WHY_NO_LONGER_OBSERVED, 100));
	CHECK(has_event(why_history_latest(h), WHY_FIRST_SEEN, 200));
	const WhyLifecycle *gone = &why_history_latest(h)->events[0];
	CHECK(gone->earliest_ns == 4 * WHY_SECOND &&
		  gone->latest_ns == 5 * WHY_SECOND);
	f = frame(p, 1, 6, true);
	p[0].metadata_changed = true;
	CHECK(why_history_append(h, &f, 100));
	CHECK(has_event(why_history_latest(h), WHY_METADATA_CHANGED, 200));
	f = frame(NULL, 0, 7, true);
	CHECK(why_history_append(h, &f, 100));
	CHECK(has_event(why_history_latest(h), WHY_NO_LONGER_OBSERVED, 200));
	f = frame(p, 1, 10, true);
	p[0].metadata_changed = false;
	CHECK(why_history_append(h, &f, 100));
	CHECK(why_history_latest(h)->sampling_gap);
	CHECK(!why_history_append(h, &f, 100) && errno == EINVAL);
	f = frame(p, 1, 11, false);
	p[0].status = WHY_GONE;
	CHECK(why_history_append(h, &f, 100));
	CHECK(has_event(why_history_latest(h), WHY_NO_LONGER_OBSERVED, 200));
	why_history_destroy(h);
}

static void test_retention(void) {
	WhyHistory *h = why_history_create(2 * WHY_SECOND, 1024 * 1024, 8);
	CHECK(h);
	WhyProcess p[] = {process(10, 100, 0)};
	for (unsigned tick = 1; tick <= 100; ++tick) {
		WhyFrame f = frame(p, 1, tick, true);
		CHECK(why_history_append(h, &f, 100));
		CHECK(why_history_stats(h).bytes <= why_history_stats(h).budget);
	}
	WhyHistoryStats stats = why_history_stats(h);
	CHECK(stats.frames == 3 && stats.evicted_frames == 97);
	CHECK(why_history_first(h)->frame.end_ns == 98 * WHY_SECOND);
	CHECK(why_history_latest(h)->event_count ==
		  0); /* identity survives frame eviction */
	why_history_destroy(h);

	h = why_history_create(300 * WHY_SECOND, (sizeof(WhyCpuDetector) + 40000),
						   2);
	CHECK(h);
	for (unsigned tick = 1; tick <= 20; ++tick) {
		WhyFrame f = frame(p, 1, tick, true);
		CHECK(why_history_append(h, &f, 100));
		CHECK(why_history_stats(h).bytes <= (sizeof(WhyCpuDetector) + 40000));
	}
	CHECK(why_history_stats(h).evicted_frames > 0);
	why_history_destroy(h);

	h = why_history_create(300 * WHY_SECOND, (sizeof(WhyCpuDetector) + 1024),
						   1);
	CHECK(h);
	WhyFrame f = frame(p, 1, 1, true);
	CHECK(!why_history_append(h, &f, 100) && errno == ENOSPC);
	CHECK(why_history_stats(h).frames == 0);
	why_history_destroy(h);

	h = why_history_create(300 * WHY_SECOND, 100000, 1);
	CHECK(h);
	WhyProcess many[] = {process(10, 100, 0), process(20, 200, 0)};
	f = frame(many, 2, 1, true);
	CHECK(why_history_append(h, &f, 100));
	CHECK(why_history_stats(h).tracking_dropped == 1);
	CHECK(why_history_latest(h)->tracking_dropped == 1);
	f = frame(NULL, 0, 2, false);
	CHECK(why_history_append(h, &f, 100));
	CHECK(!has_event(why_history_latest(h), WHY_NO_LONGER_OBSERVED, 100));
	f = frame(&many[1], 1, 3, true);
	CHECK(why_history_append(h, &f, 100));
	CHECK(has_event(why_history_latest(h), WHY_FIRST_SEEN, 200));
	CHECK(has_event(why_history_latest(h), WHY_NO_LONGER_OBSERVED, 100));
	why_history_destroy(h);

	h = why_history_create(5 * WHY_SECOND, 100000, 2);
	CHECK(h);
	f = frame(p, 1, 1, true);
	CHECK(why_history_append(h, &f, 100));
	f = frame(p, 1, 2, true);
	f.boot_ns += 1000 * WHY_SECOND;
	CHECK(why_history_append(h, &f, 100));
	CHECK(why_history_stats(h).frames == 1);
	CHECK(why_history_latest(h)->sampling_gap);
	why_history_destroy(h);
}

static void write_bytes(int directory, const char *name, const void *bytes,
						size_t length) {
	int fd = openat(directory, name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	CHECK(fd >= 0);
	CHECK(write(fd, bytes, length) == (ssize_t)length);
	CHECK(close(fd) == 0);
}

static void test_metadata(void) {
	char root[] = "why-metadata-XXXXXX";
	CHECK(mkdtemp(root));
	int directory = open(root, O_RDONLY | O_DIRECTORY);
	CHECK(directory >= 0);
	const char *stat =
		"42 (worker) S 1 0 0 0 0 0 0 0 0 0 120 80 0 0 20 0 1 0 100\n";
	write_bytes(directory, "stat", stat, strlen(stat));
	const char args[] = "worker\0arg with space\0\033unsafe\0";
	write_bytes(directory, "cmdline", args, sizeof args - 1);
	/* Only link text is observed; no target is opened. */
	CHECK(symlinkat("fixture-working-directory", directory, "cwd") == 0);
	CHECK(symlinkat("fixture-executable", directory, "exe") == 0);
	WhyProcess a = process(42, 100, why_now_ns());
	size_t budget = WHY_METADATA_FRAME_BUDGET;
	CHECK(why_collect_metadata(directory, &a, NULL, &budget));
	CHECK(a.metadata && a.metadata->verified && !a.metadata_changed);
	const WhyMetadataField *cmd = &a.metadata->fields[WHY_CMDLINE];
	CHECK(cmd->length == sizeof args - 1);
	CHECK(!memcmp(a.metadata->data + cmd->offset, args, sizeof args - 1));
	WhyProcess b = process(42, 100, why_now_ns());
	CHECK(why_collect_metadata(directory, &b, &a, &budget));
	CHECK(a.metadata == b.metadata && a.metadata->references == 2);
	why_metadata_release(b.metadata);
	b.metadata = NULL;
	/* Change comm to force the documented early refresh without sleeping. */
	strcpy(b.comm, "renamed");
	write_bytes(directory, "cmdline", "new\0", 4);
	CHECK(why_collect_metadata(directory, &b, &a, &budget));
	CHECK(b.metadata != a.metadata && b.metadata_changed);
	CHECK(a.metadata->fields[WHY_CMDLINE].length == sizeof args - 1);

	/* Snapshots own references; caller destruction cannot invalidate history.
	 */
	WhyHistory *h = why_history_create(10 * WHY_SECOND, 200000, 4);
	CHECK(h);
	WhyProcess pair[] = {a, b};
	pair[1].id.pid = 43;
	pair[1].id.start_ticks = 101;
	pair[1].ppid = 42;
	WhyFrame f = frame(pair, 2, 1, true);
	why_resolve_parents(&f);
	CHECK(pair[1].parent_metadata == a.metadata);
	CHECK(why_history_append(h, &f, 100));
	why_metadata_release(pair[1].parent_metadata);
	why_metadata_release(a.metadata);
	why_metadata_release(b.metadata);
	a.metadata = b.metadata = NULL;
	const WhyProcess *saved = &why_history_first(h)->frame.processes[1];
	CHECK(saved->parent_metadata &&
		  saved->parent_metadata->fields[WHY_CMDLINE].length ==
			  sizeof args - 1);
	why_history_destroy(h);

	WhyProcess wrong = process(42, 999, why_now_ns());
	CHECK(why_collect_metadata(directory, &wrong, NULL, &budget));
	CHECK(!wrong.metadata && wrong.metadata_status == WHY_FIELD_UNVERIFIED);
	budget = 0;
	CHECK(why_collect_metadata(directory, &a, NULL, &budget));
	CHECK(!a.metadata && a.metadata_status == WHY_FIELD_BUDGET);
	unsigned char large[WHY_METADATA_LIMIT + 1];
	memset(large, 'x', sizeof large);
	write_bytes(directory, "cmdline", large, sizeof large);
	budget = WHY_METADATA_FRAME_BUDGET;
	CHECK(why_collect_metadata(directory, &a, NULL, &budget));
	CHECK(a.metadata->fields[WHY_CMDLINE].truncated);
	CHECK(a.metadata->fields[WHY_CMDLINE].length == WHY_METADATA_LIMIT);
	why_metadata_release(a.metadata);
	a.metadata = NULL;
	write_bytes(directory, "cmdline", large, WHY_METADATA_LIMIT);
	CHECK(why_collect_metadata(directory, &a, NULL, &budget));
	CHECK(!a.metadata->fields[WHY_CMDLINE].truncated);
	why_metadata_release(a.metadata);
	a.metadata = NULL;
	write_bytes(directory, "cmdline", "", 0);
	CHECK(unlinkat(directory, "cwd", 0) == 0);
	CHECK(why_collect_metadata(directory, &a, NULL, &budget));
	CHECK(a.metadata->fields[WHY_CMDLINE].status == WHY_FIELD_EMPTY);
	CHECK(a.metadata->fields[WHY_CWD].status == WHY_FIELD_MISSING);
	why_metadata_release(a.metadata);
	a.metadata = NULL;
	if (geteuid() != 0) {
		CHECK(fchmodat(directory, "cmdline", 0000, 0) == 0);
		CHECK(why_collect_metadata(directory, &a, NULL, &budget));
		CHECK(a.metadata->fields[WHY_CMDLINE].status == WHY_FIELD_DENIED);
		why_metadata_release(a.metadata);
		a.metadata = NULL;
		CHECK(fchmodat(directory, "cmdline", 0600, 0) == 0);
	}
	CHECK(why_collect_metadata(directory, &a, NULL, &budget));
	/* Expire a fixture timestamp; no wall-clock sleeps in regression tests. */
	a.metadata->end_ns = 0;
	b = process(42, 100, why_now_ns());
	CHECK(why_collect_metadata(directory, &b, &a, &budget));
	CHECK(b.metadata != a.metadata && !b.metadata_changed);
	why_metadata_release(a.metadata);
	why_metadata_release(b.metadata);
	CHECK(unlinkat(directory, "cmdline", 0) == 0);
	CHECK(unlinkat(directory, "exe", 0) == 0);
	CHECK(unlinkat(directory, "stat", 0) == 0);
	CHECK(close(directory) == 0 && rmdir(root) == 0);
}

static WhyMetadata *metadata(size_t size) {
	WhyMetadata *m = calloc(1, sizeof *m + size);
	CHECK(m);
	m->references = 1;
	m->allocation_bytes = sizeof *m + size;
	m->verified = true;
	if (size)
		m->data[0] = 'x';
	return m;
}

static void accounting(const WhyHistory *h) {
	WhyHistoryStats stats = why_history_stats(h);
	CHECK(stats.bytes <= stats.budget);
	CHECK(stats.bytes ==
		  stats.frame_bytes + stats.metadata_bytes + stats.bookkeeping_bytes);
	size_t frames = 0;
	for (const WhyHistoryEntry *e = why_history_first(h); e; e = e->next) {
		CHECK(e->charged_bytes == sizeof *e +
									  e->frame.count * sizeof(WhyProcess) +
									  e->event_count * sizeof(WhyLifecycle));
		CHECK((e->events != NULL) == (e->event_count != 0));
		frames += e->charged_bytes;
	}
	CHECK(frames == stats.frame_bytes);
}

static void test_shared_accounting(void) {
	WhyHistory *a = why_history_create(2 * WHY_SECOND, 200000, 2);
	WhyHistory *b = why_history_create(300 * WHY_SECOND, 200000, 2);
	CHECK(a && b);
	WhyMetadata *m = metadata(4096);
	WhyProcess p = process(10, 100, 0);
	p.metadata = p.parent_metadata = m;
	why_metadata_retain(m); /* Two caller-owned references to one allocation. */
	WhyFrame f = frame(&p, 1, 1, true);
	CHECK(why_history_append(a, &f, 100));
	CHECK(why_history_append(b, &f, 100));
	f = frame(&p, 1, 2, true);
	CHECK(why_history_append(a, &f, 100));
	CHECK(m->references == 8);
	CHECK(why_history_stats(a).metadata_allocations == 1);
	CHECK(why_history_stats(a).metadata_bytes == m->allocation_bytes);
	CHECK(why_history_stats(b).metadata_bytes == m->allocation_bytes);
	CHECK(why_history_latest(a)->event_count == 0 &&
		  !why_history_latest(a)->events);
	accounting(a);
	accounting(b);
	/* Same content in a distinct snapshot must be separately charged. */
	WhyMetadata *replacement = metadata(4096);
	p.metadata = replacement;
	p.metadata_changed = true;
	f = frame(&p, 1, 3, true);
	CHECK(why_history_append(a, &f, 100));
	CHECK(why_history_stats(a).metadata_allocations == 2);
	CHECK(why_history_stats(a).metadata_bytes ==
		  m->allocation_bytes + replacement->allocation_bytes);
	CHECK(has_event(why_history_latest(a), WHY_METADATA_CHANGED, 100));
	accounting(a);
	f = frame(NULL, 0, 6, true);
	CHECK(why_history_append(a, &f, 100));
	CHECK(why_history_stats(a).metadata_allocations == 0);
	CHECK(why_history_stats(a).metadata_bytes == 0);
	CHECK(m->references == 4 && replacement->references == 1);
	CHECK(why_history_first(b)->frame.processes[0].metadata->data[0] == 'x');
	accounting(a);
	accounting(b);
	why_history_destroy(a);
	why_history_destroy(b);
	CHECK(m->references == 2);
	why_metadata_release(m);
	why_metadata_release(m);
	why_metadata_release(replacement);
}

static void test_accounting_rollback(void) {
	WhyHistory *h = why_history_create(300 * WHY_SECOND, 50000, 1);
	CHECK(h);
	size_t base = why_history_stats(h).bytes;
	WhyMetadata *small = metadata(128), *huge = metadata(45000);
	WhyProcess p[2] = {process(10, 100, 0), process(20, 200, 0)};
	p[0].metadata = small;
	p[1].metadata = huge;
	WhyFrame f = frame(p, 2, 1, true);
	CHECK(!why_history_append(h, &f, 100) && errno == ENOSPC);
	CHECK(why_history_stats(h).bytes == base);
	CHECK(why_history_stats(h).metadata_allocations == 0);
	CHECK(why_history_stats(h).tracking_dropped == 0);
	CHECK(small->references == 1 && huge->references == 1);
	CHECK(why_history_detector(h)->baseline_count == 0);
	accounting(h);
	f = frame(p, 1, 1, true);
	CHECK(why_history_append(h, &f, 100));
	CHECK(has_event(why_history_latest(h), WHY_FIRST_SEEN, 100));
	unsigned char before_failure[sizeof(WhyCpuDetector)];
	memcpy(before_failure, why_history_detector(h), sizeof before_failure);
	f = frame(p, 2, 2, true);
	CHECK(!why_history_append(h, &f, 100) && errno == ENOSPC);
	CHECK(why_history_stats(h).frames == 0);
	CHECK(small->references == 1);
	CHECK(!memcmp(before_failure, why_history_detector(h),
				  sizeof before_failure));
	CHECK(why_history_stats(h).tracking_dropped == 0);
	accounting(h);
	/* A further impossible snapshot must leave the emptied ledger consistent.
	 */
	p[0].metadata = huge;
	f = frame(p, 1, 3, true);
	CHECK(!why_history_append(h, &f, 100) && errno == ENOSPC);
	accounting(h);
	p[0].metadata = small;
	for (unsigned tick = 4; tick <= 100; ++tick) {
		f = frame(p, 1, tick, true);
		CHECK(why_history_append(h, &f, 100));
		CHECK(why_history_latest(h)->event_count == 0);
		CHECK(why_history_stats(h).metadata_allocations == 1);
		accounting(h);
	}
	CHECK(why_history_stats(h).evicted_frames > 0);
	CHECK(small->references == 1 + why_history_stats(h).frames);
	why_history_destroy(h);
	CHECK(small->references == 1 && huge->references == 1);
	why_metadata_release(small);
	why_metadata_release(huge);
}

static void test_metadata_churn(void) {
	WhyHistory *h = why_history_create(300 * WHY_SECOND, 100000, 1);
	CHECK(h);
	WhyProcess p = process(10, 100, 0);
	for (unsigned tick = 1; tick <= 80; ++tick) {
		p.metadata = metadata(128);
		p.metadata_changed = tick > 1;
		WhyFrame f = frame(&p, 1, tick, true);
		CHECK(why_history_append(h, &f, 100));
		CHECK(why_history_stats(h).metadata_allocations ==
			  why_history_stats(h).frames);
		accounting(h);
		why_metadata_release(p.metadata);
	}
	CHECK(why_history_stats(h).evicted_frames > 0);
	why_history_destroy(h);
}

int main(void) {
	test_shared_accounting();
	test_accounting_rollback();
	test_metadata_churn();
	test_lifecycle();
	test_retention();
	test_metadata();
	puts("Metadata, lifecycle, ownership and retention checks passed.");
	return 0;
}
