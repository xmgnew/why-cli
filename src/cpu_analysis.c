#include "cpu_analysis.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint64_t middle(uint64_t begin, uint64_t end) {
    return begin + (end - begin) / 2;
}

WhyCpuOverlap why_cpu_overlap(const WhyProcess *before, const WhyProcess *after,
                              long hz, uint64_t start_ns, uint64_t end_ns) {
    WhyCpuOverlap out = {.validity = WHY_CLOCK_GAP};
    if (end_ns <= start_ns)
        return out;
    WhyCpu cpu = why_process_cpu(before, after, hz);
    out.validity = cpu.validity;
    if (cpu.validity != WHY_OK)
        return out;
    uint64_t a = middle(before->begin_ns, before->end_ns);
    uint64_t b = middle(after->begin_ns, after->end_ns);
    uint64_t left = a > start_ns ? a : start_ns;
    uint64_t right = b < end_ns ? b : end_ns;
    if (right <= left)
        return out;
    out.start_ns = left;
    out.end_ns = right;
    out.covered_seconds = (double)(right - left) / (double)WHY_SECOND;
    out.cpu_seconds =
        cpu.cpu_seconds * ((double)(right - left) / (double)(b - a));
    return out;
}

WhySystemInterval why_system_interval(const WhyFrame *before,
                                      const WhyFrame *after, long hz) {
    WhySystemInterval out = {.validity = WHY_FIRST_SAMPLE};
    if (!before)
        return out;
    WhyCpu cpu = why_system_cpu(&before->system, &after->system, hz);
    out.validity = cpu.validity;
    if (before->system.end_ns >= before->system.begin_ns &&
        after->system.end_ns >= after->system.begin_ns) {
        out.start_ns = middle(before->system.begin_ns, before->system.end_ns);
        out.end_ns = middle(after->system.begin_ns, after->system.end_ns);
    }
    if (why_frame_discontinuity(before, after))
        out.validity = WHY_CLOCK_GAP;
    if (out.validity == WHY_OK) {
        out.busy_pct = cpu.busy_pct;
        out.cpu_seconds = cpu.cpu_seconds;
    }
    return out;
}

static int compare_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median(double *values, size_t n) {
    qsort(values, n, sizeof *values, compare_double);
    return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
}

static void baseline_add(WhyCpuDetector *d, WhySystemInterval sample) {
    if (d->baseline_count == WHY_BASELINE_SAMPLES) {
        memmove(d->baseline, d->baseline + 1,
                (WHY_BASELINE_SAMPLES - 1) * sizeof *d->baseline);
        --d->baseline_count;
    }
    d->baseline[d->baseline_count++] = sample;
    d->state = d->baseline_count >= WHY_BASELINE_MIN ? WHY_IDLE : WHY_WARMUP;
}

static WhyCpuIncident thresholds(const WhyCpuDetector *d) {
    double values[WHY_BASELINE_SAMPLES];
    for (size_t i = 0; i < d->baseline_count; ++i)
        values[i] = d->baseline[i].busy_pct;
    double base = median(values, d->baseline_count);
    for (size_t i = 0; i < d->baseline_count; ++i)
        values[i] = fabs(d->baseline[i].busy_pct - base);
    double mad = median(values, d->baseline_count);
    double rise = 3 * 1.4826 * mad;
    if (rise < 20)
        rise = 20;
    double enter = base + rise;
    return (WhyCpuIncident){.baseline_start_ns = d->baseline[0].start_ns,
                            .baseline_end_ns =
                                d->baseline[d->baseline_count - 1].end_ns,
                            .baseline_pct = base,
                            .mad_pct = mad,
                            .enter_pct = enter > 50 ? enter : 50,
                            .exit_pct = base + rise / 2,
                            .status = WHY_INCIDENT_ONGOING,
                            .algorithm_version = WHY_CPU_ALGORITHM_VERSION};
}

static void accumulate(WhyCpuIncident *event, WhySystemInterval sample) {
    event->end_ns = sample.end_ns;
    event->cpu_seconds += sample.cpu_seconds;
    if (sample.busy_pct > event->peak_pct) {
        event->peak_pct = sample.busy_pct;
        event->peak_start_ns = sample.start_ns;
        event->peak_end_ns = sample.end_ns;
    }
}

bool why_detector_current(const WhyCpuDetector *d, WhyCpuIncident *out) {
    if (d->state != WHY_ACTIVE)
        return false;
    *out = d->incident;
    for (size_t i = 0; i < d->recovery_count; ++i)
        accumulate(out, d->recovery[i]);
    return true;
}

bool why_detector_push(WhyCpuDetector *d, WhySystemInterval sample,
                       WhyCpuIncident *completed) {
    bool valid = sample.validity == WHY_OK && sample.end_ns > sample.start_ns &&
                 sample.end_ns - sample.start_ns >= WHY_SECOND / 2 &&
                 sample.end_ns - sample.start_ns <= 3 * WHY_SECOND / 2 &&
                 isfinite(sample.busy_pct) && sample.busy_pct >= 0 &&
                 sample.busy_pct <= 100 && isfinite(sample.cpu_seconds) &&
                 sample.cpu_seconds >= 0;
    bool interrupted = false;
    if (!valid || (d->last_end_ns && sample.start_ns != d->last_end_ns)) {
        interrupted = why_detector_current(d, completed);
        if (interrupted)
            completed->status = WHY_INCIDENT_INTERRUPTED;
        *d = (WhyCpuDetector){0};
        if (!valid)
            return interrupted;
    }
    d->last_end_ns = sample.end_ns;
    if (d->state == WHY_WARMUP) {
        baseline_add(d, sample);
    } else if (d->state == WHY_IDLE) {
        WhyCpuIncident event = thresholds(d);
        if (sample.busy_pct >= event.enter_pct) {
            d->candidate = sample;
            d->incident = event;
            d->state = WHY_CANDIDATE;
        } else {
            baseline_add(d, sample);
        }
    } else if (d->state == WHY_CANDIDATE) {
        if (sample.busy_pct >= d->incident.enter_pct) {
            d->incident.start_ns = d->candidate.start_ns;
            d->incident.detected_ns = sample.end_ns;
            accumulate(&d->incident, d->candidate);
            accumulate(&d->incident, sample);
            d->state = WHY_ACTIVE;
        } else {
            baseline_add(d, d->candidate);
            baseline_add(d, sample);
        }
    } else if (sample.busy_pct <= d->incident.exit_pct) {
        d->recovery[d->recovery_count++] = sample;
        if (d->recovery_count == 3) {
            *completed = d->incident;
            completed->end_ns = d->recovery[0].start_ns;
            completed->status = WHY_INCIDENT_RECOVERED;
            d->baseline_count = 0;
            for (size_t i = 0; i < 3; ++i)
                baseline_add(d, d->recovery[i]);
            d->recovery_count = 0;
            return true;
        }
    } else {
        for (size_t i = 0; i < d->recovery_count; ++i)
            accumulate(&d->incident, d->recovery[i]);
        d->recovery_count = 0;
        accumulate(&d->incident, sample);
    }
    return interrupted;
}
