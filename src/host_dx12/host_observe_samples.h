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
 *    a delay); a host-owned worker (never a game callback) observes the
 *    fence, copies the readback into host memory and frees the GPU objects.
 *  - Consumers never receive a borrowed pointer (C9B-R5): na_observe_sample_read
 *    copies the READY bytes into the CALLER's buffer, which the caller owns.
 *    READY data is kept until release or expiry (not read within expiry_ms).
 *  - Eligibility (C9B-R4): every list of the submission must be observed with
 *    complete hook coverage and complete barrier evidence (no truncated batch,
 *    no evidence overflow); the subresource's last recorded transition must be
 *    a completed (not BEGIN_ONLY) transition to a state other than COMMON
 *    (COMMON permits implicit promotion); simultaneous-access resources are
 *    never sampled.
 *  - Ordering (C9B-R3): while any request is armed, the host serializes each
 *    observed submission on that queue with the copy that may follow it, so
 *    nothing observed can be submitted between them; an occurrence found
 *    ORDER_AMBIGUOUS is never sampled. queue/queue_ticket name it exactly.
 *  - Lifetime (C9B-R2): the source is acquired from a submission that just
 *    referenced it (the application must keep it alive while that work runs)
 *    and held until the host fence completes.
 *  - Leases are scoped to the session and epoch of the request: stop or
 *    resync ends them (SESSION_ENDED).
 *  - The host owns every GPU object involved.
 *
 * Packed depth-stencil (D24S8, D32S8X24 and typeless families): the request
 * names a plane-0 subresource (mip/array index); only the depth plane is
 * transitioned and copied. na_sample_info.format describes the copied bytes:
 * R24_UNORM_X8_TYPELESS (D24S8 family: 24-bit UNORM depth in the low bits of
 * each 32-bit texel) or R32_FLOAT (D32S8X24 family). Both families' plane-0
 * footprints are R32_TYPELESS in D3D12, which would be ambiguous.
 *
 * Buffer ranges (C10, execution-time constant data): for a BUFFER resource,
 * x | (y << 32) is the byte offset, width the byte count (1..65536), height 0
 * or 1, subresource 0. Only UPLOAD-heap buffers (permanently GENERIC_READ, not
 * GPU-writable) are copied, at the end of the first clean submission whose
 * observed recordings bound a constant-buffer range containing it. The copy
 * is queued before the application can signal its own fence for that work,
 * so the application may not rewrite the range until the copy has run: the
 * bytes are those the submission's GPU work read (unlike NA_EV_SNAPSHOT's
 * APP_SYNC_UNKNOWN CPU bytes). Reported: format 0, width = bytes, height 1,
 * row_pitch = bytes, state_before GENERIC_READ, queue/queue_ticket of that
 * submission. Other heaps: NA_SAMPLE_E_UNSUPPORTED.
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
    NA_SAMPLE_E_UNSUPPORTED = 2,       /* non-upload buffer / MSAA / reserved / too large */
    NA_SAMPLE_E_RETIRED = 3,           /* resource destroyed before the copy */
    NA_SAMPLE_E_HOST_OBJECTS = 4,      /* host could not create its copy objects */
    NA_SAMPLE_E_TIMEOUT = 5,           /* stayed ARMED past arm_timeout_ms */
    NA_SAMPLE_E_SESSION_ENDED = 6,     /* observation stopped or resynced */
    NA_SAMPLE_E_SUBMIT_FAILED = 7      /* host fence signal failed after the copy was
                                          submitted: objects are quarantined until the
                                          device is removed, never freed early */
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
    na_session_id session;      /* scope of the lease */
    na_epoch epoch;
} na_sample_info;

/* Returns NA_OK and a nonzero token, or an NA_E_* error (no token). */
int __cdecl na_observe_sample_request(const na_sample_request* request, uint64_t* out_token);
/* Fills info (size_bytes rule as na_observe_query_status). Any thread. */
int __cdecl na_observe_sample_poll(uint64_t token, na_sample_info* info);
/* Copies READY data (data_bytes, rows row_pitch apart) into the caller's
 * buffer; NA_E_STATE unless READY, NA_E_ARG if capacity is too small. */
int __cdecl na_observe_sample_read(uint64_t token, void* dst, uint64_t capacity, uint64_t* written);
/* Cancels an armed request or releases a finished one; idempotent. A sample
 * whose copy may still run on the GPU is freed only after completion. */
int __cdecl na_observe_sample_release(uint64_t token);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NA_HOST_OBSERVE_SAMPLES_H */
