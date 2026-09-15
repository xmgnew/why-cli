#include "history.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    WhyIdentity identity;
    uint64_t last_seen_ns, metadata_ns;
    bool lost;
} TrackedProcess;

struct WhyHistory {
    WhyHistoryEntry *first, *last;
    TrackedProcess *tracked, *scratch;
    size_t tracked_count, tracking_capacity;
    uint64_t retention_ns;
    uint64_t last_accepted_ns;
    WhyHistoryStats stats;
};

WhyHistory *why_history_create(uint64_t retention_ns, size_t budget,
                               size_t tracking_capacity) {
    if (!retention_ns || !tracking_capacity ||
        tracking_capacity > WHY_MAX_PROCESSES) {
        errno = EINVAL;
        return NULL;
    }
    size_t base =
        sizeof(WhyHistory) + 2 * tracking_capacity * sizeof(TrackedProcess);
    if (budget < base) {
        errno = ENOSPC;
        return NULL;
    }
    WhyHistory *h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    h->tracked = calloc(tracking_capacity, sizeof *h->tracked);
    h->scratch = calloc(tracking_capacity, sizeof *h->scratch);
    if (!h->tracked || !h->scratch) {
        why_history_destroy(h);
        return NULL;
    }
    h->retention_ns = retention_ns;
    h->tracking_capacity = tracking_capacity;
    h->stats.bytes = base;
    h->stats.budget = budget;
    return h;
}

static void evict(WhyHistory *h) {
    WhyHistoryEntry *entry = h->first;
    h->first = entry->next;
    h->stats.bytes -= entry->charged_bytes;
    --h->stats.frames;
    ++h->stats.evicted_frames;
    why_frame_destroy(&entry->frame);
    free(entry->events);
    free(entry);
    if (!h->first)
        h->last = NULL;
    h->stats.oldest_ns = h->first ? h->first->frame.begin_ns : 0;
    h->stats.newest_ns = h->last ? h->last->frame.end_ns : 0;
}

void why_history_destroy(WhyHistory *h) {
    if (!h)
        return;
    while (h->first)
        evict(h);
    free(h->tracked);
    free(h->scratch);
    free(h);
}

static const TrackedProcess *find_tracked(const WhyHistory *h, uint32_t pid) {
    size_t lo = 0, hi = h->tracked_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (h->tracked[mid].identity.pid < pid)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < h->tracked_count && h->tracked[lo].identity.pid == pid
               ? &h->tracked[lo]
               : NULL;
}

static void event(WhyHistoryEntry *entry, WhyLifecycleType type,
                  WhyIdentity identity, uint64_t earliest, uint64_t latest) {
    entry->events[entry->event_count++] =
        (WhyLifecycle){.type = type,
                       .identity = identity,
                       .earliest_ns = earliest,
                       .latest_ns = latest};
}

static int compare_tracked(const void *a, const void *b) {
    const TrackedProcess *x = a, *y = b;
    return (x->identity.pid > y->identity.pid) -
           (x->identity.pid < y->identity.pid);
}

static void lifecycle(WhyHistory *h, WhyHistoryEntry *entry) {
    const WhyFrame *f = &entry->frame;
    size_t count = 0;
    /* Preserve existing unknown identities before admitting new ones at the
     * cap. */
    for (size_t i = 0; i < h->tracked_count; ++i) {
        TrackedProcess t = h->tracked[i];
        const WhyProcess *p = why_find_process(f, t.identity.pid);
        bool gone = (!p && f->complete) || (p && p->status == WHY_GONE) ||
                    (p && p->status == WHY_READ_OK &&
                     !why_identity_equal(t.identity, p->id));
        if (gone) {
            event(entry, WHY_NO_LONGER_OBSERVED, t.identity, t.last_seen_ns,
                  p ? p->end_ns : f->end_ns);
            continue;
        }
        if (!p || p->status != WHY_READ_OK) {
            if (!t.lost)
                event(entry, WHY_VISIBILITY_LOST, t.identity, t.last_seen_ns,
                      f->end_ns);
            t.lost = true;
        } else {
            if (t.lost)
                event(entry, WHY_VISIBILITY_RESTORED, t.identity,
                      t.last_seen_ns, p->end_ns);
            if (p->metadata_changed)
                event(entry, WHY_METADATA_CHANGED, t.identity,
                      t.metadata_ns ? t.metadata_ns : t.last_seen_ns,
                      p->metadata ? p->metadata->end_ns : p->end_ns);
            t.lost = false;
            t.last_seen_ns = p->end_ns;
            if (p->metadata)
                t.metadata_ns = p->metadata->end_ns;
        }
        h->scratch[count++] = t;
    }
    for (size_t i = 0; i < f->count; ++i) {
        const WhyProcess *p = &f->processes[i];
        if (p->status != WHY_READ_OK)
            continue;
        const TrackedProcess *old = find_tracked(h, p->id.pid);
        if (old && why_identity_equal(old->identity, p->id))
            continue;
        if (count == h->tracking_capacity) {
            ++h->stats.tracking_dropped;
            ++entry->tracking_dropped;
            continue;
        }
        event(entry, WHY_FIRST_SEEN, p->id, p->end_ns, p->end_ns);
        h->scratch[count++] = (TrackedProcess){
            .identity = p->id,
            .last_seen_ns = p->end_ns,
            .metadata_ns = p->metadata ? p->metadata->end_ns : 0};
    }
    qsort(h->scratch, count, sizeof *h->scratch, compare_tracked);
    TrackedProcess *swap = h->tracked;
    h->tracked = h->scratch;
    h->scratch = swap;
    h->tracked_count = count;
}

static bool add_bytes(size_t *bytes, size_t more) {
    if (SIZE_MAX - *bytes < more)
        return false;
    *bytes += more;
    return true;
}

bool why_history_append(WhyHistory *h, const WhyFrame *frame) {
    if (!h || !frame || frame->count > WHY_MAX_PROCESSES ||
        (frame->count && !frame->processes) ||
        frame->begin_ns > frame->end_ns ||
        (h->last_accepted_ns && frame->end_ns <= h->last_accepted_ns)) {
        errno = EINVAL;
        return false;
    }
    for (size_t i = 0; i < frame->count; ++i) {
        const WhyProcess *p = &frame->processes[i];
        if ((i && p->id.pid <= frame->processes[i - 1].id.pid) ||
            p->begin_ns > p->end_ns || p->end_ns > frame->end_ns) {
            errno = EINVAL;
            return false;
        }
    }
    size_t event_capacity = 2 * h->tracked_count + frame->count;
    size_t bytes = sizeof(WhyHistoryEntry) + frame->count * sizeof(WhyProcess) +
                   event_capacity * sizeof(WhyLifecycle);
    for (size_t i = 0; i < frame->count; ++i) {
        const WhyProcess *p = &frame->processes[i];
        if ((p->metadata &&
             !add_bytes(&bytes, p->metadata->allocation_bytes)) ||
            (p->parent_metadata &&
             !add_bytes(&bytes, p->parent_metadata->allocation_bytes))) {
            errno = ENOSPC;
            return false;
        }
    }
    size_t base = sizeof *h + 2 * h->tracking_capacity * sizeof(TrackedProcess);
    if (bytes > h->stats.budget - base) {
        errno = ENOSPC;
        return false;
    }
    bool gap =
        h->last &&
        (why_frame_discontinuity(&h->last->frame, frame) ||
         why_system_cpu(&h->last->frame.system, &frame->system, 1).validity !=
             WHY_OK);
    while (h->first &&
           (h->stats.bytes > h->stats.budget - bytes ||
            frame->end_ns - h->first->frame.end_ns > h->retention_ns ||
            (frame->boot_ns >= h->first->frame.boot_ns &&
             frame->boot_ns - h->first->frame.boot_ns > h->retention_ns)))
        evict(h);
    WhyHistoryEntry *entry = calloc(1, sizeof *entry);
    if (!entry)
        return false;
    entry->frame = *frame;
    entry->frame.count = 0;
    entry->frame.capacity = frame->count;
    entry->frame.processes =
        frame->count ? malloc(frame->count * sizeof(WhyProcess)) : NULL;
    entry->events =
        event_capacity ? calloc(event_capacity, sizeof *entry->events) : NULL;
    if ((frame->count && !entry->frame.processes) ||
        (event_capacity && !entry->events)) {
        why_frame_destroy(&entry->frame);
        free(entry->events);
        free(entry);
        return false;
    }
    for (size_t i = 0; i < frame->count; ++i) {
        entry->frame.processes[i] = frame->processes[i];
        why_metadata_retain(frame->processes[i].metadata);
        why_metadata_retain(frame->processes[i].parent_metadata);
    }
    entry->frame.count = frame->count;
    entry->sampling_gap = gap;
    entry->charged_bytes = bytes;
    lifecycle(h, entry);
    if (h->last)
        h->last->next = entry;
    else
        h->first = entry;
    h->last = entry;
    h->stats.bytes += bytes;
    ++h->stats.frames;
    h->stats.oldest_ns = h->first->frame.begin_ns;
    h->stats.newest_ns = frame->end_ns;
    h->last_accepted_ns = frame->end_ns;
    return true;
}

const WhyHistoryEntry *why_history_first(const WhyHistory *h) {
    return h->first;
}
const WhyHistoryEntry *why_history_latest(const WhyHistory *h) {
    return h->last;
}
WhyHistoryStats why_history_stats(const WhyHistory *h) { return h->stats; }

const char *why_lifecycle_name(WhyLifecycleType type) {
    switch (type) {
    case WHY_FIRST_SEEN:
        return "first seen";
    case WHY_NO_LONGER_OBSERVED:
        return "no longer observed";
    case WHY_VISIBILITY_LOST:
        return "visibility lost";
    case WHY_VISIBILITY_RESTORED:
        return "visibility restored";
    case WHY_METADATA_CHANGED:
        return "metadata changed";
    }
    return "unknown";
}
