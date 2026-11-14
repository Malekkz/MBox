#ifndef SCADA_TAG_SCADA_TAG_H
#define SCADA_TAG_SCADA_TAG_H

/*
 * The C interface of the tag database library
 * (docs/contracts/tag-database.md, section 13). It is the stable interface;
 * this header can be included from C and from C++.
 *
 * Present so far: attach, detach, heartbeat and status. The other functions
 * of section 13 are added by the issues that implement them.
 */

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
    SCADA_E_BUSY = 14        /* the process name is already attached */
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
    uint8_t reserved0;         /* 0 */
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

#ifdef __cplusplus
}
#endif

#endif /* SCADA_TAG_SCADA_TAG_H */
