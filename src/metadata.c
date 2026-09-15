#include "why.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void why_metadata_retain(WhyMetadata *metadata) {
    if (metadata)
        ++metadata->references;
}

void why_metadata_release(WhyMetadata *metadata) {
    if (metadata && --metadata->references == 0)
        free(metadata);
}

bool why_metadata_equal(const WhyMetadata *a, const WhyMetadata *b) {
    if (!a || !b || a->verified != b->verified)
        return a == b;
    for (size_t i = 0; i < WHY_METADATA_FIELDS; ++i) {
        const WhyMetadataField *x = &a->fields[i], *y = &b->fields[i];
        if (x->length != y->length || x->status != y->status ||
            x->truncated != y->truncated ||
            memcmp(a->data + x->offset, b->data + y->offset, x->length))
            return false;
    }
    return true;
}

static WhyFieldStatus field_error(void) {
    switch (errno) {
    case ENOENT:
    case ESRCH:
        return WHY_FIELD_MISSING;
    case EACCES:
    case EPERM:
        return WHY_FIELD_DENIED;
    default:
        return WHY_FIELD_ERROR;
    }
}

/* Read an extra byte to distinguish exactly-full values from truncation. */
static WhyMetadataField read_field(int directory, const char *name, bool link,
                                   unsigned char *data) {
    WhyMetadataField result = {0};
    ssize_t length;
    if (link) {
        length =
            readlinkat(directory, name, (char *)data, WHY_METADATA_LIMIT + 1);
        if (length < 0) {
            result.status = field_error();
            return result;
        }
    } else {
        int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            result.status = field_error();
            return result;
        }
        size_t used = 0;
        while (used < WHY_METADATA_LIMIT + 1) {
            length = read(fd, data + used, WHY_METADATA_LIMIT + 1 - used);
            if (length < 0 && errno == EINTR)
                continue;
            if (length < 0) {
                result.status = field_error();
                close(fd);
                return result;
            }
            if (!length)
                break;
            used += (size_t)length;
        }
        close(fd);
        length = (ssize_t)used;
    }
    result.truncated = (size_t)length > WHY_METADATA_LIMIT;
    result.length = result.truncated ? WHY_METADATA_LIMIT : (size_t)length;
    result.status = result.length ? WHY_FIELD_OK : WHY_FIELD_EMPTY;
    return result;
}

static bool verify_identity(int directory, WhyIdentity expected) {
    unsigned char data[WHY_METADATA_LIMIT + 1];
    WhyMetadataField field = read_field(directory, "stat", false, data);
    if (field.status != WHY_FIELD_OK || field.truncated)
        return false;
    data[field.length] = '\0';
    WhyProcess check;
    return why_parse_process((char *)data, &check) &&
           why_identity_equal(check.id, expected);
}

bool why_collect_metadata(int directory, WhyProcess *process,
                          const WhyProcess *previous, size_t *budget) {
    const WhyMetadata *old = previous ? previous->metadata : NULL;
    bool same = previous && previous->status == WHY_READ_OK &&
                why_identity_equal(previous->id, process->id);
    uint64_t now = why_now_ns();
    process->metadata_changed =
        same && (strcmp(previous->comm, process->comm) != 0 ||
                 previous->comm_truncated != process->comm_truncated);
    if (same && old && old->verified && now >= old->end_ns &&
        now - old->end_ns < 10 * WHY_SECOND &&
        strcmp(previous->comm, process->comm) == 0 &&
        previous->comm_truncated == process->comm_truncated) {
        if (old->allocation_bytes > *budget) {
            process->metadata_status = WHY_FIELD_BUDGET;
            return true;
        }
        process->metadata = previous->metadata;
        why_metadata_retain(process->metadata);
        *budget -= old->allocation_bytes;
        process->metadata_status = WHY_FIELD_OK;
        return true;
    }
    unsigned char data[WHY_METADATA_FIELDS][WHY_METADATA_LIMIT + 1];
    if (*budget < sizeof(WhyMetadata)) {
        process->metadata_status = WHY_FIELD_BUDGET;
        return true;
    }
    WhyMetadataField fields[WHY_METADATA_FIELDS];
    const char *names[] = {"cmdline", "cwd", "exe"};
    size_t bytes = sizeof(WhyMetadata);
    for (size_t i = 0; i < WHY_METADATA_FIELDS; ++i) {
        fields[i] = read_field(directory, names[i], i != WHY_CMDLINE, data[i]);
        fields[i].offset = bytes - sizeof(WhyMetadata);
        bytes += fields[i].length + 1;
    }
    bool verified = verify_identity(directory, process->id);
    if (!verified) {
        /* Never expose context gathered across an identity change or failed
         * recheck. */
        process->metadata_status = WHY_FIELD_UNVERIFIED;
        return true;
    }
    if (bytes > *budget) {
        process->metadata_status = WHY_FIELD_BUDGET;
        return true;
    }
    WhyMetadata *metadata = calloc(1, bytes);
    if (!metadata)
        return false;
    metadata->references = 1;
    metadata->allocation_bytes = bytes;
    metadata->begin_ns = now;
    metadata->end_ns = why_now_ns();
    metadata->verified = true;
    for (size_t i = 0; i < WHY_METADATA_FIELDS; ++i) {
        metadata->fields[i] = fields[i];
        memcpy(metadata->data + fields[i].offset, data[i], fields[i].length);
    }
    process->metadata = metadata;
    process->metadata_status = WHY_FIELD_OK;
    process->metadata_changed =
        process->metadata_changed ||
        (same && old && !why_metadata_equal(old, metadata));
    *budget -= bytes;
    return true;
}
