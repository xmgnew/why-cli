#ifndef WHY_HISTORY_H
#define WHY_HISTORY_H

#include "cpu_analysis.h"

#define WHY_HISTORY_SECONDS 300U
#define WHY_HISTORY_BYTES (64U * 1024U * 1024U)

typedef enum {
    WHY_FIRST_SEEN,
    WHY_NO_LONGER_OBSERVED,
    WHY_VISIBILITY_LOST,
    WHY_VISIBILITY_RESTORED,
    WHY_METADATA_CHANGED
} WhyLifecycleType;

typedef struct {
    WhyLifecycleType type;
    WhyIdentity identity;
    uint64_t earliest_ns, latest_ns;
} WhyLifecycle;

/* Borrowed immutable views; any append may invalidate them through eviction. */
typedef struct WhyHistoryEntry {
    WhyFrame frame;
    WhyLifecycle *events;
    size_t event_count;
    size_t tracking_dropped;
    bool sampling_gap;
    bool incident_completed;
    WhyCpuIncident incident;
    struct WhyHistoryEntry *next;
    size_t charged_bytes;
} WhyHistoryEntry;

typedef struct WhyHistory WhyHistory;

typedef struct {
    size_t frames, bytes, budget, evicted_frames, tracking_dropped;
    uint64_t oldest_ns, newest_ns;
} WhyHistoryStats;

/* Capacity must be 1..WHY_MAX_PROCESSES. No disk storage or background thread.
 */
WhyHistory *why_history_create(uint64_t retention_ns, size_t budget,
                               size_t tracking_capacity);
void why_history_destroy(WhyHistory *history);
/* Copies a sorted frame, retaining immutable metadata. Strictly increasing
 * times. On failure, tracker state is unchanged, but older frames may have been
 * evicted. ENOSPC means one snapshot exceeds the configured record budget.
 */
bool why_history_append(WhyHistory *history, const WhyFrame *frame, long hz);
const WhyHistoryEntry *why_history_first(const WhyHistory *history);
const WhyHistoryEntry *why_history_latest(const WhyHistory *history);
WhyHistoryStats why_history_stats(const WhyHistory *history);
const char *why_lifecycle_name(WhyLifecycleType type);

const WhyCpuDetector *why_history_detector(const WhyHistory *history);

typedef struct {
    double cpu_seconds;
    size_t portions, unavailable_intervals;
    bool left_truncated, right_provisional;
    bool partial_scan, timing_degraded;
} WhyAlignedSummary;
/* Visitor receives only observed nonempty portions; no synthetic zero values.
 * It must not mutate history. There is no allocation and no extrapolation.
 */
typedef void (*WhyAlignedVisitor)(WhyIdentity identity, WhyCpuOverlap portion,
                                  void *context);
WhyAlignedSummary why_history_align(const WhyHistory *history,
                                    uint64_t start_ns, uint64_t end_ns, long hz,
                                    WhyAlignedVisitor visitor, void *context);

#endif
