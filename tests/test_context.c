#include "context.h"
#include "report.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                                               \
	do {                                                                       \
		if (!(c)) {                                                            \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);            \
			exit(1);                                                           \
		}                                                                      \
	} while (0)

static void test_parents(void) {
	WhyHistory *h = why_history_create(300 * WHY_SECOND, 1000000, 16);
	CHECK(h);
	WhyProcess p[10] = {0};
	WhyFrame f = {
		.processes = p, .count = 10, .capacity = 10, .complete = true};
	for (size_t i = 0; i < 10; ++i) {
		p[i].id = (WhyIdentity){(uint32_t)i + 1, 1};
		p[i].status = WHY_READ_OK;
		p[i].end_ns = WHY_SECOND;
		strcpy(p[i].comm, i ? "worker" : "\033shell");
		if (i) {
			p[i].ppid = (uint32_t)i;
			p[i].parent_known = true;
			p[i].parent = p[i - 1].id;
		}
	}
	f.begin_ns = f.end_ns = f.boot_ns = WHY_SECOND;
	CHECK(why_history_append(h, &f, 100));
	const WhyHistoryEntry *original = why_history_first(h);
	const WhyProcess *child = &original->frame.processes[2];
	WhyParentChain chain = why_parent_chain(h, child);
	CHECK(chain.available && chain.count == 2 && chain.end == WHY_CHAIN_ROOT);
	CHECK(chain.ancestors[1]->id.pid == 1);
	chain = why_parent_chain(h, &original->frame.processes[9]);
	CHECK(chain.count == 8 && chain.end == WHY_CHAIN_LIMIT);
	chain = why_parent_chain(
		h, &p[2]); /* A live/external pointer is not a history view. */
	CHECK(!chain.available);
	p[1].id.start_ticks =
		2; /* Same PID, different identity: don't stitch it in. */
	f.begin_ns = f.end_ns = f.boot_ns = 2 * WHY_SECOND;
	CHECK(why_history_append(h, &f, 100));
	chain = why_parent_chain(h, &why_history_latest(h)->frame.processes[2]);
	CHECK(chain.count == 0 && chain.end == WHY_CHAIN_MISSING);
	chain = why_parent_chain(h, child);
	CHECK(chain.count == 2 && chain.ancestors[0]->id.start_ticks == 1);
	p[1].id.start_ticks = 1;
	p[1].parent = p[2].id;
	p[1].ppid = p[2].id.pid;
	f.begin_ns = f.end_ns = f.boot_ns = 3 * WHY_SECOND;
	CHECK(why_history_append(h, &f, 100));
	chain = why_parent_chain(h, &why_history_latest(h)->frame.processes[2]);
	CHECK(chain.count == 1 && chain.end == WHY_CHAIN_CYCLE);
	p[2].parent_known = false;
	f.begin_ns = f.end_ns = f.boot_ns = 4 * WHY_SECOND;
	CHECK(why_history_append(h, &f, 100));
	chain = why_parent_chain(h, &why_history_latest(h)->frame.processes[2]);
	CHECK(chain.count == 0 && chain.end == WHY_CHAIN_UNKNOWN);
	why_history_destroy(h);
}

static WhyHistory *timing_fixture(uint64_t ticks, unsigned first, bool gap,
								  bool partial, uint64_t retention,
								  uint64_t bracket) {
	WhyHistory *h = why_history_create(retention, 1000000, 4);
	CHECK(h);
	WhyProcess p = {.id = {42, ticks}, .status = WHY_READ_OK};
	strcpy(p.comm, "worker");
	WhyFrame f = {.processes = &p, .capacity = 1, .complete = true};
	f.system.cpus[0] = true;
	f.system.cpu_count = 1;
	for (unsigned t = 0; t <= 15; ++t) {
		f.begin_ns = f.boot_ns = (uint64_t)t * WHY_SECOND;
		f.end_ns = f.system.begin_ns = f.system.end_ns = p.begin_ns = p.end_ns =
			f.begin_ns + bracket;
		if (gap && t >= first)
			f.boot_ns += 5 * WHY_SECOND;
		f.count = t >= first && t < 15 ? 1 : 0;
		f.complete = !(partial && t == 15);
		f.system.ticks[0] = (uint64_t)t * 100;
		f.system.ticks[3] = (uint64_t)t * 700;
		CHECK(why_history_append(h, &f, 100));
	}
	return h;
}
static void test_timing(void) {
	WhyCpuIncident e = {.start_ns = 11 * WHY_SECOND,
						.end_ns = 14 * WHY_SECOND,
						.status = WHY_INCIDENT_RECOVERED};
	WhyHistory *h = timing_fixture(1050, 11, false, false, 300 * WHY_SECOND, 0);
	WhyTimingContext c =
		why_timing_context(h, (WhyIdentity){42, 1050}, &e, 100);
	CHECK(c.start_known && c.start_near_rise && c.loss_near_recovery);
	CHECK(c.start_earliest_ns == 10 * WHY_SECOND + WHY_SECOND / 2);
	CHECK(c.start_latest_ns >= c.start_earliest_ns + WHY_SECOND / 100);
	CHECK(c.loss_earliest_ns == 14 * WHY_SECOND &&
		  c.loss_latest_ns == 15 * WHY_SECOND);
	e.status = WHY_INCIDENT_ONGOING;
	c = why_timing_context(h, (WhyIdentity){42, 1050}, &e, 100);
	CHECK(!c.loss_near_recovery);
	e.status = WHY_INCIDENT_INTERRUPTED;
	c = why_timing_context(h, (WhyIdentity){42, 1050}, &e, 100);
	CHECK(!c.loss_near_recovery);
	c = why_timing_context(h, (WhyIdentity){42, 1051}, &e, 100);
	CHECK(!c.start_known && !c.loss_near_recovery);
	why_history_destroy(h);
	e.status = WHY_INCIDENT_RECOVERED;
	h = timing_fixture(100, 11, false, true, 300 * WHY_SECOND, 0);
	c = why_timing_context(h, (WhyIdentity){42, 100}, &e, 100);
	CHECK(c.start_known && !c.start_near_rise && !c.loss_near_recovery);
	why_history_destroy(h); /* First seen near rise is NOT birth near rise. */
	h = timing_fixture(1050, 11, true, false, 300 * WHY_SECOND, 0);
	c = why_timing_context(h, (WhyIdentity){42, 1050}, &e, 100);
	CHECK(!c.start_known && !c.start_near_rise);
	why_history_destroy(h);
	h = timing_fixture(1050, 11, false, false, 2 * WHY_SECOND, 0);
	c = why_timing_context(h, (WhyIdentity){42, 1050}, &e, 100);
	CHECK(!c.start_known && c.loss_near_recovery);
	why_history_destroy(h);
	h = timing_fixture(1299, 14, false, false, 300 * WHY_SECOND,
					   WHY_SECOND / 20);
	c = why_timing_context(h, (WhyIdentity){42, 1299}, &e, 100);
	CHECK(c.start_known &&
		  !c.start_near_rise); /* Uncertainty crosses the 2s boundary. */
	why_history_destroy(h);
	h = timing_fixture(UINT64_MAX, 11, false, false, 300 * WHY_SECOND, 0);
	c = why_timing_context(h, (WhyIdentity){42, UINT64_MAX}, &e, 100);
	CHECK(!c.start_known); /* Overflow/future clock values are not evidence. */
	why_history_destroy(h);
}
static void test_reporting(void) {
	WhyHistory *h = why_history_create(300 * WHY_SECOND, 1000000, 4);
	CHECK(h);
	WhyProcess p[2] = {
		{.id = {1, 1}},
		{.id = {2, 1}, .ppid = 1, .parent_known = true, .parent = {1, 1}}};
	strcpy(p[0].comm, "\033shell");
	strcpy(p[1].comm, "worker");
	WhyFrame f = {.processes = p, .count = 2, .capacity = 2, .complete = true};
	f.system.cpus[0] = true;
	f.system.cpu_count = 1;
	for (unsigned t = 1; t <= 2; ++t) {
		f.begin_ns = f.end_ns = f.boot_ns = f.system.begin_ns =
			f.system.end_ns = (uint64_t)t * WHY_SECOND;
		p[0].begin_ns = p[0].end_ns = p[1].begin_ns = p[1].end_ns = f.end_ns;
		p[1].user_ticks = (uint64_t)t * 100;
		f.system.ticks[0] = f.system.ticks[3] = (uint64_t)t * 100;
		CHECK(why_history_append(h, &f, 100));
	}
	char report[8192];
	FILE *out = fmemopen(report, sizeof report, "w");
	CHECK(out);
	why_report_query(out, h, 100, 2, 2 * WHY_SECOND);
	CHECK(fclose(out) == 0);
	CHECK(
		strstr(report, "parent snapshot at 1.000 s: <- 1:1 \\x1bshell [root]"));
	CHECK(!strchr(report, '\033'));
	CHECK(strstr(report, "system=1.000, processes=1.000"));
	why_history_destroy(h);
}

int main(void) {
	test_reporting();
	test_parents();
	test_timing();
	puts("Snapshot ancestry and conservative lifecycle correlation tests "
		 "passed.");
	return 0;
}
