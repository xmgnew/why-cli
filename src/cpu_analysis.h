#ifndef WHY_CPU_ANALYSIS_H
#define WHY_CPU_ANALYSIS_H

#include "why.h"

#define WHY_BASELINE_SAMPLES 30U
#define WHY_BASELINE_MIN 10U
#define WHY_CPU_ALGORITHM_VERSION 1U

typedef struct {
    uint64_t start_ns, end_ns;
    double busy_pct, cpu_seconds;
    WhyValidity validity;
} WhySystemInterval;

typedef struct {
    WhyValidity validity;
    uint64_t start_ns, end_ns;
    double cpu_seconds, covered_seconds;
} WhyCpuOverlap;

/* Uniform-rate estimate over the intersection; never extrapolates missing time.
 * Partition windows must not overlap when summing results for one source span.
 */
WhyCpuOverlap why_cpu_overlap(const WhyProcess *before, const WhyProcess *after,
                              long hz, uint64_t start_ns, uint64_t end_ns);
WhySystemInterval why_system_interval(const WhyFrame *before,
                                      const WhyFrame *after, long hz);

typedef enum {
    WHY_WARMUP,
    WHY_IDLE,
    WHY_CANDIDATE,
    WHY_ACTIVE
} WhyDetectorState;
typedef enum {
    WHY_INCIDENT_ONGOING,
    WHY_INCIDENT_RECOVERED,
    WHY_INCIDENT_INTERRUPTED
} WhyIncidentStatus;

typedef struct {
    uint64_t start_ns, end_ns, detected_ns;
    uint64_t peak_start_ns, peak_end_ns;
    uint64_t baseline_start_ns, baseline_end_ns;
    double baseline_pct, mad_pct, enter_pct, exit_pct;
    double peak_pct, cpu_seconds;
    WhyIncidentStatus status;
    unsigned algorithm_version;
} WhyCpuIncident;

/* Fixed-size, pointer-free state. No hidden storage or allocation. */
typedef struct {
    WhyDetectorState state;
    WhySystemInterval baseline[WHY_BASELINE_SAMPLES];
    size_t baseline_count;
    WhySystemInterval candidate, recovery[3];
    size_t recovery_count;
    uint64_t last_end_ns;
    WhyCpuIncident incident;
} WhyCpuDetector;

/* A zero-initialized detector is ready for input. Returns a completed incident
 * only on recovery/interruption; output is unchanged otherwise. Invalid samples
 * reset warmup. Discontinuous valid samples begin a new baseline after the gap.
 */
bool why_detector_push(WhyCpuDetector *detector, WhySystemInterval interval,
                       WhyCpuIncident *completed);
bool why_detector_current(const WhyCpuDetector *detector, WhyCpuIncident *out);

#endif
