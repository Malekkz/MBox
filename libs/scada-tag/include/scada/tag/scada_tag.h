#ifndef SCADA_TAG_SCADA_TAG_H
#define SCADA_TAG_SCADA_TAG_H

/*
 * The C interface of the tag database library
 * (docs/contracts/tag-database.md, section 13). It is the stable interface;
 * this header can be included from C and from C++.
 *
 * Present so far: attach, observe, detach, heartbeat, notify, status, the
 * owner and process functions (scada_owner_info, scada_proc_resolve,
 * scada_proc_status), the tag functions (resolve, info, read, write and
 * their batch and string forms), the change rings (scada_sub_*), the
 * reliable queues (scada_queue_*) and the wait sets (scada_waitset_*). The
 * other functions of section 13 are added by the issues that implement
 * them.
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

/* The process index of an observer context, which is no process
 * (section 11.4): scada_status.process_index of an observer. */
#define SCADA_NO_PROCESS UINT16_C(0xFFFF)

/* What scada_tag_status reports. 32 bytes. */
/* NOLINTNEXTLINE(modernize-use-using): C interface */
typedef struct {
    uint64_t generation;       /* number of the generation attached to */
    uint32_t generation_state; /* SCADA_GENERATION_*, or 0 if not valid */
    uint16_t process_index;    /* of the calling process in the process
                                  table; SCADA_NO_PROCESS for an observer */
    uint8_t tagd_down;         /* 1 if scada-tagd is down (section 10.1) */
    uint8_t clock_syscall;     /* 1 if reading the clocks is a system call on
                                  this machine, so that the functions marked
                                  real-time safe are not (section 13.1) */
    /* The three counts below are 0 for an observer, which has no regions
       of its own. */
    uint32_t regions_created;  /* own regions the attach created */
    uint32_t regions_reused;   /* own regions the attach found and reused */
    uint32_t records_repaired; /* tag records repaired at attach (6.3) */
    uint16_t owner_count;      /* owners of the generation (owner indexes
                                  0 to owner_count - 1) */
    uint16_t process_count;    /* processes of the generation (process
                                  indexes 0 to process_count - 1) */
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
 * Opens an observer context on the current generation and stores it in
 * `*out` (section 11.4). An observer is no process: it needs no entry in
 * the process table and may run as any user who can read the database
 * (section 3.1: a member of the group scada, or root). It makes the checks
 * of attach step 1 (base directory, `current`, generation directory,
 * header, layout version) and maps every region it reads read-only. It
 * creates no file, takes no lock and writes nothing to shared memory, so
 * any number of observers may run beside the processes of the generation.
 *
 * In an observer context: every function that reads works as for a
 * process that owns no tag; scada_tag_write, scada_tag_write_many and
 * scada_tag_write_string return SCADA_E_NOT_OWNER (SCADA_E_INVALID for a
 * handle that is none); scada_tag_heartbeat and scada_tag_notify return
 * SCADA_E_INVALID and do nothing. scada_tag_status takes over what notify
 * does for a process: call it about once a second. It also reports a
 * generation change, which an observer does not see through a heartbeat:
 * SCADA_E_RETIRING when the generation is RETIRING or `current` names
 * another one. The observer then detaches, observes again and resolves
 * its handles again (section 11.3).
 *
 * SCADA_E_INVALID: `out` is NULL, or tagd_uid is neither -1 nor a uid.
 * SCADA_E_PERMISSION: the caller may not read the base directory or the
 * generation directory, or one of them or the header has the wrong owner
 * or mode. SCADA_E_LAYOUT: magic or layout version of the header are not
 * those of this library, or the header is not valid. SCADA_E_RETIRING:
 * the generation is retiring and no new one was found. SCADA_E_IO with
 * errno set: ENOENT if there is no base directory or no current
 * generation, EAGAIN if the generation is not running. On an error `*out`
 * is NULL.
 *
 * Not real-time.
 */
int scada_tag_observe(scada_ctx** out);

/* As scada_tag_observe, with the base directory and the uid of scada-tagd
 * replaced (section 3: for tests). `retain_dir` is not used. */
int scada_tag_observe_opts(const scada_attach_opts* opts, scada_ctx** out);

/*
 * Detaches and frees `ctx`; NULL does nothing. The process reads as down at
 * once (section 10.1). Its regions stay for its next attach. An observer
 * context writes nothing here either. No other thread may use `ctx` during
 * or after the call.
 */
void scada_tag_detach(scada_ctx* ctx);

/*
 * Writes the heartbeat of the process (section 10.1). Call it from the main
 * loop or cycle of the process, never from a helper thread.
 *
 * A heartbeat that comes more than the heartbeat timeout after the one
 * before is counted as late in the `proc` region, with the longest gap, and
 * scada-tagd logs it: readers saw the process down meanwhile. The figures
 * are a lower bound: the gap is measured between clock readings. Safe from
 * several threads: the published time never goes back.
 *
 * SCADA_OK; SCADA_E_RETIRING if the generation is retiring: the heartbeat
 * was written, and the caller detaches and attaches again (section 11.3);
 * SCADA_E_INVALID if `ctx` is NULL or an observer, which has no heartbeat.
 *
 * Real-time safe: no allocation, no lock, no system call.
 */
int scada_tag_heartbeat(scada_ctx* ctx);

/*
 * Wakes the consumers of every change ring the calling thread wrote and
 * every queue it posted to since its previous call, increments the cycle
 * counter of the process and wakes the threads that wait on it (section 8.4).
 * It also looks again, at most once a second each, for regions of other
 * processes that this context asked for and that were missing, and checks the
 * clocks again at most once a minute.
 *
 * A real-time task calls it once at the end of each cycle, after its last
 * read, write and post; other code after each batch of writes and after
 * each post. A process that
 * reads tags of other processes and writes none still calls it about once
 * a second (or scada_tag_status), or an owner that starts after the reader
 * resolved its tags keeps reading as BAD with OWNER_DOWN.
 *
 * SCADA_OK; SCADA_E_INVALID if `ctx` is NULL or an observer, which wakes
 * nobody and calls scada_tag_status instead; SCADA_E_IO with errno set if
 * the kernel refused a wake, in which case consumers still receive within
 * 100 ms.
 *
 * Bounded, not real-time: one system call that does not block for each
 * ring written and queue posted to and one for the cycle, and the looks for
 * missing regions. Safe from any thread; it acts for the calling thread.
 */
int scada_tag_notify(scada_ctx* ctx);

/*
 * Fills `*out` with the state of the attachment. It also looks again, at
 * most once a second each, for regions of other processes that this context
 * asked for and that were missing, and checks the clocks again at most once
 * a minute, as scada_tag_notify does. In an observer context it also reads
 * `current` again, at most once a second.
 *
 * SCADA_OK; SCADA_E_RETIRING if the generation is retiring, or, for an
 * observer, if `current` names another generation;
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

/* What scada_owner_info reports about an owner (section 5). 48 bytes. */
/* NOLINTNEXTLINE(modernize-use-using): C interface */
typedef struct {
    char name[SCADA_OWNER_NAME_SIZE]; /* the owner name, NUL-terminated */
    uint32_t tag_count;               /* its tags: slots 0 to tag_count - 1 */
    uint32_t change_ring_capacity;    /* entries of its change ring */
    uint16_t process_index;           /* the process it is a thread of */
    uint8_t owned;                    /* 1 if the calling process is that
                                         process */
    uint8_t reserved0;                /* 0 */
    uint32_t reserved1;               /* 0 */
} scada_owner_info_t; /* NOLINT(readability-identifier-naming): named like the other structures of section 13 */

/*
 * Fills `*out` with name, tag count, ring capacity and process of the
 * owner `owner_index` (0 to scada_status.owner_count - 1). With
 * scada_tag_handle and scada_tag_info it lists every tag of the
 * generation.
 *
 * SCADA_E_INVALID: an argument is NULL or `owner_index` is no owner of
 * this generation.
 *
 * Not real-time.
 */
int scada_owner_info(scada_ctx* ctx, uint16_t owner_index,
                     scada_owner_info_t* out);

/*
 * The handle of the tag in slot `slot` of owner `owner_index` (section
 * 5.1), or SCADA_INVALID_HANDLE if the two do not fit a handle. Whether the
 * tag exists is checked by the function the handle is passed to; slots 0
 * to scada_owner_info_t.tag_count - 1 exist.
 *
 * Real-time safe; needs no context.
 */
uint32_t scada_tag_handle(uint16_t owner_index, uint32_t slot);

/* Why scada_proc_status reports a process as down (section 10.1): the
 * first rule that holds. */
/* NOLINTNEXTLINE(performance-enum-size): a C enumeration; C gives it the type int */
enum {
    SCADA_DOWN_NONE = 0,            /* not down */
    SCADA_DOWN_REGION_MISSING = 1,  /* its proc region is missing, unusable
                                       or not ready */
    SCADA_DOWN_NOT_RUNNING = 2,     /* its state is not RUNNING */
    SCADA_DOWN_HEARTBEAT_STALE = 3, /* its heartbeat is older than its
                                       timeout, or too far ahead */
    SCADA_DOWN_NOT_RESOLVED = 4     /* not judged: the process was not
                                       resolved in this context; with
                                       SCADA_E_INVALID */
};

/* Values of scada_proc_status_t.state (section 10). */
/* NOLINTNEXTLINE(performance-enum-size): a C enumeration; C gives it the type int */
enum {
    SCADA_PROC_STARTING = 1,
    SCADA_PROC_RUNNING = 2,
    SCADA_PROC_STOPPING = 3
};

/* What scada_proc_status reports about a process (section 10). 88 bytes. */
/* NOLINTNEXTLINE(modernize-use-using): C interface */
typedef struct {
    char name[SCADA_OWNER_NAME_SIZE]; /* the process name, NUL-terminated */
    uint64_t heartbeat;               /* heartbeat counter; 0 if no region */
    int64_t heartbeat_mono_ns;        /* CLOCK_MONOTONIC of the last
                                         heartbeat; 0 if no region */
    int64_t heartbeat_age_ns;         /* time since then, never negative; 0
                                         if no region */
    uint32_t heartbeat_timeout_ms;    /* from the process table */
    uint32_t state;                   /* SCADA_PROC_* as stored; 0 if no
                                         region */
    uint32_t cycle_seq;               /* advanced by scada_tag_notify */
    uint32_t pid;                     /* as the process stored it; for
                                         diagnosis only */
    uint32_t uid;                     /* from the process table */
    uint16_t process_index;           /* in the process table */
    uint8_t down;                     /* 1 if down by the rules of section
                                         10.1, as the library judges it now */
    uint8_t down_reason;              /* SCADA_DOWN_* */
    uint8_t marked_down;              /* 1 if scada-tagd has set the `down`
                                         flag of the process table; lags up
                                         to one check interval */
    uint8_t realtime;                 /* 1 if the process table marks it
                                         REALTIME */
    /* NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): size of a reserved field, fixed by the contract */
    uint8_t reserved[6]; /* 0 */
} scada_proc_status_t; /* NOLINT(readability-identifier-naming): the name is fixed by the contract, section 13 */

/*
 * Stores in `*index` the process index of the process `process_name` and
 * maps its proc region read-only, so that scada_proc_status makes no system
 * call. A region that is not there yet is looked for again by
 * scada_tag_notify and scada_tag_status, at most once a second.
 *
 * SCADA_E_NOT_FOUND: no process has this name. SCADA_E_INVALID: an
 * argument is NULL or `process_name` is no process name.
 *
 * Not real-time.
 */
int scada_proc_resolve(scada_ctx* ctx, const char* process_name,
                       uint16_t* index);

/*
 * Fills `*out` with the state of the process `index` (0 to
 * scada_status.process_count - 1) now. Resolve it in this context first
 * (scada_proc_resolve); the caller's own process and scada-tagd need no
 * resolve.
 *
 * The fields of the proc region are read one by one; they belong to the
 * same heartbeat only as far as diagnosis needs.
 *
 * SCADA_E_INVALID: an argument is NULL, `index` is no process of this
 * generation, or the process was not resolved in this context. In the last
 * case `*out` still gets the fields of the process table (name, uid,
 * timeout, realtime flag, index, the `down` flag of scada-tagd), with down
 * 1 and down_reason SCADA_DOWN_NOT_RESOLVED, so that a caller that does
 * not check the code does not take it for a process that is up; resolve
 * it by that name.
 *
 * Real-time safe: no allocation, no lock, no system call. Safe from any
 * thread.
 */
int scada_proc_status(scada_ctx* ctx, uint16_t index, scada_proc_status_t* out);

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
 * scada_tag_resolve. A tag of another process none of whose tags was
 * resolved in this context gives SCADA_E_INVALID, so that a forgotten
 * resolve does not look like a dead owner.
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
 * SCADA_E_INVALID: an argument is NULL, `handle` is not a handle of this
 * generation, or its owner is another process of which no tag was
 * resolved in this context. SCADA_E_LAYOUT: the record does not hold a
 * value of the type of the tag; this does not go away by itself.
 *
 * Only SCADA_E_BUSY leaves `*out` unchanged. With every other error `*out`
 * gets value 0, times 0 and quality BAD (and the type of the tag, or 0
 * with SCADA_E_INVALID), so that a caller that keeps its output does not
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
 * `buf` NULL with `cap` 0 asks for the length alone: `*len` and `*meta`
 * are filled as with a buffer that is large enough, and nothing else is
 * written.
 *
 * SCADA_E_TYPE: the tag is not a STRING. SCADA_E_INVALID: `ctx` or `len`
 * is NULL, `buf` is NULL with `cap` above 0, `handle` is not a handle of
 * this generation or was not resolved as for scada_tag_read, or `cap` is
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

/* A set of change rings, queues and cycle counters to wait on together
 * (section 8.4). */
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
 * update_mono_ns is older than what a read gave for the same tag. This is
 * not a resolve: resolve a tag of the owner before reading its tags, or a
 * read gives SCADA_E_INVALID (section 6.2). If the
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

/* One side of a reliable queue (section 9): the producer or the consumer. */
/* NOLINTNEXTLINE(modernize-use-using,readability-identifier-naming): C interface; the name is fixed by the contract, section 13 */
typedef struct scada_queue scada_queue;

/* Values of scada_event_record.kind (section 9.4). */
/* NOLINTNEXTLINE(performance-enum-size): a C enumeration; C gives it the type int */
enum {
    SCADA_EVENT_ALARM = 1,
    SCADA_EVENT_COMMAND = 2,
    SCADA_EVENT_AUDIT = 3,
    SCADA_EVENT_SYSTEM = 4
};

/* Sizes of the records (section 9.4), in bytes. */
/* NOLINTNEXTLINE(performance-enum-size): a C enumeration; C gives it the type int */
enum {
    SCADA_EVENT_PAYLOAD_MAX = 232,
    SCADA_EVENT_RECORD_SIZE = 256,
    SCADA_WRITE_REQUEST_SIZE = 64,
    SCADA_WRITE_RESULT_SIZE = 32
};

/* Values of scada_write_request.operation (section 9.4). */
/* NOLINTNEXTLINE(performance-enum-size): a C enumeration; C gives it the type int */
enum {
    SCADA_OP_DIRECT = 1,
    SCADA_OP_SELECT = 2,
    SCADA_OP_OPERATE = 3,
    SCADA_OP_CANCEL = 4,
    SCADA_OP_READ_NOW = 5
};

/* Values of scada_write_request.source (section 9.4). */
/* NOLINTNEXTLINE(performance-enum-size): a C enumeration; C gives it the type int */
enum {
    SCADA_SOURCE_AUTO = 1,
    SCADA_SOURCE_OPERATOR = 2,
    SCADA_SOURCE_REMOTE = 3
};

/* Values of scada_write_result.status (section 9.4). */
/* NOLINTNEXTLINE(performance-enum-size): a C enumeration; C gives it the type int */
enum {
    SCADA_RESULT_OK = 1,
    SCADA_RESULT_REJECTED = 2,
    SCADA_RESULT_TIMEOUT = 3,
    SCADA_RESULT_DEVICE_ERROR = 4,
    SCADA_RESULT_NOT_AUTHORISED = 5,
    SCADA_RESULT_EXPIRED = 6,
    SCADA_RESULT_ABORTED = 7
};

/* A record of a queue of kind EVENTS (events.*). 256 bytes. */
/* NOLINTNEXTLINE(modernize-use-using): C interface */
typedef struct {
    uint64_t seq;           /* set by scada_queue_post: the id of the
                               record, unique in the queue for the life of
                               the generation */
    int64_t time_ns;        /* CLOCK_REALTIME */
    uint16_t kind;          /* SCADA_EVENT_* */
    uint16_t payload_len;   /* at most SCADA_EVENT_PAYLOAD_MAX */
    uint32_t source_handle; /* the tag the event concerns, or
                               SCADA_INVALID_HANDLE */
    /* NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): a field of a contract structure (coding standard, section 4) */
    uint8_t payload[SCADA_EVENT_PAYLOAD_MAX]; /* serialised message */
} scada_event_record; /* NOLINT(readability-identifier-naming): a C name of the contract's EventRecord */

/* A record of a queue of kind CMD (cmd.<driver>.<source>). 64 bytes. Every
 * reserved field is 0. */
/* NOLINTNEXTLINE(modernize-use-using): C interface */
typedef struct {
    uint64_t request_id;        /* set by scada_queue_post:
                                   (queue id << 48) | position */
    uint32_t target_handle;     /* the tag to write */
    uint8_t operation;          /* SCADA_OP_* */
    uint8_t source;             /* SCADA_SOURCE_* */
    uint8_t value_type;         /* SCADA_TYPE_* but not STRING; 0 for CANCEL
                                   and READ_NOW, which carry no value */
    uint8_t reserved0;          /* 0 */
    uint64_t value;             /* bit pattern; 0 without a value type */
    uint32_t user_id;           /* not 0 for OPERATOR, 0 for AUTO and REMOTE */
    uint32_t reserved1;         /* 0 */
    int64_t expiry_mono_ns;     /* CLOCK_MONOTONIC; the driver must not
                                   execute the request after it */
    int64_t posted_mono_ns;     /* set by scada_queue_post to the time of the
                                   post */
    uint64_t select_request_id; /* for OPERATE and CANCEL the SELECT they
                                   belong to; 0 otherwise */
    /* NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): a field of a contract structure (coding standard, section 4), of the size the contract gives */
    uint8_t reserved[8]; /* 0 */
} scada_write_request; /* NOLINT(readability-identifier-naming): a C name of the contract's WriteRequest */

/* A record of a queue of kind RESULT (result.<driver>.<source>). 32 bytes.
 * Every reserved field is 0. */
/* NOLINTNEXTLINE(modernize-use-using): C interface */
typedef struct {
    uint64_t request_id; /* of the request answered */
    uint8_t status;      /* SCADA_RESULT_* */
    /* NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): a field of a contract structure (coding standard, section 4) */
    uint8_t reserved0[3];      /* 0 */
    int32_t protocol_error;    /* driver-specific, 0 if none */
    int64_t completed_time_ns; /* CLOCK_REALTIME */
    /* NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): a field of a contract structure (coding standard, section 4), of the size the contract gives */
    uint8_t reserved[8]; /* 0 */
} scada_write_result; /* NOLINT(readability-identifier-naming): a C name of the contract's WriteResult */

/* The longest lifetime of a write request, expiry_mono_ns - posted_mono_ns,
 * in ms (section 14). */
/* NOLINTNEXTLINE(performance-enum-size): a C enumeration; C gives it the type int */
enum { SCADA_REQUEST_LIFETIME_MAX_MS = 60000 };

/*
 * Opens the side of the reliable queue `queue_name` that the calling
 * process has in the queue table, producer or consumer, and stores it in
 * `*out`. The regions it needs are mapped here. The queue region of a
 * producer that has not attached yet is looked for again while
 * scada_queue_next waits.
 *
 * SCADA_E_INVALID: an argument is NULL, `queue_name` is no queue name, or
 * the queue has the calling process at both ends. SCADA_E_NOT_FOUND: no
 * queue has this name. SCADA_E_NOT_OWNER: the calling process is neither
 * producer nor consumer of the queue, or `ctx` is an observer context (for
 * any `queue_name`, before anything is mapped; section 11.4), so an
 * observer has no queue to post to, acknowledge or add to a wait set.
 * SCADA_E_BUSY: this side of the queue
 * is open in this context already. On an error `*out` is NULL.
 *
 * Not real-time. Close the queue before detaching `ctx`.
 */
int scada_queue_open(scada_ctx* ctx, const char* queue_name, scada_queue** out);

/*
 * Posts the record `record` of `size` bytes to the queue (section 9.1):
 * a scada_event_record, scada_write_request or scada_write_result, as the
 * kind of the queue says. The library sets the id (seq, request_id) and,
 * for a request, posted_mono_ns; the caller sets the rest. The consumer is
 * not woken: call scada_tag_notify afterwards (section 8.4).
 *
 * SCADA_OK: the record is in the queue and will be delivered. For an event
 * this is what "journalled" means (section 9.3): an action that needs an
 * event record must not be taken unless its post returned SCADA_OK.
 * SCADA_E_PEER_DOWN: the consumer's process is down or not attached, as
 * this process judges it (section 10.1), or its cursor is not valid;
 * nothing is queued. SCADA_E_FULL: the queue holds `capacity` records the
 * consumer has not finished with; nothing is queued. SCADA_E_INVALID: an
 * argument is NULL, `queue` is the consumer's side, `size` is not the record
 * size of the queue, or the record is not valid: a kind, operation,
 * source, value type or status that is none, a payload longer than
 * SCADA_EVENT_PAYLOAD_MAX, a reserved field that is not 0, a BOOL value
 * other than 0 or 1, a user id for AUTO or REMOTE or none (0) for
 * OPERATOR, a select_request_id
 * where it does not belong or missing where it does, or a request that
 * has expired or would live longer than SCADA_REQUEST_LIFETIME_MAX_MS.
 *
 * Real-time safe: no allocation, no lock, no system call. One thread posts
 * to a queue; a library built without NDEBUG stops the process when a
 * second thread posts (section 13.2).
 */
int scada_queue_post(scada_queue* queue, const void* record, size_t size);

/*
 * Copies the next record of the queue into `record` (`size` bytes, the
 * record size of the queue), waiting up to `timeout_ms` (0: do not wait)
 * for one (section 9.2). A wait looks at the queue at least every
 * SCADA_WAIT_SLICE_MS.
 *
 * EVENTS and RESULT (at least once): the record stays in the queue until
 * scada_queue_ack; until then every call returns the same record again.
 * After a crash before the ack it is delivered again, so a consumer
 * ignores an id (seq, request_id) it has already stored.
 *
 * CMD (at most once): the request is taken from the queue before this
 * call returns, so it is never delivered again, also not after a crash.
 * Acknowledge nothing. A request that has expired, or whose lifetime is
 * longer than SCADA_REQUEST_LIFETIME_MAX_MS, is never returned: it is
 * answered SCADA_RESULT_EXPIRED in the result queue of the same driver and
 * source, and the next one is looked at. The thread that calls this
 * function on a cmd queue is therefore the one that posts to that result
 * queue. Time passes after the return: check expiry_mono_ns again right
 * before acting on the device.
 *
 * SCADA_OK: `record` holds a record. SCADA_E_TIMEOUT: none arrived.
 * SCADA_E_LAYOUT: the next record is not valid (its id is not its
 * position, or a check of scada_queue_post fails); it is passed over and
 * the next call goes on with the one after it. Also when the queue is
 * stopped: its cursor and position are no valid pair, or its file was
 * replaced inside the generation (section 9.2). Nothing is consumed then,
 * the call returns only when `timeout_ms` has passed, so that a loop over
 * this call does not spin, and the queue stays stopped until a tag-set
 * change or a reboot; log it once. SCADA_E_INVALID: an argument is NULL,
 * `queue` is the producer's side, `size` is not the record size, or
 * `timeout_ms` is negative. SCADA_E_IO with errno set: the kernel refused the
 * wait. `record` is changed only with SCADA_OK.
 *
 * Not real-time: it may block. One thread uses the consumer's side.
 */
int scada_queue_next(scada_queue* queue, void* record, size_t size,
                     int timeout_ms);

/*
 * Acknowledges the record the last scada_queue_next returned: it is
 * processed and is not delivered again (section 9.2). The producer may
 * reuse its place from now on.
 *
 * SCADA_OK; SCADA_E_INVALID if `queue` is NULL, is the producer's side or a
 * CMD queue, or no record is waiting for its ack.
 *
 * Not real-time, but makes no system call. One thread uses the consumer's
 * side.
 */
int scada_queue_ack(scada_queue* queue);

/* What scada_queue_stats reports: the counts of one side of a queue in one
 * context, since attach. A count belongs to the side that makes it. 80
 * bytes. */
/* NOLINTNEXTLINE(modernize-use-using): C interface */
typedef struct {
    uint64_t posted;          /* producer: records accepted by post; the
                                 consumer of a cmd queue: none (its answers
                                 count on the result queue's producer) */
    uint64_t delivered;       /* consumer: records next returned with
                                 SCADA_OK, a record delivered again
                                 included */
    uint64_t expired;         /* consumer of a cmd queue: requests not
                                 returned because they had expired or lived
                                 too long, answered EXPIRED (9.2) */
    uint64_t aborted;         /* consumer of a cmd queue: requests drained
                                 at attach and answered ABORTED (9.2) */
    uint64_t skipped;         /* consumer: records attach passed over
                                 undelivered: those a new proc region
                                 skipped (9.2) and those of a cmd queue
                                 whose cursor was out of range. For an
                                 events queue these are lost audit
                                 records */
    uint64_t answers_dropped; /* consumer of a cmd queue: EXPIRED and
                                 ABORTED answers that could not be posted
                                 (result queue full, requester down, no
                                 result queue, or another thread posts
                                 there); the requester sees them expire */
    uint64_t invalid;         /* consumer: records passed over with
                                 SCADA_E_LAYOUT because they were not
                                 valid */
    uint64_t out_of_range;    /* consumer: looks that found the cursor and
                                 the queue's position out of range; the
                                 queue is stopped until the next
                                 generation */
    uint64_t replaced;        /* consumer: 1 if the queue's file was found
                                 replaced, removed or shrunk inside the
                                 generation; the queue is dead from then on
                                 and the producer gets SCADA_E_PEER_DOWN.
                                 Producer: 1 if attach created the queue's
                                 file while the consumer's cursor was not
                                 0, that is anew inside the generation; log
                                 it as a WARNING */
    uint16_t queue_id;        /* the queue's id in the queue table */
    uint16_t kind;            /* 6 EVENTS, 7 CMD, 8 RESULT */
    uint8_t producer;         /* 1 for the producer's side, 0 for the
                                 consumer's */
    /* NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): a C structure of fixed size */
    uint8_t reserved[3]; /* 0 */
} scada_queue_stats_t; /* NOLINT(readability-identifier-naming): named like scada_sub_stats_t of the contract, section 13 */

/*
 * Fills `*out` with the counts of the side `queue` of a queue in its
 * context, including what attach did before the queue was opened.
 *
 * SCADA_OK; SCADA_E_INVALID if an argument is NULL. Safe from any thread.
 */
int scada_queue_stats(scada_queue* queue, scada_queue_stats_t* out);

/*
 * Closes the side of the queue; NULL does nothing. It also leaves the wait
 * set it was added to. A record delivered and not acknowledged is
 * delivered again by the next open. Any thread may close it. A queue stays
 * bound to the thread that posts to it while a side that posts to it is
 * open in this context: its producer side and, for a result queue, the
 * consumer side of its cmd queue. Once the last of them is closed, another
 * thread may post to it.
 */
void scada_queue_close(scada_queue* queue);

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
 * Adds the consumer's side of the queue `queue` to the wait set `set`:
 * scada_waitset_wait returns when `queue` has a record to deliver. A wait
 * set never reports a stopped queue (section 9.2) as ready, so a consumer
 * that calls scada_queue_next only after a wake learns of the stop from
 * scada_queue_stats or from a timed call of scada_queue_next.
 *
 * SCADA_E_INVALID: an argument is NULL; `queue` is the producer's side,
 * belongs to another context or is in a wait set already; the wait set
 * holds 127 sources already; or scada_waitset_fd was called on it.
 */
int scada_waitset_add_queue(scada_waitset* set, scada_queue* queue);

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
 * 5.16). Then call scada_sub_next and scada_queue_next with timeout 0 on
 * each subscription and queue until it gives SCADA_E_TIMEOUT.
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
 * scada_sub_next and scada_queue_next with timeout 0 on each subscription
 * and queue until it gives SCADA_E_TIMEOUT. A cycle counter that advanced makes
 * it readable too. The descriptor is non-blocking and belongs to the wait set:
 * do not close it.
 *
 * SCADA_E_INVALID: an argument is NULL. SCADA_E_IO with errno set: the
 * eventfd or the thread could not be created.
 *
 * Not real-time.
 */
int scada_waitset_fd(scada_waitset* set, int* descriptor);

/*
 * Destroys the wait set and stops its thread, if any; NULL does nothing.
 * The subscriptions and queues in it stay open.
 */
void scada_waitset_destroy(scada_waitset* set);

#ifdef __cplusplus
}
#endif

#endif /* SCADA_TAG_SCADA_TAG_H */
