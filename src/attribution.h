#ifndef WHY_ATTRIBUTION_H
#define WHY_ATTRIBUTION_H

#include "history.h"

/* Reserved from the CLI recording budget; includes all query arrays. */
#define WHY_ATTRIBUTION_BYTES (16U * 1024U * 1024U)
#define WHY_ATTRIBUTION_CAPACITY WHY_MAX_PROCESSES

typedef enum {
	WHY_ATTR_PROVISIONAL = 1U << 0,
	WHY_ATTR_TRUNCATED = 1U << 1,
	WHY_ATTR_GAP = 1U << 2,
	WHY_ATTR_PARTIAL_SCAN = 1U << 3,
	WHY_ATTR_SLOW_SCAN = 1U << 4,
	WHY_ATTR_LOW_COVERAGE = 1U << 5,
	WHY_ATTR_ACCOUNTING = 1U << 6,
	WHY_ATTR_UNKNOWN_BASELINE = 1U << 7,
	WHY_ATTR_CAPACITY = 1U << 8
} WhyAttributionIssue;

typedef struct {
	WhyIdentity identity;
	const WhyProcess *context; /* Borrowed from immutable history. */
	double cpu_seconds, excess_seconds, covered_seconds;
	size_t members, intervals;
	bool baseline_known, weak_group, likely, context_stable;
} WhyContributor;

typedef struct {
	WhyContributor
		*rows; /* total-CPU order; increment_order indexes these rows */
	size_t *increment_order;
	size_t count, identities, dropped, allocation_bytes;
	uint64_t start_ns, end_ns;
	double system_seconds, process_seconds, excess_seconds,
		system_covered_seconds;
	double residual_seconds, accounted_ratio, coverage_ratio;
	unsigned issues;
	bool has_baseline;
} WhyAttribution;

/* NULL incident means total activity over retained system intervals. No history
 * mutation is permitted until the result is destroyed. A bounded identity table
 * reports overflow explicitly, never silently renormalizing the retained rows.
 */
WhyAttribution *why_attribute(const WhyHistory *history,
							  const WhyCpuIncident *incident, long hz,
							  size_t capacity);
/* Total activity within an explicit window; no inferred baseline. Missing
 * boundaries remain flagged rather than silently expanding the window. */
WhyAttribution *why_attribute_window(const WhyHistory *history,
									 uint64_t start_ns, uint64_t end_ns,
									 long hz, size_t capacity);
void why_attribution_destroy(WhyAttribution *result);

#endif
