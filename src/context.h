#ifndef WHY_CONTEXT_H
#define WHY_CONTEXT_H
#include "history.h"

#define WHY_ANCESTOR_LIMIT 8U

typedef enum {
	WHY_CHAIN_ROOT,
	WHY_CHAIN_UNKNOWN,
	WHY_CHAIN_MISSING,
	WHY_CHAIN_CYCLE,
	WHY_CHAIN_LIMIT
} WhyChainEnd;

typedef struct {
	const WhyProcess *ancestors[WHY_ANCESTOR_LIMIT];
	size_t count;
	uint64_t observed_ns;
	WhyChainEnd end;
	bool available;
} WhyParentChain;

typedef struct {
	uint64_t start_earliest_ns, start_latest_ns;
	uint64_t loss_earliest_ns, loss_latest_ns;
	bool start_known, start_near_rise, loss_near_recovery;
} WhyTimingContext;

/* All pointers are borrowed from one retained frame, never a stitched ancestry.
 * Destroy/use results before history mutation; no ownership or CPU aggregation.
 */
WhyParentChain why_parent_chain(const WhyHistory *history,
								const WhyProcess *process);
/* Evidence only: these flags never change attribution scores or label gates.
 * Unknown birth clocks and missing lifecycle events remain unknown. */
WhyTimingContext why_timing_context(const WhyHistory *history,
									WhyIdentity identity,
									const WhyCpuIncident *event, long hz);
#endif
