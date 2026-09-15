#ifndef WHY_H
#define WHY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WHY_MAX_PROCESSES 32768U
#define WHY_MAX_CPUS 8192U
#define WHY_SECOND UINT64_C(1000000000)
#define WHY_METADATA_LIMIT 4096U
#define WHY_METADATA_FRAME_BUDGET (8U * 1024U * 1024U)

typedef enum {
    WHY_FIELD_OK,
    WHY_FIELD_EMPTY,
    WHY_FIELD_MISSING,
    WHY_FIELD_DENIED,
    WHY_FIELD_ERROR,
    WHY_FIELD_UNVERIFIED,
    WHY_FIELD_BUDGET
} WhyFieldStatus;

typedef enum { WHY_CMDLINE, WHY_CWD, WHY_EXE, WHY_METADATA_FIELDS } WhyField;

typedef struct {
    size_t offset, length;
    WhyFieldStatus status;
    bool truncated;
} WhyMetadataField;

/* Immutable apart from references. Data includes embedded NULs in cmdline. */
typedef struct WhyMetadata {
    size_t references, allocation_bytes;
    uint64_t begin_ns, end_ns;
    bool verified;
    WhyMetadataField fields[WHY_METADATA_FIELDS];
    unsigned char data[];
} WhyMetadata;

typedef struct {
    uint32_t pid;
    uint64_t start_ticks;
} WhyIdentity;

typedef enum {
    WHY_OK,
    WHY_FIRST_SAMPLE,
    WHY_IDENTITY_CHANGED,
    WHY_CLOCK_GAP,
    WHY_COUNTER_RESET,
    WHY_TOPOLOGY_CHANGED,
    WHY_NO_TICKS
} WhyValidity;

typedef enum {
    WHY_READ_OK,
    WHY_GONE,
    WHY_DENIED,
    WHY_MALFORMED,
    WHY_IO_ERROR
} WhyReadStatus;

typedef struct {
    WhyIdentity id;
    uint32_t ppid;
    uint64_t user_ticks, system_ticks, threads;
    uint64_t begin_ns, end_ns;
    char comm[256];
    bool comm_truncated;
    char state;
    WhyReadStatus status;
    bool parent_known;
    WhyIdentity parent;
    WhyMetadata *metadata;
    WhyMetadata *parent_metadata;
    WhyFieldStatus metadata_status;
    bool metadata_changed;
} WhyProcess;

typedef struct {
    /* user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice
     */
    uint64_t ticks[10];
    bool cpus[WHY_MAX_CPUS];
    size_t cpu_count;
    uint64_t begin_ns, end_ns;
} WhySystem;

typedef struct {
    WhySystem system;
    WhyProcess *processes;
    size_t count, capacity;
    size_t gone, denied, malformed, io_errors, skipped;
    uint64_t begin_ns, end_ns, boot_ns, realtime_ns;
    bool complete;
    size_t metadata_skipped;
} WhyFrame;

typedef struct {
    WhyValidity validity;
    double cpu_seconds, cores, busy_pct;
} WhyCpu;

/* Parsers commit output only on success. Input must be NUL terminated. */
bool why_parse_process(const char *text, WhyProcess *out);
bool why_parse_system(const char *text, WhySystem *out);
bool why_identity_equal(WhyIdentity a, WhyIdentity b);
WhyCpu why_process_cpu(const WhyProcess *before, const WhyProcess *after,
                       long hz);
WhyCpu why_system_cpu(const WhySystem *before, const WhySystem *after, long hz);
const char *why_validity_name(WhyValidity validity);
void why_resolve_parents(WhyFrame *frame);
const WhyProcess *why_find_process(const WhyFrame *frame, uint32_t pid);

/* Caller owns frames. Init once, reuse for scans, destroy once. */
bool why_frame_init(WhyFrame *frame);
void why_frame_destroy(WhyFrame *frame);
/* root is normally /proc; fixtures may supply another proc-shaped directory. */
bool why_collect(const char *root, uint32_t *cursor, const WhyFrame *previous,
                 WhyFrame *frame);
uint64_t why_now_ns(void);
bool why_frame_discontinuity(const WhyFrame *before, const WhyFrame *after);

void why_metadata_retain(WhyMetadata *metadata);
void why_metadata_release(WhyMetadata *metadata);
bool why_metadata_equal(const WhyMetadata *a, const WhyMetadata *b);
/* Internal collector helper: directory must be the FD used for the first stat.
 */
bool why_collect_metadata(int directory, WhyProcess *process,
                          const WhyProcess *previous, size_t *budget);
void why_frame_clear(WhyFrame *frame);

#endif
