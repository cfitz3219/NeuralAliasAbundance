/*
 * C9b / A2-R03 (second part): asynchronous GPU resource content samples.
 * ABI 0.2 addition (append-only), implemented by the observation producers
 * (host_observe_producers); same platform/packing rules as host_observe.h.
 *
 * Contract:
 *  - A consumer REQUESTS a sample of one subresource (optionally a subrect)
 *    and receives a token. The host arms it; nothing is copied yet.
 *  - The copy happens at the END of the first later game submission on a
 *    DIRECT or COMPUTE queue whose recorded barriers establish the
 *    subresource's state at that point (explicit transitions observed in the
 *    submitted recordings). The host then appends its OWN command list on
 *    that queue, right after the game's lists: transition to COPY_SOURCE,
 *    copy into a host-owned readback buffer, transition back, signal a
 *    host-owned fence. The game's work is never modified or reordered.
 *    A submission without such evidence leaves the request armed; the host
 *    never guesses a state.
 *  - The content is therefore the subresource as the GPU left it at the end
 *    of that submission (queue + ticket reported), not at an arbitrary pass.
 *  - READY only after the host fence has completed (completion evidence, not
 *    a delay). The data stays valid until the consumer RELEASES the token,
 *    or until it EXPIRES (not released within the expiry after READY), or
 *    the device is lost (DEVICE_LOST; no data).
 *  - The host owns every GPU object involved; the consumer only reads the
 *    mapped bytes between a READY poll and release.
 */
#ifndef NA_HOST_OBSERVE_SAMPLES_H
#define NA_HOST_OBSERVE_SAMPLES_H

#include <stdint.h>

#include "host_dx12/host_observe.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum na_sample_status {
    NA_SAMPLE_ARMED = 1,        /* waiting for a submission with state evidence */
    NA_SAMPLE_IN_FLIGHT = 2,    /* copy submitted, host fence not yet complete */
    NA_SAMPLE_READY = 3,        /* data valid until release / expiry */
    NA_SAMPLE_FAILED = 4,       /* see na_sample_info.reason */
    NA_SAMPLE_CANCELLED = 5,
    NA_SAMPLE_EXPIRED = 6,
    NA_SAMPLE_DEVICE_LOST = 7
} na_sample_status;

typedef enum na_sample_reason {
    NA_SAMPLE_OK = 0,
    NA_SAMPLE_E_UNKNOWN_RESOURCE = 1,  /* not an observed resource */
    NA_SAMPLE_E_UNSUPPORTED = 2,       /* buffer / MSAA / reserved / too large */
    NA_SAMPLE_E_RETIRED = 3,           /* resource destroyed before the copy */
    NA_SAMPLE_E_HOST_OBJECTS = 4,      /* host could not create its copy objects */
    NA_SAMPLE_E_TIMEOUT = 5            /* stayed ARMED past arm_timeout_ms */
} na_sample_reason;

typedef struct na_sample_request {
    uint32_t size_bytes;        /* caller: sizeof(na_sample_request) */
    uint32_t subresource;
    na_object_id resource;
    uint32_t x, y, width, height; /* subrect; width == 0 => whole subresource */
    uint32_t arm_timeout_ms;    /* 0 = default 5000 */
    uint32_t expiry_ms;         /* after READY; 0 = default 2000 */
} na_sample_request;

typedef struct na_sample_info {
    uint32_t size_bytes;        /* caller sets capacity; host rewrites */
    uint32_t status;            /* na_sample_status */
    uint32_t reason;            /* na_sample_reason */
    uint32_t format;            /* DXGI_FORMAT of the copied plane */
    na_object_id resource;
    uint32_t subresource;
    uint32_t x, y, width, height; /* the region actually copied */
    uint32_t row_pitch;         /* bytes between rows in `data` */
    uint64_t data_bytes;
    na_object_id queue;         /* submission the copy followed */
    uint64_t queue_ticket;
    uint32_t state_before;      /* the recorded state the copy transitioned from */
    uint32_t _pad;
    const void* data;           /* READY only; valid until release/expiry */
} na_sample_info;

/* Returns NA_OK and a nonzero token, or an NA_E_* error (no token). */
int __cdecl na_observe_sample_request(const na_sample_request* request, uint64_t* out_token);
/* Fills info (size_bytes rule as na_observe_query_status). Any thread. */
int __cdecl na_observe_sample_poll(uint64_t token, na_sample_info* info);
/* Cancels an armed request or releases a finished one; idempotent. */
int __cdecl na_observe_sample_release(uint64_t token);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NA_HOST_OBSERVE_SAMPLES_H */
