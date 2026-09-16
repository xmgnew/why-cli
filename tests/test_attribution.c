#include "attribution.h"
#include "report.h"

#include <math.h>
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

typedef enum {
	BASE_GAP,
	TRUNCATED,
	NO_DATA,
	NORMAL,
	GROUP,
	DISTINCT_PARENTS,
	UNKNOWN_BASE,
	NEW_PROCESS,
	MISSING_READ,
	SLOW,
	PARTIAL,
	OVER_ACCOUNTED,
	CHANGED_NAME,
	STRONG_GROUP,
	DIFFERENT_EXE,
	PARTIAL_GROUP
} Scenario;

static WhyCpuIncident incident(void) {
	return (WhyCpuIncident){.start_ns = 11 * WHY_SECOND,
							.end_ns = 14 * WHY_SECOND,
							.baseline_start_ns = WHY_SECOND,
							.baseline_end_ns = 11 * WHY_SECOND,
							.cpu_seconds = 21,
							.status = WHY_INCIDENT_RECOVERED};
}
static WhyHistory *fixture(Scenario scenario) {
	WhyHistory *h = why_history_create(
		(scenario == TRUNCATED ? 4 : 300) * WHY_SECOND, 8 * 1024 * 1024, 16);
	CHECK(h);
	WhyProcess p[3] = {0};
	for (size_t i = 0; i < 3; ++i) {
		p[i].id = (WhyIdentity){(uint32_t)(10 + i), 1};
		strcpy(p[i].comm, i ? "worker" : "service");
	}
	bool grouped = scenario == GROUP || scenario == DISTINCT_PARENTS ||
				   scenario == STRONG_GROUP || scenario == DIFFERENT_EXE ||
				   scenario == PARTIAL_GROUP;
	if (scenario == STRONG_GROUP || scenario == DIFFERENT_EXE) {
		for (size_t i = 1; i < 3; ++i) {
			const char *path = scenario == DIFFERENT_EXE && i == 2
								   ? "/fixture/other"
								   : "/fixture/worker";
			size_t bytes = sizeof(WhyMetadata) + strlen(path) + 1;
			p[i].metadata = calloc(1, bytes);
			CHECK(p[i].metadata);
			p[i].metadata->references = 1;
			p[i].metadata->allocation_bytes = bytes;
			p[i].metadata->verified = true;
			p[i].metadata->fields[WHY_EXE].length = strlen(path);
			memcpy(p[i].metadata->data, path, strlen(path) + 1);
		}
	}
	WhyFrame f = {.processes = p, .capacity = 3, .complete = true};
	f.system.cpus[0] = true;
	f.system.cpu_count = 1;
	for (unsigned t = 0; t <= 14; ++t) {
		uint64_t now = (uint64_t)t * WHY_SECOND;
		f.begin_ns = f.end_ns = f.boot_ns = now;
		f.system.begin_ns = f.system.end_ns = now;
		if (t) {
			unsigned busy = scenario == OVER_ACCOUNTED ? 10
							: t > 11				   ? 700
													   : 400;
			f.system.ticks[0] += busy;
			f.system.ticks[3] += 800 - busy;
			p[0].user_ticks += 400;
			p[1].user_ticks += t > 11 ? (grouped ? 150 : 300) : 0;
			p[2].user_ticks += t > 11 ? 150 : 0;
		}
		if (scenario == BASE_GAP)
			f.system.ticks[4] = t == 4 ? 1 : 0;
		if (scenario == NO_DATA)
			f.system.ticks[0] = f.system.ticks[3] = 0;
		f.count = grouped ? 3 : 2;
		if (scenario == PARTIAL_GROUP && t < 10)
			f.count = 2;
		if ((scenario == UNKNOWN_BASE && t < 10) ||
			(scenario == NEW_PROCESS && t < 12))
			f.count = 1;
		for (size_t i = 0; i < 3; ++i) {
			p[i].begin_ns = p[i].end_ns = now;
			p[i].status = WHY_READ_OK;
			if (i && grouped) {
				p[i].parent_known = true;
				p[i].parent = (WhyIdentity){
					scenario == DISTINCT_PARENTS ? (uint32_t)(90 + i) : 99, 1};
			}
		}
		if (scenario == NEW_PROCESS)
			p[1].id.start_ticks = 1150;
		if (scenario == MISSING_READ && t == 12)
			p[1].status = WHY_DENIED;
		if (scenario == SLOW)
			f.end_ns += WHY_SECOND / 4;
		if (scenario == PARTIAL && t == 12)
			f.complete = false;
		if (scenario == CHANGED_NAME && t >= 13)
			strcpy(p[1].comm, "renamed");
		CHECK(why_history_append(h, &f, 100));
	}
	for (size_t i = 1; i < 3; ++i)
		why_metadata_release(p[i].metadata);
	return h;
}
static const WhyContributor *by_pid(const WhyAttribution *r, uint32_t pid) {
	for (size_t i = 0; i < r->count; ++i)
		if (r->rows[i].identity.pid == pid)
			return &r->rows[i];
	return NULL;
}
static void test_ranking(void) {
	WhyHistory *h = fixture(NORMAL);
	WhyCpuIncident e = incident();
	WhyAttribution *r = why_attribute(h, &e, 100, 16);
	CHECK(r && r->count == 2 && !r->issues);
	CHECK(r->rows[0].identity.pid == 10 &&
		  fabs(r->rows[0].cpu_seconds - 12) < 1e-9);
	CHECK(r->rows[r->increment_order[0]].identity.pid == 11);
	CHECK(fabs(by_pid(r, 11)->excess_seconds - 9) < 1e-9);
	CHECK(by_pid(r, 11)->likely && !by_pid(r, 10)->likely);
	CHECK(r->system_seconds == 21 && r->process_seconds == 21 &&
		  r->residual_seconds == 0);
	CHECK(r->allocation_bytes <= WHY_ATTRIBUTION_BYTES);
	why_attribution_destroy(r);
	r = why_attribute(h, NULL, 100, 16);
	CHECK(r && !r->has_baseline && !r->rows[0].likely);
	why_attribution_destroy(r);
	r = why_attribute(h, &e, 100, 1);
	CHECK(r && r->dropped && (r->issues & WHY_ATTR_CAPACITY));
	CHECK(!r->rows[0].likely);
	why_attribution_destroy(r);
	e.status = WHY_INCIDENT_ONGOING;
	r = why_attribute(h, &e, 100, 16);
	CHECK(r && (r->issues & WHY_ATTR_PROVISIONAL) && !by_pid(r, 11)->likely);
	why_attribution_destroy(r);
	why_history_destroy(h);
}
static void test_grouping(void) {
	WhyCpuIncident e = incident();
	WhyHistory *h = fixture(GROUP);
	WhyAttribution *r = why_attribute(h, &e, 100, 16);
	CHECK(r && r->count == 2);
	const WhyContributor *worker = by_pid(r, 11);
	CHECK(worker && worker->members == 2 && worker->weak_group &&
		  worker->likely);
	CHECK(worker->cpu_seconds == 9 && r->process_seconds == 21);
	why_attribution_destroy(r);
	why_history_destroy(h);
	h = fixture(DISTINCT_PARENTS);
	r = why_attribute(h, &e, 100, 16);
	CHECK(r && r->count == 3);
	CHECK(!by_pid(r, 11)->likely &&
		  !by_pid(r, 12)->likely); /* exact tie: no arbitrary winner */
	why_attribution_destroy(r);
	why_history_destroy(h);
	h = fixture(STRONG_GROUP);
	r = why_attribute(h, &e, 100, 16);
	CHECK(r && r->count == 2 && by_pid(r, 11)->members == 2 &&
		  !by_pid(r, 11)->weak_group);
	why_attribution_destroy(r);
	why_history_destroy(h);
	h = fixture(DIFFERENT_EXE);
	r = why_attribute(h, &e, 100, 16);
	CHECK(r && r->count == 3);
	why_attribution_destroy(r);
	why_history_destroy(h);
	h = fixture(PARTIAL_GROUP);
	r = why_attribute(h, &e, 100, 16);
	CHECK(r && r->count == 2 && !by_pid(r, 11)->baseline_known &&
		  !by_pid(r, 11)->likely);
	why_attribution_destroy(r);
	why_history_destroy(h);
	h = fixture(CHANGED_NAME);
	r = why_attribute(h, &e, 100, 16);
	CHECK(r && !by_pid(r, 11)->context_stable && !by_pid(r, 11)->likely);
	why_attribution_destroy(r);
	why_history_destroy(h);
}
static void test_quality(void) {
	const Scenario scenarios[] = {UNKNOWN_BASE, MISSING_READ,	SLOW,
								  PARTIAL,		OVER_ACCOUNTED, BASE_GAP,
								  TRUNCATED,	NO_DATA};
	const unsigned flags[] = {WHY_ATTR_UNKNOWN_BASELINE, WHY_ATTR_LOW_COVERAGE,
							  WHY_ATTR_SLOW_SCAN,		 WHY_ATTR_PARTIAL_SCAN,
							  WHY_ATTR_ACCOUNTING,		 WHY_ATTR_GAP,
							  WHY_ATTR_TRUNCATED,		 WHY_ATTR_GAP};
	WhyCpuIncident e = incident();
	for (size_t i = 0; i < sizeof scenarios / sizeof *scenarios; ++i) {
		WhyHistory *h = fixture(scenarios[i]);
		WhyAttribution *r = why_attribute(h, &e, 100, 16);
		CHECK(r && (r->issues & flags[i]));
		for (size_t j = 0; j < r->count; ++j)
			CHECK(!r->rows[j].likely);
		if (scenarios[i] == NO_DATA)
			CHECK(r->system_covered_seconds == 0);
		if (scenarios[i] == UNKNOWN_BASE)
			CHECK(!by_pid(r, 11)->baseline_known);
		if (scenarios[i] == OVER_ACCOUNTED)
			CHECK(r->residual_seconds < 0 && r->accounted_ratio > 1.05);
		why_attribution_destroy(r);
		why_history_destroy(h);
	}
	WhyHistory *h = fixture(NEW_PROCESS);
	WhyAttribution *r = why_attribute(h, &e, 100, 16);
	CHECK(r && by_pid(r, 11)->baseline_known);
	CHECK(by_pid(r, 11)->cpu_seconds == 6 &&
		  by_pid(r, 11)->excess_seconds == 6);
	CHECK(by_pid(r, 11)->likely);
	why_attribution_destroy(r);
	why_history_destroy(h);
}
static void test_bin_increment(void) {
	WhyHistory *h = why_history_create(300 * WHY_SECOND, 1000000, 4);
	CHECK(h);
	WhyProcess p = {.id = {42, 1}};
	strcpy(p.comm, "worker");
	WhyFrame f = {.processes = &p, .count = 1, .capacity = 1, .complete = true};
	f.system.cpus[0] = true;
	for (unsigned t = 0; t <= 5; ++t) {
		f.begin_ns = f.boot_ns = f.system.begin_ns = f.system.end_ns =
			(uint64_t)t * WHY_SECOND;
		p.begin_ns = p.end_ns = f.end_ns = f.begin_ns + WHY_SECOND / 4;
		if (t) {
			p.user_ticks += t == 4 ? 200 : t == 5 ? 0 : 100;
			f.system.ticks[0] += 100;
			f.system.ticks[3] += 700;
		}
		CHECK(why_history_append(h, &f, 100));
	}
	WhyCpuIncident e = {.baseline_start_ns = WHY_SECOND,
						.baseline_end_ns = 3 * WHY_SECOND,
						.start_ns = 3 * WHY_SECOND,
						.end_ns = 5 * WHY_SECOND,
						.status = WHY_INCIDENT_RECOVERED};
	WhyAttribution *r = why_attribute(h, &e, 100, 8);
	CHECK(r && r->count == 1);
	CHECK(fabs(r->rows[0].cpu_seconds - 2.25) < 1e-9);
	CHECK(fabs(r->rows[0].excess_seconds - 0.75) < 1e-9);
	CHECK(r->issues & WHY_ATTR_SLOW_SCAN);
	why_attribution_destroy(r);
	why_history_destroy(h);
}

static void test_query_window(void) {
	WhyHistory *h = fixture(NORMAL);
	WhyAttribution *r = why_attribute_window(
		h, 12 * WHY_SECOND + WHY_SECOND / 2, 14 * WHY_SECOND, 100, 16);
	CHECK(r && !r->has_baseline);
	CHECK(fabs(r->process_seconds - 10.5) < 1e-9);
	CHECK(fabs(r->system_seconds - 10.5) < 1e-9);
	CHECK(fabs(by_pid(r, 10)->cpu_seconds - 6) < 1e-9);
	CHECK(fabs(by_pid(r, 11)->cpu_seconds - 4.5) < 1e-9);
	why_attribution_destroy(r);
	CHECK(!why_attribute_window(h, 14 * WHY_SECOND, 14 * WHY_SECOND, 100, 16));
	r = why_attribute_window(h, 14 * WHY_SECOND, 16 * WHY_SECOND, 100, 16);
	CHECK(r && r->system_covered_seconds == 0 &&
		  (r->issues & WHY_ATTR_TRUNCATED));
	why_attribution_destroy(r);
	char report[16384];
	FILE *out = fmemopen(report, sizeof report, "w");
	CHECK(out);
	why_report_query(out, h, 100, 2, 14 * WHY_SECOND);
	CHECK(fclose(out) == 0);
	CHECK(strstr(report, "Query window: 12.000..14.000"));
	CHECK(strstr(report, "system=14.000, processes=14.000"));
	CHECK(strstr(report, "FULL EVENT results"));
	CHECK(strstr(report, "Context for the leading rows"));
	CHECK(strstr(report,
				 "timing correlations do not prove cause or change rankings"));
	CHECK(strstr(report, "CPU spike 11.000..14.000"));
	out = fmemopen(report, sizeof report, "w");
	CHECK(out);
	why_report_query(out, h, 100, 2, 20 * WHY_SECOND);
	CHECK(fclose(out) == 0);
	CHECK(strstr(report, "No comparable observations"));
	why_history_destroy(h);
}

int main(void) {
	test_query_window();
	test_ranking();
	test_grouping();
	test_quality();
	test_bin_increment();
	puts("Total/increment ranking, grouping and quality-gate tests passed.");
	return 0;
}
