#include "history.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop(int signal_number) {
    (void)signal_number;
    stopped = 1;
}

static void escaped_bytes(const unsigned char *text, size_t length) {
    for (const unsigned char *p = text; p < text + length; ++p) {
        if (*p < 32 || *p >= 127 || *p == '\\')
            printf("\\x%02x", (unsigned)*p);
        else
            putchar(*p);
    }
}

static void escaped(const char *text) {
    escaped_bytes((const unsigned char *)text, strlen(text));
}

static const char *field_status(WhyFieldStatus status) {
    switch (status) {
    case WHY_FIELD_OK:
        return "available";
    case WHY_FIELD_EMPTY:
        return "empty";
    case WHY_FIELD_MISSING:
        return "missing";
    case WHY_FIELD_DENIED:
        return "permission denied";
    case WHY_FIELD_ERROR:
        return "read error";
    case WHY_FIELD_UNVERIFIED:
        return "identity recheck failed";
    case WHY_FIELD_BUDGET:
        return "metadata budget reached";
    }
    return "unknown";
}

static void display_metadata(const WhyProcess *p) {
    if (!p->metadata) {
        printf("  Context: unavailable (%s)\n",
               field_status(p->metadata_status));
        return;
    }
    const char *names[] = {"Arguments (NUL-separated)", "Working directory",
                           "Executable"};
    printf("  Context observed at monotonic %.3f s\n",
           (double)p->metadata->end_ns / (double)WHY_SECOND);
    for (size_t i = 0; i < WHY_METADATA_FIELDS; ++i) {
        const WhyMetadataField *f = &p->metadata->fields[i];
        printf("  %s: ", names[i]);
        if (f->status == WHY_FIELD_OK)
            escaped_bytes(p->metadata->data + f->offset, f->length);
        else
            printf("[%s]", field_status(f->status));
        if (f->truncated)
            printf(" [truncated]");
        putchar('\n');
    }
}

static void display_history(const WhyHistory *history, bool timeline) {
    WhyHistoryStats stats = why_history_stats(history);
    printf("\nHistory: %zu frames, %.1f seconds, %zu / %zu charged bytes; "
           "%zu evicted, %zu untracked observations\n",
           stats.frames,
           stats.frames ? (double)(stats.newest_ns - stats.oldest_ns) /
                              (double)WHY_SECOND
                        : 0,
           stats.bytes, stats.budget, stats.evicted_frames,
           stats.tracking_dropped);
    if (!timeline)
        return;
    puts("Recorded lifecycle observations (monotonic seconds; disappearance is "
         "not an exact exit):");
    for (const WhyHistoryEntry *entry = why_history_first(history); entry;
         entry = entry->next) {
        if (entry->tracking_dropped)
            printf(
                "  %zu new observations could not be tracked in this frame\n",
                entry->tracking_dropped);
        if (entry->sampling_gap)
            printf("  %.3f  sampling gap\n",
                   (double)entry->frame.end_ns / (double)WHY_SECOND);
        for (size_t i = 0; i < entry->event_count; ++i) {
            const WhyLifecycle *event = &entry->events[i];
            printf("  %.3f..%.3f  %u:%" PRIu64 "  %s\n",
                   (double)event->earliest_ns / (double)WHY_SECOND,
                   (double)event->latest_ns / (double)WHY_SECOND,
                   event->identity.pid, event->identity.start_ticks,
                   why_lifecycle_name(event->type));
        }
    }
}

static void display(const WhyFrame *previous, const WhyFrame *current, long hz,
                    size_t sequence, bool details) {
    bool gap = previous && why_frame_discontinuity(previous, current);
    WhyCpu system = why_system_cpu(previous ? &previous->system : NULL,
                                   &current->system, hz);
    if (gap)
        system.validity = WHY_CLOCK_GAP;
    printf("\nSample %zu | %zu visible entries | scan %.2f ms\n", sequence,
           current->count, (double)(current->end_ns - current->begin_ns) / 1e6);
    if (system.validity == WHY_OK)
        printf("System CPU: %.1f%% busy (whole machine), %.3f CPU seconds\n",
               system.busy_pct, system.cpu_seconds);
    else
        printf("System CPU: unavailable (%s)\n",
               why_validity_name(system.validity));
    printf("Scan: %s; skipped=%zu gone=%zu denied=%zu malformed=%zu I/O "
           "errors=%zu\n",
           current->complete ? "complete" : "partial", current->skipped,
           current->gone, current->denied, current->malformed,
           current->io_errors);
    if (current->metadata_skipped)
        printf("Context unavailable for %zu readable processes.\n",
               current->metadata_skipped);
    if (current->end_ns - current->begin_ns > WHY_SECOND / 5)
        puts("Timing quality: scan exceeds 200 ms; attribution would be "
             "degraded.");
    puts("PID\tPPID\tSTART_TICKS\tCPU_SECONDS\tCORES\tSTATE\tPARENT_"
         "ID\tCOMMAND");
    for (size_t i = 0; i < current->count; ++i) {
        const WhyProcess *p = &current->processes[i];
        if (p->status != WHY_READ_OK)
            continue;
        const WhyProcess *old =
            previous ? why_find_process(previous, p->id.pid) : NULL;
        WhyCpu cpu = why_process_cpu(old, p, hz);
        if (gap || system.validity == WHY_TOPOLOGY_CHANGED)
            cpu.validity = WHY_CLOCK_GAP;
        printf("%u\t%u\t%" PRIu64 "\t", p->id.pid, p->ppid, p->id.start_ticks);
        if (cpu.validity == WHY_OK)
            printf("%.3f\t%.3f\t", cpu.cpu_seconds, cpu.cores);
        else
            printf("n/a\tn/a\t");
        printf("%c\t", p->state);
        if (p->parent_known)
            printf("%u:%" PRIu64 "\t", p->parent.pid, p->parent.start_ticks);
        else
            printf("unknown\t");
        escaped(p->comm);
        if (p->comm_truncated)
            printf(" [name truncated]");
        if (p->id.pid == (uint32_t)getpid())
            printf(" [why recorder]");
        if (cpu.validity != WHY_OK)
            printf(" [%s]", why_validity_name(cpu.validity));
        putchar('\n');
        if (details)
            display_metadata(p);
    }
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        puts("Usage: why sample [--count N] [--details] [--history]\n\n"
             "Linux process/CPU recorder prototype; 1-second sampling.\n"
             "N includes the initial baseline sample (default: 2). Ctrl-C "
             "stops.\n"
             "--details displays captured arguments, cwd and executable.\n"
             "--history displays the retained lifecycle timeline on exit.\n"
             "History is bounded to 300 seconds / 64 MiB and disappears on "
             "exit.\n"
             "Spike detection and cross-terminal queries are not implemented "
             "yet.");
        return 0;
    }
    if (argc < 2 || strcmp(argv[1], "sample") != 0) {
        fprintf(stderr, "Use 'why sample [--count N] [--details] [--history]' "
                        "or 'why --help'.\n");
        return 2;
    }
    size_t count = 2;
    bool details = false, timeline = false, have_count = false;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--details") && !details) {
            details = true;
        } else if (!strcmp(argv[i], "--history") && !timeline) {
            timeline = true;
        } else if (!strcmp(argv[i], "--count") && !have_count && i + 1 < argc) {
            const char *value = argv[++i];
            char *end;
            errno = 0;
            unsigned long n = strtoul(value, &end, 10);
            if (value[0] < '1' || value[0] > '9' || errno || *end ||
                n > 1000000) {
                fprintf(stderr, "--count must be between 1 and 1000000.\n");
                return 2;
            }
            count = (size_t)n;
            have_count = true;
        } else {
            fprintf(stderr, "Unknown, repeated or incomplete option: %s\n",
                    argv[i]);
            return 2;
        }
    }
#ifndef __linux__
    fprintf(stderr, "Live collection requires Linux. Core tests can run on "
                    "this platform.\n");
    return 1;
#endif
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) {
        perror("sysconf(_SC_CLK_TCK)");
        return 1;
    }
    struct sigaction action = {0};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    WhyFrame frames[2] = {0};
    if (!why_frame_init(&frames[0]) || !why_frame_init(&frames[1])) {
        fprintf(stderr, "Cannot allocate bounded sample buffers.\n");
        why_frame_destroy(&frames[0]);
        why_frame_destroy(&frames[1]);
        return 1;
    }
    WhyHistory *history = why_history_create(
        WHY_HISTORY_SECONDS * WHY_SECOND, WHY_HISTORY_BYTES, WHY_MAX_PROCESSES);
    if (!history) {
        perror("Cannot allocate history");
        why_frame_destroy(&frames[0]);
        why_frame_destroy(&frames[1]);
        return 1;
    }
    uint32_t cursor = 0;
    uint64_t deadline = why_now_ns();
    int status = 0;
    for (size_t i = 0; i < count && !stopped; ++i) {
        WhyFrame *current = &frames[i % 2];
        const WhyFrame *previous = i ? &frames[(i - 1) % 2] : NULL;
        if (!why_collect("/proc", &cursor, previous, current)) {
            perror("Cannot collect /proc");
            status = 1;
            break;
        }
        if (!why_history_append(history, current)) {
            perror("Cannot retain sample");
            status = 1;
            break;
        }
        display(previous, current, hz, i + 1, details);
        if (i + 1 == count)
            break;
        deadline += WHY_SECOND;
        uint64_t now = why_now_ns();
        if (now >= deadline)
            deadline += ((now - deadline) / WHY_SECOND + 1) * WHY_SECOND;
        while (!stopped && (now = why_now_ns()) < deadline) {
            uint64_t remaining = deadline - now;
            struct timespec delay = {.tv_sec = (time_t)(remaining / WHY_SECOND),
                                     .tv_nsec = (long)(remaining % WHY_SECOND)};
            if (nanosleep(&delay, NULL) != 0 && errno != EINTR) {
                perror("nanosleep");
                status = 1;
                stopped = 1;
            }
        }
    }
    display_history(history, timeline);
    why_history_destroy(history);
    why_frame_destroy(&frames[0]);
    why_frame_destroy(&frames[1]);
    return status;
}
