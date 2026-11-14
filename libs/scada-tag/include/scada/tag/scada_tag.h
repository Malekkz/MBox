#ifndef SCADA_TAG_SCADA_TAG_H
#define SCADA_TAG_SCADA_TAG_H

/*
 * The C interface of the tag database library
 * (docs/contracts/tag-database.md, section 13). It is the stable interface;
 * this header can be included from C and from C++.
 *
 * Present so far: attach, detach, heartbeat, notify, status, the tag
 * functions (resolve, info, read, write and their batch and string forms),
 * the change rings (scada_sub_*) and the wait sets (scada_waitset_*,
 * without queues). The other functions of section 13 are added by the
 * issues that implement them.
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
 * Wakes the consumers of every change ring the calling thread wrote since
 * its previous call, increments the cycle counter of the process and wakes
 * the threads that wait on it (section 8.4). It also looks again, at most
 * once a second each, for regions of other processes that this context
 * asked for and that were missing, and checks the clocks again at most
 * once a minute.
 *
 * A real-time task calls it once at the end of each cycle, after its last
 * read and write; other code after each batch of writes. A process that
 * reads tags of other processes and writes none still calls it about once
 * a second (or scada_tag_status), or an owner that starts after the reader
 * resolved its tags keeps reading as BAD with OWNER_DOWN.
 *
 * SCADA_OK; SCADA_E_INVALID if `ctx` is NULL; SCADA_E_IO with errno set if
 * the kernel refused a wake, in which case consumers still receive within
 * 100 ms.
 *
 * Bounded, not real-time: one system call that does not block for each
 * ring written and one for the cycle, and the looks for missing regions.
 * Safe from any thread; it acts for the calling thread.
 */
int scada_tag_notify(scada_ctx* ctx);

/*
 * Fills `*out` with the state of the attachment. It also looks again, at
 * most once a second each, for regions of other processes that this context
 * asked for and that were missing, and checks the clocks again at most once
 * a minute, as scada_tag_notify does.
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
 * Reads the tag `handle` into `*out` (section 6.2).
 *
 * RESOLVE THE TAG IN THIS CONTEXT FIRST. A handle is a number that is the
 * same in every process, but the regions of its owner are mapped by
 * scada_tag_resolve. A tag of another process that was never resolved in
 * this context reads as BAD with OWNER_DOWN, as if its owner were down.
 *
 * The quality is BAD with SCADA_FLAG_OWNER_DOWN added when the process of
 * the owner is down or its region is missing. The library judges that
 * itself at every read (section 10.1): the process is not attached, has
 * stopped, or its heartbeat is older than its timeout. The value and
 * times are then the last ones the owner wrote. If the region is missing
 * or the owner died in the middle of a write there is no value: value,
 * times and change_count are 0 and mean nothing; do not plot them, compare
 * them or act on them.
 *
 * SCADA_E_BUSY: the owner was in the middle of a write at each of 64
 * attempts and is not down; keep the previous value for this cycle.
 * SCADA_E_INVALID: an argument is NULL or `handle` is not a handle of this
 * generation. SCADA_E_LAYOUT: the record does not hold a value of the type
 * of the tag; this does not go away by itself.
 *
 * Only SCADA_E_BUSY leaves `*out` unchanged. With every other error `*out`
 * gets value 0, times 0 and quality BAD (and the type of the tag, or 0 if
 * the handle is none), so that a caller that keeps its output does not
 * hold on to a good value.
 *
 * Real-time safe: no allocation, no lock, no system call. Safe from any
 * thread.
 */
int scada_tag_read(scada_ctx* ctx, uint32_t handle, scada_value* out);

/*
 * Reads the `n` tags `handles[0..n)` into `out[0..n)`, as scada_tag_read
 * does, with one reading of the clock for all of them. If `status` is not
 * NULL, `status[i]` gets the return code for `handles[i]`. `out[i]` is
 * unchanged where that code is SCADA_E_BUSY and holds quality BAD where it
 * is another error, so `out` is safe to use without `status`.
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
 * this generation, the quality or a quality flag is none,
 * SCADA_FLAG_OWNER_DOWN is set, or a BOOL value is neither 0 nor 1. On an
 * error nothing is written.
 *
 * Real-time safe. All writes to the tags of one owner come from one
 * thread, the first one that writes them. A library built without NDEBUG
 * stops the process when another thread writes them (section 13.2).
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
 * than SCADA_STRING_MAX.
 *
 * `buf` is unchanged on every error. `*len` and `*meta` are unchanged on
 * SCADA_E_BUSY; with a buffer that is too small `*meta` is unchanged too.
 * With every other error `*len` is 0 and `*meta` gets quality BAD, as for
 * scada_tag_read.
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
 * the quality or a quality flag is none, or SCADA_FLAG_OWNER_DOWN is set.
 * On an error nothing is written.
 *
 * Not real-time. All writes to the tags of one owner come from one thread.
 */
int scada_tag_write_string(scada_ctx* ctx, uint32_t handle, const char* text,
                           size_t len, uint8_t quality, uint16_t flags,
                           int64_t source_time_ns);

/* A subscription to the change ring of one owner (section 8). */
/* NOLINTNEXTLINE(modernize-use-using,readability-identifier-naming): C interface; the name is fixed by the contract, section 13 */
typedef struct scada_sub scada_sub;

/* A set of change rings and cycle counters to wait on together (section
 * 8.4). */
/* NOLINTNEXTLINE(modernize-use-using,readability-identifier-naming): C interface; the name is fixed by the contract, section 13 */
typedef struct scada_waitset scada_waitset;

/* The longest wait of the library: a wait longer than this looks at the
 * ring again at least this often, so that a missed wake delays an entry by
 * at most this much (section 8.4). In ms. */
/* NOLINTNEXTLINE(performance-enum-size): a C enumeration; C gives it the type int */
enum { SCADA_WAIT_SLICE_MS = 100 };

/* One change of a tag, as its owner wrote it (section 8). 32 bytes. */
/* NOLINTNEXTLINE(modernize-use-using): C interface */
typedef struct {
    uint32_t handle;        /* the tag; of the owner of the subscription */
    uint8_t type;           /* SCADA_TYPE_* */
    uint8_t quality;        /* SCADA_QUALITY_* as stored; BAD while the
                               owner's process is down */
    uint16_t quality_flags; /* SCADA_FLAG_* bits as stored; OWNER_DOWN
                               added while the owner's process is down */
    uint64_t value;         /* bit pattern; for a STRING its slot number:
                               read the text with scada_tag_read_string */
    int64_t source_time_ns; /* CLOCK_REALTIME */
    int64_t update_mono_ns; /* CLOCK_MONOTONIC; the same instant as in the
                               tag record */
} scada_change; /* NOLINT(readability-identifier-naming): the name is fixed by the contract, section 13 */

/* Why scada_sub_next returned SCADA_E_GAP (scada_sub_stats_t.last_gap).
 * The action is the same for all of them: read the tags again. The reason
 * is for reporting: an alarm system raises a communication alarm for
 * SCADA_GAP_OWNER_DOWN and clears it for SCADA_GAP_OWNER_BACK. */
/* NOLINTNEXTLINE(performance-enum-size): a C enumeration; C gives it the type int */
enum {
    SCADA_GAP_NONE = 0,       /* no gap yet */
    SCADA_GAP_OVERRUN = 1,    /* fell `capacity` entries behind (8.2) */
    SCADA_GAP_RESTART = 2,    /* the owner attached again: the epoch
                                 changed while it was not seen down */
    SCADA_GAP_RING = 3,       /* the ring appeared, disappeared or was
                                 replaced */
    SCADA_GAP_OWNER_DOWN = 4, /* the owner's process was seen going down */
    SCADA_GAP_OWNER_BACK = 5  /* the owner's process was seen coming back;
                                 this includes a ring that appeared with it */
};

/* What scada_sub_stats reports. 48 bytes. */
/* NOLINTNEXTLINE(modernize-use-using): C interface */
typedef struct {
    uint64_t received;      /* entries delivered with SCADA_OK */
    uint64_t gaps;          /* times SCADA_E_GAP was returned */
    uint64_t lost;          /* entries overwritten before they were read,
                               as far as overruns tell; an owner restart
                               adds none */
    uint64_t invalid;       /* entries refused with SCADA_E_LAYOUT */
    uint32_t epoch;         /* epoch of the ring as last seen */
    uint16_t owner_index;   /* of the owner in the owner table */
    uint16_t process_index; /* of the process that owner is a thread of */
    uint32_t owner_changes; /* times the owner's process was seen going
                               down or coming back; each was a gap */
    uint8_t owner_down;     /* 1 if the owner's process was down at the
                               last look (section 10.1) */
    uint8_t last_gap;       /* SCADA_GAP_*: why the last SCADA_E_GAP was
                               returned, SCADA_GAP_NONE before the first */
    uint8_t reserved[2];    /* 0 */
} scada_sub_stats_t; /* NOLINT(readability-identifier-naming): the name is fixed by the contract, section 13 */

/*
 * Subscribes to the change ring of the owner `owner` and stores the
 * subscription in `*out`. The subscription starts at the end of the ring:
 * it delivers the changes written from now on. To know the state before,
 * read the tags of the owner after this call; drop a change whose
 * update_mono_ns is older than what a read gave for the same tag. If the
 * ring is missing (the owner has not attached yet), the call succeeds and
 * the first look at the ring once it is there gives SCADA_E_GAP.
 *
 * SCADA_E_INVALID: an argument is NULL or `owner` is no owner name.
 * SCADA_E_NOT_FOUND: no owner has this name. SCADA_E_LAYOUT: the ring
 * header gives another capacity than the owner table. On an error `*out`
 * is NULL.
 *
 * Not real-time. Close the subscription before detaching `ctx`.
 */
int scada_sub_open(scada_ctx* ctx, const char* owner, scada_sub** out);

/*
 * Stores the next change of the ring in `*out`, waiting up to `timeout_ms`
 * (0: do not wait) for one (section 8.2). A wait looks at the ring at
 * least every SCADA_WAIT_SLICE_MS, so a missed wake delays a change by
 * that much at most.
 *
 * SCADA_OK: `*out` holds a change. SCADA_E_TIMEOUT: none arrived.
 * SCADA_E_GAP (section 8.3): changes were lost because the subscription
 * fell behind by the capacity of the ring, the owner attached again (the
 * epoch changed), the ring appeared, disappeared or was replaced, or the
 * owner's process was seen going down or coming back (section 10.1). The
 * subscription now continues at the end of the ring; read every tag of
 * the owner you care about again, and drop a change older than that read.
 * Reported once for each such event. SCADA_E_LAYOUT: the next entry is not
 * a valid change of a tag of this owner; it is skipped and counted, and
 * the next call goes on with the entry after it; also, at every call, if
 * the ring header gives another capacity than the owner table.
 * SCADA_E_INVALID: an argument is NULL or `timeout_ms` is negative.
 * SCADA_E_IO with errno set: the kernel refused the wait. `*out` is
 * changed only with SCADA_OK.
 *
 * An owner that dies writes no change; the gap when its process is seen
 * down is what tells the consumer. The tags it then reads give BAD with
 * SCADA_FLAG_OWNER_DOWN, and a consumer that keeps a copy of the values
 * must take those, not hold on to the last good ones. The process is seen
 * down within its heartbeat timeout plus SCADA_WAIT_SLICE_MS of a waiting
 * call, at once after a clean stop. An owner that restarts gives at most
 * two gaps (down, back), one if the subscription did not look while it
 * was down. A change that still arrives while the owner's process is
 * down (its threads may write while its main loop is hung) comes with
 * quality BAD and SCADA_FLAG_OWNER_DOWN, as a read would give it.
 *
 * Not real-time: it may block. One thread uses a subscription.
 */
int scada_sub_next(scada_sub* sub, scada_change* out, int timeout_ms);

/*
 * Fills `*out` with the counts of the subscription.
 *
 * SCADA_OK; SCADA_E_INVALID if an argument is NULL.
 */
int scada_sub_stats(scada_sub* sub, scada_sub_stats_t* out);

/*
 * Closes the subscription; NULL does nothing. It also leaves the wait set
 * it was added to.
 */
void scada_sub_close(scada_sub* sub);

/*
 * Creates an empty wait set of `ctx` and stores it in `*out`.
 *
 * SCADA_E_INVALID: an argument is NULL. On an error `*out` is NULL.
 *
 * Not real-time. Destroy the wait set before detaching `ctx`.
 */
int scada_waitset_create(scada_ctx* ctx, scada_waitset** out);

/*
 * Adds the subscription `sub` to the wait set `set`: scada_waitset_wait returns
 * when `sub` has something to deliver (a change or a gap).
 *
 * SCADA_E_INVALID: an argument is NULL; `sub` belongs to another context
 * or is in a wait set already; the wait set holds 127 sources already; or
 * scada_waitset_fd was called on it.
 */
int scada_waitset_add_sub(scada_waitset* set, scada_sub* sub);

/*
 * Adds the cycle counter of the process `process_index` (section 10):
 * scada_waitset_wait returns when that process has called
 * scada_tag_notify since the previous return, whether or not it changed a
 * tag. The `proc` region of the process is mapped here; while it is
 * missing the source is not ready.
 *
 * SCADA_E_INVALID as for scada_waitset_add_sub, and if `process_index` is
 * no process of the generation.
 */
int scada_waitset_add_cycle(scada_waitset* set, uint16_t process_index);

/*
 * Waits up to `timeout_ms` (0: do not wait) until a source of the wait set
 * is ready, with one system call for all of them (futex_waitv, Linux
 * 5.16). Then call scada_sub_next with timeout 0 on each subscription
 * until it gives SCADA_E_TIMEOUT.
 *
 * SCADA_OK: a source is ready. SCADA_E_TIMEOUT: none became ready.
 * SCADA_E_INVALID: `set` is NULL or `timeout_ms` is negative. SCADA_E_IO
 * with errno set: the kernel refused the wait.
 *
 * Not real-time: it blocks. One thread uses a wait set and its
 * subscriptions.
 */
int scada_waitset_wait(scada_waitset* set, int timeout_ms);

/*
 * Stores in `*descriptor` a file descriptor that becomes readable when a source
 * of the wait set may be ready, for poll and epoll. The first call starts one
 * thread of the library that waits on the sources and turns their wakes
 * into this process-local eventfd; later calls give the same descriptor.
 * Sources cannot be added from then on.
 *
 * When the descriptor is readable: read 8 bytes from it, then call
 * scada_sub_next with timeout 0 on each subscription until it gives
 * SCADA_E_TIMEOUT. A cycle counter that advanced makes it readable too. The
 * descriptor is non-blocking and belongs to the wait set: do not close it.
 *
 * SCADA_E_INVALID: an argument is NULL. SCADA_E_IO with errno set: the
 * eventfd or the thread could not be created.
 *
 * Not real-time.
 */
int scada_waitset_fd(scada_waitset* set, int* descriptor);

/*
 * Destroys the wait set and stops its thread, if any; NULL does nothing.
 * The subscriptions in it stay open.
 */
void scada_waitset_destroy(scada_waitset* set);

#ifdef __cplusplus
}
#endif

#endif /* SCADA_TAG_SCADA_TAG_H */
