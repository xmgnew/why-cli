#include "why.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool number(const char **cursor, uint64_t *value) {
    const char *p = *cursor;
    while (isspace((unsigned char)*p))
        ++p;
    if (!isdigit((unsigned char)*p))
        return false;
    errno = 0;
    char *end;
    unsigned long long n = strtoull(p, &end, 10);
    if (errno == ERANGE || (*end && !isspace((unsigned char)*end)))
        return false;
    *value = (uint64_t)n;
    *cursor = end;
    return true;
}

bool why_parse_process(const char *text, WhyProcess *out) {
    WhyProcess p = {0};
    uint64_t pid;
    if (!number(&text, &pid) || pid == 0 || pid > UINT32_MAX)
        return false;
    while (isspace((unsigned char)*text))
        ++text;
    const char *close = strrchr(text, ')');
    if (*text != '(' || !close || close <= text)
        return false;
    /* Kernel worker display names can exceed TASK_COMM_LEN. */
    size_t name_length = (size_t)(close - text - 1);
    p.comm_truncated = name_length >= sizeof p.comm;
    if (p.comm_truncated)
        name_length = sizeof p.comm - 1;
    memcpy(p.comm, text + 1, name_length);
    text = close + 1;
    while (isspace((unsigned char)*text))
        ++text;
    if (!*text || !strchr("RSDZTtXxKWPI", *text) ||
        !isspace((unsigned char)text[1]))
        return false;
    p.state = *text++;
    p.id.pid = (uint32_t)pid;
    for (unsigned field = 4; field <= 22; ++field) {
        uint64_t value;
        while (isspace((unsigned char)*text))
            ++text;
        bool negative = *text == '-';
        if (negative)
            ++text;
        if (!isdigit((unsigned char)*text))
            return false;
        if (!number(&text, &value))
            return false;
        bool required_unsigned = field == 4 || field == 14 || field == 15 ||
                                 field == 20 || field == 22;
        if (required_unsigned && negative)
            return false;
        switch (field) {
        case 4:
            if (value > UINT32_MAX)
                return false;
            p.ppid = (uint32_t)value;
            break;
        case 14:
            p.user_ticks = value;
            break;
        case 15:
            p.system_ticks = value;
            break;
        case 20:
            p.threads = value;
            break;
        case 22:
            p.id.start_ticks = value;
            break;
        default:
            break;
        }
    }
    *out = p;
    return true;
}

bool why_parse_system(const char *text, WhySystem *out) {
    WhySystem s = {0};
    bool aggregate = false;
    while (*text) {
        const char *end = strchr(text, '\n');
        if (!end)
            end = text + strlen(text);
        if (strncmp(text, "cpu", 3) == 0) {
            bool total = isspace((unsigned char)text[3]);
            const char *p = text + 3;
            uint64_t id = 0;
            if (!total && (!number(&p, &id) || id >= WHY_MAX_CPUS))
                return false;
            if (total ? aggregate : s.cpus[id])
                return false;
            for (size_t i = 0; i < 10; ++i) {
                while (p < end && isspace((unsigned char)*p))
                    ++p;
                uint64_t value;
                if (p >= end || !number(&p, &value) || p > end)
                    return false;
                if (total)
                    s.ticks[i] = value;
            }
            if (total)
                aggregate = true;
            else {
                s.cpus[id] = true;
                ++s.cpu_count;
            }
        }
        text = *end ? end + 1 : end;
    }
    if (!aggregate || s.cpu_count == 0)
        return false;
    *out = s;
    return true;
}

bool why_identity_equal(WhyIdentity a, WhyIdentity b) {
    return a.pid == b.pid && a.start_ticks == b.start_ticks;
}

static uint64_t midpoint(uint64_t begin, uint64_t end) {
    return begin + (end - begin) / 2;
}

static bool interval(uint64_t a, uint64_t b, double *seconds) {
    if (b <= a || b - a < WHY_SECOND / 2 || b - a > 3 * WHY_SECOND / 2)
        return false;
    *seconds = (double)(b - a) / (double)WHY_SECOND;
    return true;
}

WhyCpu why_process_cpu(const WhyProcess *before, const WhyProcess *after,
                       long hz) {
    WhyCpu result = {.validity = WHY_FIRST_SAMPLE};
    if (!before || before->status != WHY_READ_OK ||
        after->status != WHY_READ_OK)
        return result;
    if (!why_identity_equal(before->id, after->id)) {
        result.validity = WHY_IDENTITY_CHANGED;
        return result;
    }
    double seconds;
    result.validity = WHY_CLOCK_GAP;
    if (hz <= 0 || before->end_ns < before->begin_ns ||
        after->end_ns < after->begin_ns ||
        !interval(midpoint(before->begin_ns, before->end_ns),
                  midpoint(after->begin_ns, after->end_ns), &seconds))
        return result;
    result.validity = WHY_COUNTER_RESET;
    if (after->user_ticks < before->user_ticks ||
        after->system_ticks < before->system_ticks)
        return result;
    result.cpu_seconds =
        ((double)(after->user_ticks - before->user_ticks) +
         (double)(after->system_ticks - before->system_ticks)) /
        (double)hz;
    result.cores = result.cpu_seconds / seconds;
    result.validity = WHY_OK;
    return result;
}

WhyCpu why_system_cpu(const WhySystem *before, const WhySystem *after,
                      long hz) {
    WhyCpu result = {.validity = WHY_FIRST_SAMPLE};
    if (!before)
        return result;
    result.validity = WHY_TOPOLOGY_CHANGED;
    if (memcmp(before->cpus, after->cpus, sizeof before->cpus) != 0)
        return result;
    double seconds;
    result.validity = WHY_CLOCK_GAP;
    if (hz <= 0 || before->end_ns < before->begin_ns ||
        after->end_ns < after->begin_ns ||
        !interval(midpoint(before->begin_ns, before->end_ns),
                  midpoint(after->begin_ns, after->end_ns), &seconds))
        return result;
    double total = 0, busy = 0;
    result.validity = WHY_COUNTER_RESET;
    for (size_t i = 0; i < 8; ++i) {
        if (after->ticks[i] < before->ticks[i])
            return result;
        double delta = (double)(after->ticks[i] - before->ticks[i]);
        total += delta;
        if (i != 3 && i != 4 && i != 7)
            busy += delta;
    }
    result.validity = WHY_NO_TICKS;
    if (total == 0)
        return result;
    result.cpu_seconds = busy / (double)hz;
    result.cores = result.cpu_seconds / seconds;
    result.busy_pct = 100 * busy / total;
    result.validity = WHY_OK;
    return result;
}

const char *why_validity_name(WhyValidity validity) {
    switch (validity) {
    case WHY_OK:
        return "valid";
    case WHY_FIRST_SAMPLE:
        return "no adjacent sample";
    case WHY_IDENTITY_CHANGED:
        return "PID reused";
    case WHY_CLOCK_GAP:
        return "sampling gap";
    case WHY_COUNTER_RESET:
        return "counter decreased";
    case WHY_TOPOLOGY_CHANGED:
        return "CPU set changed";
    case WHY_NO_TICKS:
        return "no CPU ticks";
    }
    return "unknown";
}

const WhyProcess *why_find_process(const WhyFrame *frame, uint32_t pid) {
    size_t lo = 0, hi = frame->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (frame->processes[mid].id.pid < pid)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < frame->count && frame->processes[lo].id.pid == pid
               ? &frame->processes[lo]
               : NULL;
}

void why_resolve_parents(WhyFrame *frame) {
    for (size_t i = 0; i < frame->count; ++i) {
        WhyProcess *p = &frame->processes[i];
        why_metadata_release(p->parent_metadata);
        p->parent_metadata = NULL;
        p->parent_known = false;
        const WhyProcess *parent = why_find_process(frame, p->ppid);
        if (p->status == WHY_READ_OK && parent &&
            parent->status == WHY_READ_OK && parent->id.pid != p->id.pid &&
            parent->id.start_ticks <= p->id.start_ticks) {
            p->parent = parent->id;
            p->parent_known = true;
            p->parent_metadata = parent->metadata;
            why_metadata_retain(p->parent_metadata);
        }
    }
}
