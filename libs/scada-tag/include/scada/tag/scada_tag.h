#ifndef SCADA_TAG_SCADA_TAG_H
#define SCADA_TAG_SCADA_TAG_H

/*
 * The C interface of the tag database library
 * (docs/contracts/tag-database.md, section 13). It is the stable interface;
 * this header can be included from C and from C++.
 *
 * Present so far: attach, detach, heartbeat, status, and the tag functions
 * (resolve, info, read, write and their batch and string forms). The other
 * functions of section 13 are added by the issues that implement them.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes (section 13.3). Every function that returns int returns one
 * of them. */
/* NOLINTNEXTLINE(performance-enum-size): a C enumeration; C gives it the type int */
enum {
    SCADA_OK = 0,
    SCADA_E_INVALID = 1,     /* invalid argument or handle */
    SCADA_E_LAYOUT = 2,      /* layout version or magic mismatch, or a header
                                that is not valid */
    SCADA_E_NOT_FOUND = 3,   /* unknown tag, owner, process or queue name */
    SCADA_E_NOT_OWNER = 4,   /* write by a process that does not own the tag */
    SCADA_E_TYPE = 5,        /* value type does not match the tag type */
    SCADA_E_FULL = 6,        /* reliable queue is full */
    SCADA_E_PEER_DOWN = 7,   /* the queue's consumer is not attached or down */
    SCADA_E_GAP = 8,         /* change ring overrun or owner restart */
    SCADA_E_TIMEOUT = 9,     /* nothing arrived within the timeout */
    SCADA_E_RETIRING = 10,   /* the generation is retiring; reattach */
    SCADA_E_TAGD_DOWN = 11,  /* scada-tagd heartbeat lost */
    SCADA_E_PERMISSION = 12, /* uid mismatch, or a directory or region has
                                the wrong owner or mode */
    SCADA_E_IO = 13,         /* operating system error; errno is set */
    SCADA_E_BUSY = 14        /* the writer was mid-write for the whole read
                                attempt; or the process name is already
                                attached */
};

/* One process attached to the tag database. */
/* NOLINTNEXTLINE(modernize-use-using,readability-identifier-naming): C interface; the name is fixed by the contract, section 13 */
typedef struct scada_ctx scada_ctx;

/* Options of scada_tag_attach_opts (section 13). Set every field. */
/* NOLINTNEXTLINE(modernize-use-using): C interface */
typedef struct {
    const char* base_dir;   /* NULL: /dev/shm/scada */
    const char* retain_dir; /* NULL: /var/lib/scada/retain */
    int64_t tagd_uid;       /* -1: look up the user scada-tagd */
} scada_attach_opts; /* NOLINT(readability-identifier-naming): the name is fixed by the contract, section 13 */

/* Values of scada_status.generation_state. */
/* NOLINTNEXTLINE(performance-enum-size): a C enumeration; C gives it the type int */
enum {
    SCADA_GENERATION_BUILDING = 1,
    SCADA_GENERATION_RUNNING = 2,
    SCADA_GENERATION_RETIRING = 3
};

/* What scada_tag_status reports. 32 bytes. */
/* NOLINTNEXTLINE(modernize-use-using): C interface */
typedef struct {
    uint64_t generation;       /* number of the generation attached to */
    uint32_t generation_state; /* SCADA_GENERATION_*, or 0 if not valid */
    uint16_t process_index;    /* of the calling process in the process table */
    uint8_t tagd_down;         /* 1 if scada-tagd is down (section 10.1) */
    uint8_t clock_syscall;     /* 1 if reading the clocks is a system call on
                                  this machine, so that the functions marked
                                  real-time safe are not (section 13.1) */
    uint32_t regions_created;  /* own regions the attach created */
    uint32_t regions_reused;   /* own regions the attach found and reused */
    uint32_t records_repaired; /* tag records repaired at attach (6.3) */
    uint32_t reserved1;        /* 0 */
} scada_status; /* NOLINT(readability-identifier-naming): the name is fixed by the contract, section 13 */

/*
 * Attaches the calling process to the current generation as `process_name`
 * and stores the context in `*out` (section 11.2). The process must be
 * listed in the process table and run as the user given there. Its own
 * regions are created, or reused and repaired. From a successful return on
 * the caller must call scada_tag_heartbeat at an interval of at most one
 * third of its heartbeat timeout.
 *
 * SCADA_E_INVALID: an argument is NULL, the name is not a process name, or
 * tagd_uid is neither -1 nor a uid. SCADA_E_NOT_FOUND: the process is not in
 * the process table. SCADA_E_PERMISSION: the process runs as the wrong
 * user, or the base directory, the generation directory, the header or an
 * own region has the wrong owner or mode. SCADA_E_BUSY: a process of this
 * name is already attached. SCADA_E_LAYOUT: magic or layout version of the
 * header are not those of this library, or the header is not valid.
 * SCADA_E_RETIRING: the generation is retiring and no new one was found.
 * SCADA_E_IO: an error of the operating system, with errno set: ENOENT if
 * there is no base directory or no current generation, ENOSPC if the file
 * system has no room for the regions. On an error `*out` is NULL.
 *
 * Not real-time.
 */
int scada_tag_attach(const char* process_name, scada_ctx** out);

/* As scada_tag_attach, with the directories and the uid of scada-tagd
 * replaced (section 3: for tests). */
int scada_tag_attach_opts(const char* process_name,
                          const scada_attach_opts* opts, scada_ctx** out);

/*
 * Detaches and frees `ctx`; NULL does nothing. The process reads as down at
 * once (section 10.1). Its regions stay for its next attach. No other
 * thread may use `ctx` during or after the call.
 */
void scada_tag_detach(scada_ctx* ctx);

/*
 * Writes the heartbeat of the process (section 10.1). Call it from the main
 * loop or cycle of the process, never from a helper thread.
 *
 * SCADA_OK; SCADA_E_RETIRING if the generation is retiring: the heartbeat
 * was written, and the caller detaches and attaches again (section 11.3);
 * SCADA_E_INVALID if `ctx` is NULL.
 *
 * Real-time safe: no allocation, no lock, no system call.
 */
int scada_tag_heartbeat(scada_ctx* ctx);

/*
 * Fills `*out` with the state of the attachment. It also looks again, at
 * most once a second each, for regions of other processes that this context
 * asked for and that were missing.
 *
 * SCADA_OK; SCADA_E_RETIRING if the generation is retiring;
 * SCADA_E_TAGD_DOWN if the generation is not retiring and scada-tagd is
 * down, in which case existing mappings keep working. `*out` is filled in
 * all three cases. SCADA_E_INVALID if an argument is NULL.
 *
 * Not real-time.
 */
int scada_tag_status(scada_ctx* ctx, scada_status* out);

/* The handle that refers to no tag (section 5.1). */
#define SCADA_INVALID_HANDLE UINT32_C(0xFFFFFFFF)

/* The longest tag name and the longest string value, in bytes (section
 * 14), and the sizes of the name fields of scada_tag_info_t. */
/* NOLINTNEXTLINE(performance-enum-size): a C enumeration; C gives it the type int */
enum {
    SCADA_TAG_NAME_MAX = 127,
    SCADA_STRING_MAX = 250,
    SCADA_TAG_NAME_SIZE = 128,
    SCADA_OWNER_NAME_SIZE = 32
};

/* Values of scada_value.type (section 6). */
/* NOLINTNEXTLINE(performance-enum-size): a C enumeration; C gives it the type int */
enum {
    SCADA_TYPE_BOOL = 1,    /* value is 0 or 1 */
    SCADA_TYPE_INT64 = 2,   /* value holds the bits of an int64_t */
    SCADA_TYPE_UINT64 = 3,  /* value is the number */
    SCADA_TYPE_FLOAT64 = 4, /* value holds the bits of an IEEE 754 double */
    SCADA_TYPE_STRING = 5   /* value is the number of the string slot */
};

/* Values of scada_value.quality (section 6). */
/* NOLINTNEXTLINE(performance-enum-size): a C enumeration; C gives it the type int */
enum {
    SCADA_QUALITY_GOOD = 0,
    SCADA_QUALITY_UNCERTAIN = 1,
    SCADA_QUALITY_BAD = 2
};

/* Bits of scada_value.quality_flags (section 6). */
/* NOLINTNEXTLINE(performance-enum-size): a C enumeration; C gives it the type int */
enum {
    SCADA_FLAG_STALE = 1 << 0,
    SCADA_FLAG_COMM_FAIL = 1 << 1,
    SCADA_FLAG_OUT_OF_RANGE = 1 << 2,
    SCADA_FLAG_SUBSTITUTED = 1 << 3,
    SCADA_FLAG_NOT_INIT = 1 << 4,
    SCADA_FLAG_OWNER_DOWN = 1 << 5, /* set by a read, never stored */
    SCADA_FLAG_TIME_UNCERTAIN = 1 << 6,
    SCADA_FLAG_BLOCKED = 1 << 7,
    SCADA_FLAG_TEST = 1 << 8,
    SCADA_FLAG_OVERFLOW = 1 << 9,
    SCADA_FLAG_RESTORED = 1 << 10,
    SCADA_FLAG_LOCAL_TIME = 1 << 11 /* source_time_ns was set by the owner */
};

/* Bits of scada_tag_info_t.flags (section 5). */
/* NOLINTNEXTLINE(performance-enum-size): a C enumeration; C gives it the type int */
enum {
    SCADA_TAG_RETAIN = 1 << 0,
    SCADA_TAG_WRITABLE_BY_COMMAND = 1 << 1,
    SCADA_TAG_RETAIN_NOW = 1 << 2
};

/* One value of a tag with its quality and times (section 13). 48 bytes. */
/* NOLINTNEXTLINE(modernize-use-using): C interface */
typedef struct {
    uint8_t type;           /* SCADA_TYPE_* */
    uint8_t quality;        /* SCADA_QUALITY_* */
    uint16_t quality_flags; /* SCADA_FLAG_* bits */
    uint32_t change_count;  /* filled by read: how often value, quality or
                               flags changed; wraps */
    uint64_t value;         /* bit pattern, see SCADA_TYPE_* */
    int64_t source_time_ns; /* CLOCK_REALTIME; on write: 0 means "no device
                               time" */
    int64_t update_time_ns; /* CLOCK_REALTIME; filled by read, ignored by
                               write */
    int64_t update_mono_ns; /* CLOCK_MONOTONIC; filled by read, ignored by
                               write */
    int64_t age_ns;         /* filled by read: time since update_mono_ns,
                               never negative */
} scada_value; /* NOLINT(readability-identifier-naming): the name is fixed by the contract, section 13 */

/* What scada_tag_info reports about a tag. 168 bytes. */
/* NOLINTNEXTLINE(modernize-use-using): C interface */
typedef struct {
    uint8_t type;           /* SCADA_TYPE_* */
    uint8_t flags;          /* SCADA_TAG_* bits */
    uint8_t owned;          /* 1 if the calling process owns the tag and
                               may write it */
    uint8_t reserved0;      /* 0 */
    uint16_t owner_index;   /* of its owner in the owner table */
    uint16_t process_index; /* of the process that owner is a thread of */
    char name[SCADA_TAG_NAME_SIZE];    /* the tag name, NUL-terminated */
    char owner[SCADA_OWNER_NAME_SIZE]; /* the owner name, NUL-terminated */
} scada_tag_info_t; /* NOLINT(readability-identifier-naming): the name is fixed by the contract, section 13 */

/*
 * Stores in `*handle` the handle of the tag `name` (section 5.1). The
 * handle is valid in this context until it is detached. If another process
 * owns the tag, its regions are mapped read-only and brought into memory
 * here, so that the first read makes no system call; regions that are not
 * there yet are looked for again later, and the tag reads as BAD with
 * OWNER_DOWN meanwhile.
 *
 * SCADA_E_NOT_FOUND: no tag has this name. SCADA_E_INVALID: an argument is
 * NULL or `name` is no tag name. On an error `*handle` is
 * SCADA_INVALID_HANDLE.
 *
 * Not real-time.
 */
int scada_tag_resolve(scada_ctx* ctx, const char* name, uint32_t* handle);

/*
 * Fills `*out` with name, type, flags and owner of the tag `handle`.
 *
 * SCADA_E_INVALID: an argument is NULL or `handle` is not a handle of this
 * generation. SCADA_E_LAYOUT: the entry of the tag in the header is not
 * valid.
 *
 * Not real-time.
 */
int scada_tag_info(scada_ctx* ctx, uint32_t handle, scada_tag_info_t* out);

/*
 * Reads the tag `handle` into `*out` (section 6.2). Resolve the tag in this
 * context first: a tag of another process that was never resolved here
 * reads as BAD with OWNER_DOWN.
 *
 * The quality is BAD with SCADA_FLAG_OWNER_DOWN added when the process of
 * the owner is down or its region is missing. The library judges that
 * itself at every read (section 10.1): the process is not attached, has
 * stopped, or its heartbeat is older than its timeout. The value and
 * times are then the last ones the owner wrote, or 0 if the region is
 * missing or the owner died in the middle of a write.
 *
 * SCADA_E_BUSY: the owner was in the middle of a write at each of 64
 * attempts and is not down; keep the previous value for this cycle.
 * SCADA_E_INVALID: an argument is NULL or `handle` is not a handle of this
 * generation. SCADA_E_LAYOUT: the record does not hold a value of the type
 * of the tag. On an error `*out` is unchanged.
 *
 * Real-time safe: no allocation, no lock, no system call. Safe from any
 * thread.
 */
int scada_tag_read(scada_ctx* ctx, uint32_t handle, scada_value* out);

/*
 * Reads the `n` tags `handles[0..n)` into `out[0..n)`, as scada_tag_read
 * does, with one reading of the clock for all of them. If `status` is not
 * NULL, `status[i]` gets the return code for `handles[i]`. `out[i]` is
 * unchanged where that code is not SCADA_OK.
 *
 * SCADA_OK if every tag was read, otherwise the code of the first one that
 * was not. SCADA_E_INVALID also if `ctx` is NULL, or `n` is not 0 and
 * `handles` or `out` is NULL; nothing is read then.
 *
 * Real-time safe. Safe from any thread.
 */
int scada_tag_read_many(scada_ctx* ctx, const uint32_t* handles, size_t n,
                        scada_value* out, int* status);

/*
 * Writes `*value` to the tag `handle` (section 6.1). It uses `type`, `value`,
 * `quality`, `quality_flags` and `source_time_ns`; the library sets the
 * update times. If `source_time_ns` is 0, the source time is the update
 * time and SCADA_FLAG_LOCAL_TIME is set. A STRING tag is written with
 * scada_tag_write_string.
 *
 * SCADA_E_NOT_OWNER: the calling process does not own the tag.
 * SCADA_E_TYPE: `value->type` is not the type of the tag, or the tag is a
 * STRING. SCADA_E_INVALID: an argument is NULL, `handle` is not a handle of
 * this generation, the quality or a quality flag is none, or a BOOL value
 * is neither 0 nor 1. On an error nothing is written.
 *
 * Real-time safe. All writes to the tags of one owner come from one thread.
 */
int scada_tag_write(scada_ctx* ctx, uint32_t handle, const scada_value* value);

/*
 * Writes `values[0..n)` to the tags `handles[0..n)`, as scada_tag_write does,
 * with one reading of the clocks for all of them. All of them are checked
 * first: on an error, which is the code of the first pair that is not
 * valid, nothing is written.
 *
 * Real-time safe. All writes to the tags of one owner come from one thread.
 */
int scada_tag_write_many(scada_ctx* ctx, const uint32_t* handles, size_t n,
                         const scada_value* values);

/*
 * Reads the STRING tag `handle`: the text goes to `buf[0..*len)`, which is
 * not NUL-terminated, and, if `meta` is not NULL, what scada_tag_read gives
 * goes to `*meta` (section 7). Text, quality and times belong to the same
 * write. The text is empty where scada_tag_read gives the value 0 for a
 * tag whose owner is down. The text is what the owner stored; a reader
 * that needs valid UTF-8 checks it.
 *
 * SCADA_E_TYPE: the tag is not a STRING. SCADA_E_INVALID: `ctx`, `buf` or
 * `len` is NULL, `handle` is not a handle of this generation, or `cap` is
 * less than the length of the text, which is then in `*len`; a buffer of
 * SCADA_STRING_MAX bytes is always enough. SCADA_E_BUSY and SCADA_E_LAYOUT
 * as for scada_tag_read; SCADA_E_LAYOUT also if the stored length is more
 * than SCADA_STRING_MAX. On an error `buf` and `*meta` are unchanged.
 *
 * Not real-time. Safe from any thread.
 */
int scada_tag_read_string(scada_ctx* ctx, uint32_t handle, char* buf,
                          size_t cap, size_t* len, scada_value* meta);

/*
 * Writes the text `text[0..len)` with `quality`, `flags` and `source_time_ns`
 * to the STRING tag `handle` (section 7). `source_time_ns` 0 is treated as
 * by scada_tag_write.
 *
 * SCADA_E_NOT_OWNER: the calling process does not own the tag.
 * SCADA_E_TYPE: the tag is not a STRING. SCADA_E_INVALID: `ctx` is NULL,
 * `text` is NULL and `len` is not 0, `len` is more than SCADA_STRING_MAX, the
 * text is not valid UTF-8, `handle` is not a handle of this generation, or
 * the quality or a quality flag is none. On an error nothing is written.
 *
 * Not real-time. All writes to the tags of one owner come from one thread.
 */
int scada_tag_write_string(scada_ctx* ctx, uint32_t handle, const char* text,
                           size_t len, uint8_t quality, uint16_t flags,
                           int64_t source_time_ns);

#ifdef __cplusplus
}
#endif

#endif /* SCADA_TAG_SCADA_TAG_H */
