#include "history.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c)                                                               \
    do {                                                                       \
        if (!(c)) {                                                            \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);            \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

static WhySystemInterval sample(unsigned t, double pct) {
    return (WhySystemInterval){.start_ns = (uint64_t)t * WHY_SECOND,
                               .end_ns = (uint64_t)(t + 1) * WHY_SECOND,
                               .busy_pct = pct,
                               .cpu_seconds = pct / 100,
                               .validity = WHY_OK};
}
static void warmup(WhyCpuDetector *d, double pct) {
    WhyCpuIncident out;
    for (unsigned i = 1; i <= 10; ++i)
        CHECK(!why_detector_push(d, sample(i, pct), &out));
    CHECK(d->state == WHY_IDLE);
}
static void test_detection(void) {
    WhyCpuDetector d = {0};
    WhyCpuIncident out = {0};
    warmup(&d, 14);
    CHECK(!why_detector_push(&d, sample(11, 80), &out));
    CHECK(d.state == WHY_CANDIDATE && !why_detector_current(&d, &out));
    CHECK(!why_detector_push(&d, sample(12, 87), &out));
    CHECK(why_detector_current(&d, &out));
    CHECK(out.start_ns == 11 * WHY_SECOND &&
          out.detected_ns == 13 * WHY_SECOND);
    CHECK(out.enter_pct == 50 && out.exit_pct == 24 && out.peak_pct == 87);
    CHECK(out.peak_start_ns == 12 * WHY_SECOND);
    CHECK(!why_detector_push(&d, sample(13, 20), &out));
    CHECK(!why_detector_push(&d, sample(14, 18), &out));
    CHECK(why_detector_current(&d, &out) && out.end_ns == 15 * WHY_SECOND);
    CHECK(why_detector_push(&d, sample(15, 16), &out));
    CHECK(out.status == WHY_INCIDENT_RECOVERED &&
          out.end_ns == 13 * WHY_SECOND);
    CHECK(fabs(out.cpu_seconds - 1.67) < 1e-9);
    CHECK(d.state == WHY_WARMUP && d.baseline_count == 3);

    d = (WhyCpuDetector){0};
    warmup(&d, 14);
    CHECK(!why_detector_push(&d, sample(11, 90), &out));
    CHECK(!why_detector_push(&d, sample(12, 14), &out));
    CHECK(d.state == WHY_IDLE && d.baseline_count == 12);
    CHECK(!why_detector_push(&d, sample(13, 80), &out));
    CHECK(!why_detector_push(&d, sample(14, 80), &out));
    CHECK(!why_detector_push(&d, sample(15, 20), &out));
    CHECK(!why_detector_push(&d, sample(16, 90), &out));
    CHECK(d.recovery_count == 0 && why_detector_current(&d, &out));
    CHECK(fabs(out.cpu_seconds - 2.7) < 1e-9);
    WhySystemInterval bad = sample(17, 0);
    bad.validity = WHY_COUNTER_RESET;
    CHECK(why_detector_push(&d, bad, &out));
    CHECK(out.status == WHY_INCIDENT_INTERRUPTED &&
          out.end_ns == 17 * WHY_SECOND);
    CHECK(d.state == WHY_WARMUP && d.baseline_count == 0);

    d = (WhyCpuDetector){0};
    warmup(&d, 90);
    CHECK(!why_detector_push(&d, sample(11, 100), &out));
    CHECK(d.state == WHY_IDLE); /* enter=110, never clamped */
    d = (WhyCpuDetector){0};
    for (unsigned i = 1; i <= 10; ++i)
        CHECK(!why_detector_push(&d, sample(i, i % 2 ? 20 : 60), &out));
    CHECK(!why_detector_push(&d, sample(11, 100), &out));
    CHECK(d.state == WHY_IDLE); /* MAD makes threshold > 100 */
    d = (WhyCpuDetector){0};
    for (unsigned i = 1; i <= 40; ++i)
        CHECK(!why_detector_push(&d, sample(i, 14), &out));
    CHECK(d.baseline_count == WHY_BASELINE_SAMPLES);
    CHECK(d.baseline[0].start_ns == 11 * WHY_SECOND);

    d = (WhyCpuDetector){0};
    warmup(&d, 14);
    CHECK(!why_detector_push(&d, sample(11, 80), &out));
    CHECK(!why_detector_push(&d, sample(12, 80), &out));
    CHECK(why_detector_push(&d, sample(20, 80), &out));
    CHECK(out.status == WHY_INCIDENT_INTERRUPTED && d.baseline_count == 1);
    bad = sample(21, NAN);
    CHECK(!why_detector_push(&d, bad, &out));
    CHECK(d.baseline_count == 0);
}

static WhyProcess process(unsigned tick, uint64_t cpu) {
    return (WhyProcess){.id = {42, 1},
                        .begin_ns =
                            (uint64_t)tick * WHY_SECOND + WHY_SECOND / 4,
                        .end_ns = (uint64_t)tick * WHY_SECOND + WHY_SECOND / 4,
                        .user_ticks = cpu,
                        .status = WHY_READ_OK};
}
static void test_alignment(void) {
    WhyProcess a = process(1, 0), b = process(2, 200);
    WhyCpuOverlap x = why_cpu_overlap(&a, &b, 100, WHY_SECOND, 2 * WHY_SECOND);
    WhyCpuOverlap y =
        why_cpu_overlap(&a, &b, 100, 2 * WHY_SECOND, 3 * WHY_SECOND);
    CHECK(x.validity == WHY_OK && fabs(x.cpu_seconds - 1.5) < 1e-9);
    CHECK(fabs(y.cpu_seconds - 0.5) < 1e-9);
    CHECK(x.cpu_seconds + y.cpu_seconds == 2);
    CHECK(x.covered_seconds + y.covered_seconds == 1);
    CHECK(why_cpu_overlap(&a, &b, 100, 3 * WHY_SECOND, 4 * WHY_SECOND)
              .covered_seconds == 0);
    CHECK(why_cpu_overlap(NULL, &b, 100, WHY_SECOND, 2 * WHY_SECOND).validity ==
          WHY_FIRST_SAMPLE);
    b.id.start_ticks = 2;
    CHECK(why_cpu_overlap(&a, &b, 100, WHY_SECOND, 2 * WHY_SECOND).validity ==
          WHY_IDENTITY_CHANGED);
}

static void test_history_pipeline(void) {
    WhyHistory *h = why_history_create(3 * WHY_SECOND, 1000000, 4);
    CHECK(h);
    WhyFrame f = {.complete = true};
    WhyProcess p;
    f.processes = &p;
    f.count = f.capacity = 1;
    f.system.cpus[0] = true;
    for (unsigned i = 1; i <= 20; ++i) {
        p = process(i, i * 10);
        f.begin_ns = f.boot_ns = f.system.begin_ns = f.system.end_ns =
            (uint64_t)i * WHY_SECOND;
        f.end_ns = p.end_ns;
        unsigned pct = i >= 12 && i <= 17 ? 80 : 14;
        f.system.ticks[0] += pct;
        f.system.ticks[3] += 100 - pct;
        CHECK(why_history_append(h, &f, 100));
    }
    const WhyHistoryEntry *e = why_history_latest(h);
    CHECK(e->incident_completed &&
          e->incident.status == WHY_INCIDENT_RECOVERED);
    CHECK(e->incident.start_ns == 11 * WHY_SECOND &&
          e->incident.end_ns == 17 * WHY_SECOND);
    CHECK(e->incident.cpu_seconds > 4.79 && e->incident.cpu_seconds < 4.81);
    CHECK(why_history_stats(h).evicted_frames > 0);
    WhyAlignedSummary aligned =
        why_history_align(h, 18 * WHY_SECOND, 19 * WHY_SECOND, 100, NULL, NULL);
    CHECK(!aligned.left_truncated && !aligned.right_provisional);
    CHECK(fabs(aligned.cpu_seconds - 0.1) < 1e-9);
    aligned =
        why_history_align(h, WHY_SECOND, 21 * WHY_SECOND, 100, NULL, NULL);
    CHECK(aligned.left_truncated && aligned.right_provisional);
    why_history_destroy(h);
}
int main(void) {
    test_detection();
    test_alignment();
    test_history_pipeline();
    puts("CPU alignment, detection, recovery and history integration checks "
         "passed.");
    return 0;
}
