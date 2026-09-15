#include "why.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t clock_ns(clockid_t clock) {
    struct timespec ts;
    if (clock_gettime(clock, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * WHY_SECOND + (uint64_t)ts.tv_nsec;
}

uint64_t why_now_ns(void) { return clock_ns(CLOCK_MONOTONIC); }

bool why_frame_init(WhyFrame *frame) {
    *frame = (WhyFrame){0};
    frame->processes = calloc(WHY_MAX_PROCESSES, sizeof *frame->processes);
    if (!frame->processes)
        return false;
    frame->capacity = WHY_MAX_PROCESSES;
    return true;
}

void why_frame_destroy(WhyFrame *frame) {
    why_frame_clear(frame);
    free(frame->processes);
    *frame = (WhyFrame){0};
}

void why_frame_clear(WhyFrame *frame) {
    for (size_t i = 0; i < frame->count; ++i) {
        why_metadata_release(frame->processes[i].metadata);
        why_metadata_release(frame->processes[i].parent_metadata);
    }
    WhyProcess *storage = frame->processes;
    size_t capacity = frame->capacity;
    *frame = (WhyFrame){.processes = storage, .capacity = capacity};
}

/* Read at most capacity - 1 bytes, detecting truncation instead of parsing it.
 */
static bool read_file(int directory, const char *name, char *buffer,
                      size_t capacity) {
    int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return false;
    size_t used = 0;
    bool ok = false;
    for (;;) {
        ssize_t n = read(fd, buffer + used, capacity - 1 - used);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0) {
            buffer[used] = '\0';
            ok = true;
            break;
        }
        used += (size_t)n;
        if (used == capacity - 1) {
            errno = EOVERFLOW;
            break;
        }
    }
    int saved = errno;
    close(fd);
    errno = saved;
    return ok;
}

static WhyReadStatus read_status(void) {
    if (errno == ENOENT || errno == ESRCH)
        return WHY_GONE;
    if (errno == EACCES || errno == EPERM)
        return WHY_DENIED;
    if (errno == EOVERFLOW)
        return WHY_MALFORMED;
    return WHY_IO_ERROR;
}

static bool read_process(int root, WhyProcess *process,
                         const WhyProcess *previous, size_t *metadata_budget) {
    char name[32], buffer[4096];
    snprintf(name, sizeof name, "%u", process->id.pid);
    process->begin_ns = why_now_ns();
    int directory =
        openat(root, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    bool ok =
        directory >= 0 && read_file(directory, "stat", buffer, sizeof buffer);
    WhyReadStatus status = ok ? WHY_READ_OK : read_status();
    uint64_t end = why_now_ns();
    WhyProcess parsed;
    if (ok && (!why_parse_process(buffer, &parsed) ||
               parsed.id.pid != process->id.pid))
        status = WHY_MALFORMED;
    if (status == WHY_READ_OK) {
        parsed.begin_ns = process->begin_ns;
        *process = parsed;
    }
    process->end_ns = end;
    process->status = status;
    bool success =
        status != WHY_READ_OK ||
        why_collect_metadata(directory, process, previous, metadata_budget);
    if (directory >= 0)
        close(directory);
    return success;
}

static int compare_process(const void *a, const void *b) {
    const WhyProcess *pa = a, *pb = b;
    return (pa->id.pid > pb->id.pid) - (pa->id.pid < pb->id.pid);
}

/* A bounded max heap keeps the lowest PIDs after the rotation cursor. */
static void select_pid(WhyFrame *frame, uint32_t pid) {
    size_t i;
    if (frame->count < frame->capacity) {
        i = frame->count++;
        while (i && frame->processes[(i - 1) / 2].id.pid < pid) {
            frame->processes[i] = frame->processes[(i - 1) / 2];
            i = (i - 1) / 2;
        }
    } else {
        if (pid >= frame->processes[0].id.pid)
            return;
        i = 0;
        while (2 * i + 1 < frame->count) {
            size_t child = 2 * i + 1;
            if (child + 1 < frame->count && frame->processes[child + 1].id.pid >
                                                frame->processes[child].id.pid)
                ++child;
            if (frame->processes[child].id.pid <= pid)
                break;
            frame->processes[i] = frame->processes[child];
            i = child;
        }
    }
    frame->processes[i] = (WhyProcess){.id.pid = pid};
}

static bool enumerate(DIR *directory, uint32_t cursor, WhyFrame *frame,
                      size_t *total) {
    *total = 0;
    frame->count = 0;
    rewinddir(directory);
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry)
            return errno == 0;
        if (entry->d_name[0] < '1' || entry->d_name[0] > '9')
            continue;
        char *end;
        errno = 0;
        unsigned long pid = strtoul(entry->d_name, &end, 10);
        if (errno || *end || pid > UINT32_MAX)
            continue;
        ++*total;
        if (pid > cursor)
            select_pid(frame, (uint32_t)pid);
    }
}

bool why_collect(const char *root, uint32_t *cursor, const WhyFrame *previous,
                 WhyFrame *frame) {
    if (previous == frame || !frame->processes || !frame->capacity) {
        errno = EINVAL;
        return false;
    }
    why_frame_clear(frame);
    frame->begin_ns = why_now_ns();
#ifdef __linux__
    frame->boot_ns = clock_ns(CLOCK_BOOTTIME);
#else
    frame->boot_ns = frame->begin_ns;
#endif
    frame->realtime_ns = clock_ns(CLOCK_REALTIME);
    DIR *directory = opendir(root);
    if (!directory)
        return false;
    /* Large but bounded: supports WHY_MAX_CPUS modern cpu lines. */
    const size_t system_capacity = 4U * 1024U * 1024U;
    char *buffer = malloc(system_capacity);
    bool ok = false;
    if (!buffer)
        goto done;
    uint64_t begin = why_now_ns();
    if (!read_file(dirfd(directory), "stat", buffer, system_capacity))
        goto done;
    uint64_t end = why_now_ns();
    if (!why_parse_system(buffer, &frame->system)) {
        errno = EINVAL;
        goto done;
    }
    frame->system.begin_ns = begin;
    frame->system.end_ns = end;
    size_t total;
    frame->complete = enumerate(directory, *cursor, frame, &total);
    if (frame->count == 0 && *cursor != 0 && frame->complete)
        frame->complete = enumerate(directory, 0, frame, &total);
    frame->skipped = total >= frame->count ? total - frame->count : 0;
    frame->complete = frame->complete && frame->skipped == 0;
    qsort(frame->processes, frame->count, sizeof *frame->processes,
          compare_process);
    *cursor = frame->skipped && frame->count
                  ? frame->processes[frame->count - 1].id.pid
                  : 0;
    size_t metadata_budget = WHY_METADATA_FRAME_BUDGET;
    for (size_t i = 0; i < frame->count; ++i) {
        const WhyProcess *old =
            previous ? why_find_process(previous, frame->processes[i].id.pid)
                     : NULL;
        if (!read_process(dirfd(directory), &frame->processes[i], old,
                          &metadata_budget))
            goto done;
        if (frame->processes[i].status == WHY_READ_OK &&
            !frame->processes[i].metadata)
            ++frame->metadata_skipped;
        switch (frame->processes[i].status) {
        case WHY_READ_OK:
            break;
        case WHY_GONE:
            ++frame->gone;
            break;
        case WHY_DENIED:
            ++frame->denied;
            break;
        case WHY_MALFORMED:
            ++frame->malformed;
            break;
        case WHY_IO_ERROR:
            ++frame->io_errors;
            break;
        }
    }
    why_resolve_parents(frame);
    frame->end_ns = why_now_ns();
    ok = frame->begin_ns && frame->end_ns && frame->boot_ns &&
         frame->realtime_ns;
    if (!ok)
        errno = EIO;
done:;
    int saved = errno;
    free(buffer);
    closedir(directory);
    errno = saved;
    return ok;
}

bool why_frame_discontinuity(const WhyFrame *before, const WhyFrame *after) {
    if (after->begin_ns <= before->begin_ns || after->boot_ns < before->boot_ns)
        return true;
    uint64_t mono = after->begin_ns - before->begin_ns;
    uint64_t boot = after->boot_ns - before->boot_ns;
    uint64_t difference = boot > mono ? boot - mono : mono - boot;
    return difference > WHY_SECOND / 10;
}
