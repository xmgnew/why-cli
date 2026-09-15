#include "history.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);    \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

static const char *process_text = "42 (worker ) name) R 1 0 0 0 -1 0 0 0 0 0 "
                                  "120 80 999 999 20 -5 2 0 1000 0 0\n";
static const char *system_text = "cpu 100 0 0 100 0 0 0 0 50 0\n"
                                 "cpu0 100 0 0 100 0 0 0 0 50 0\n"
                                 "intr 123\n";

static void test_parsers(void) {
    WhyProcess p;
    CHECK(why_parse_process(process_text, &p));
    CHECK(p.id.pid == 42 && p.id.start_ticks == 1000 && p.ppid == 1);
    CHECK(strcmp(p.comm, "worker ) name") == 0);
    CHECK(p.user_ticks == 120 && p.system_ticks == 80 && p.threads == 2);
    WhyProcess saved = p;
    CHECK(!why_parse_process("42 (oops) R 1", &p));
    CHECK(memcmp(&p, &saved, sizeof p) == 0);
    CHECK(!why_parse_process("-42 (x) R 1", &p));
    CHECK(!why_parse_process("42 (0123456789012345) R 1", &p));
    CHECK(!why_parse_process("18446744073709551616 (x) R 1", &p));
    CHECK(!why_parse_process("42 (x) ? 1", &p));
    char extended[1024];
    const char *fields = strrchr(process_text, ')') + 1;
    snprintf(extended, sizeof extended, "42 (pool_workqueue_release)%s",
             fields);
    CHECK(why_parse_process(extended, &p));
    CHECK(strcmp(p.comm, "pool_workqueue_release") == 0);
    CHECK(!p.comm_truncated && p.user_ticks == 120);
    char long_name[301];
    memset(long_name, 'w', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = '\0';
    snprintf(extended, sizeof extended, "42 (%s)%s", long_name, fields);
    CHECK(why_parse_process(extended, &p));
    CHECK(p.comm_truncated && strlen(p.comm) == 255 && p.user_ticks == 120);
    WhySystem s;
    CHECK(why_parse_system(system_text, &s));
    CHECK(s.cpu_count == 1 && s.cpus[0] && s.ticks[8] == 50);
    CHECK(!why_parse_system("cpu 1 2\ncpu0 1 2\n", &s));
    CHECK(!why_parse_system("cpu 1 2 3 4 5 6 7 8 9 10\n", &s));
}

static void test_cpu(void) {
    WhyProcess a, b;
    CHECK(why_parse_process(process_text, &a));
    a.begin_ns = a.end_ns = WHY_SECOND;
    b = a;
    b.begin_ns = b.end_ns = 2 * WHY_SECOND;
    b.user_ticks += 200;
    WhyCpu cpu = why_process_cpu(&a, &b, 100);
    CHECK(cpu.validity == WHY_OK && cpu.cpu_seconds == 2 && cpu.cores == 2);
    CHECK(why_process_cpu(NULL, &b, 100).validity == WHY_FIRST_SAMPLE);
    ++b.id.start_ticks;
    CHECK(why_process_cpu(&a, &b, 100).validity == WHY_IDENTITY_CHANGED);
    b.id = a.id;
    b.system_ticks = 1;
    CHECK(why_process_cpu(&a, &b, 100).validity == WHY_COUNTER_RESET);
    b = a;
    b.begin_ns = b.end_ns = 4 * WHY_SECOND;
    CHECK(why_process_cpu(&a, &b, 100).validity == WHY_CLOCK_GAP);
    b.status = WHY_DENIED;
    CHECK(why_process_cpu(&a, &b, 100).validity == WHY_FIRST_SAMPLE);
    WhySystem x, y;
    CHECK(why_parse_system(system_text, &x));
    x.begin_ns = x.end_ns = WHY_SECOND;
    y = x;
    y.begin_ns = y.end_ns = 2 * WHY_SECOND;
    y.ticks[0] += 640;
    y.ticks[3] += 160;
    y.ticks[8] += 500;
    cpu = why_system_cpu(&x, &y, 100);
    CHECK(cpu.validity == WHY_OK && fabs(cpu.busy_pct - 80) < 1e-9);
    CHECK(fabs(cpu.cpu_seconds - 6.4) < 1e-9);
    y.ticks[4] = 0;
    x.ticks[4] = 1;
    CHECK(why_system_cpu(&x, &y, 100).validity == WHY_COUNTER_RESET);
    x.ticks[4] = 0;
    y.cpus[1] = true;
    CHECK(why_system_cpu(&x, &y, 100).validity == WHY_TOPOLOGY_CHANGED);
    y = x;
    y.begin_ns = y.end_ns = 2 * WHY_SECOND;
    CHECK(why_system_cpu(&x, &y, 100).validity == WHY_NO_TICKS);
    y.ticks[4] += 100;
    y.ticks[7] += 100;
    CHECK(why_system_cpu(&x, &y, 100).busy_pct == 0);
    WhyFrame f = {.begin_ns = WHY_SECOND, .boot_ns = WHY_SECOND};
    WhyFrame g = {.begin_ns = 2 * WHY_SECOND, .boot_ns = 20 * WHY_SECOND};
    CHECK(why_frame_discontinuity(&f, &g));
    g.boot_ns = 2 * WHY_SECOND;
    CHECK(!why_frame_discontinuity(&f, &g));
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    CHECK(file != NULL);
    CHECK(fputs(text, file) >= 0);
    CHECK(fclose(file) == 0);
}

static void test_collector(void) {
    char root[] = "why-test-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char system_path[256], child_dir[256], parent_dir[256], child_path[256],
        parent_path[256];
    snprintf(system_path, sizeof system_path, "%s/stat", root);
    snprintf(child_dir, sizeof child_dir, "%s/42", root);
    snprintf(parent_dir, sizeof parent_dir, "%s/1", root);
    snprintf(child_path, sizeof child_path, "%s/42/stat", root);
    snprintf(parent_path, sizeof parent_path, "%s/1/stat", root);
    CHECK(mkdir(child_dir, 0700) == 0 && mkdir(parent_dir, 0700) == 0);
    write_text(system_path, system_text);
    write_text(child_path, process_text);
    write_text(parent_path,
               "1 (parent) S 0 0 0 0 0 0 0 0 0 0 0 0 0 0 20 0 1 0 500\n");
    WhyFrame frame;
    CHECK(why_frame_init(&frame));
    uint32_t cursor = 0;
    CHECK(why_collect(root, &cursor, NULL, &frame));
    CHECK(frame.complete && frame.count == 2);
    const WhyProcess *child = why_find_process(&frame, 42);
    CHECK(child && child->parent_known && child->parent.start_ticks == 500);
    WhyFrame next;
    CHECK(why_frame_init(&next));
    CHECK(why_collect(root, &cursor, &frame, &next));
    CHECK(why_find_process(&next, 42)->metadata == child->metadata);
    WhyHistory *history =
        why_history_create(300 * WHY_SECOND, WHY_HISTORY_BYTES, 8);
    CHECK(history && why_history_append(history, &frame));
    CHECK(why_history_append(history, &next));
    CHECK(why_history_latest(history)->event_count == 0);
    why_frame_destroy(&next);
    CHECK(why_history_latest(history)->frame.processes[1].metadata != NULL);
    why_history_destroy(history);
    frame.processes[0].id.start_ticks = 2000;
    why_resolve_parents(&frame);
    CHECK(!why_find_process(&frame, 42)->parent_known);
    frame.capacity = 1;
    CHECK(why_collect(root, &cursor, NULL, &frame));
    CHECK(!frame.complete && frame.count == 1 && frame.skipped == 1 &&
          frame.processes[0].id.pid == 1);
    CHECK(why_collect(root, &cursor, NULL, &frame));
    CHECK(!frame.complete && frame.processes[0].id.pid == 42);
    frame.capacity = WHY_MAX_PROCESSES;
    cursor = 0;
    write_text(child_path, "truncated");
    CHECK(why_collect(root, &cursor, NULL, &frame));
    CHECK(frame.malformed == 1 &&
          why_find_process(&frame, 42)->status == WHY_MALFORMED);
    CHECK(unlink(child_path) == 0);
    CHECK(why_collect(root, &cursor, NULL, &frame));
    CHECK(frame.gone == 1);
    why_frame_destroy(&frame);
    CHECK(unlink(parent_path) == 0 && unlink(system_path) == 0);
    CHECK(rmdir(child_dir) == 0 && rmdir(parent_dir) == 0 && rmdir(root) == 0);
}

int main(void) {
    test_parsers();
    test_cpu();
    test_collector();
    puts("All parser, CPU, identity, clock, and collector fixture checks "
         "passed.");
    return 0;
}
