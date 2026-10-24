/*
 * B-002 â€” DX12 host observation ABI. *** FROZEN at ABI 0.1 (2026-09-23) ***
 * Accepted by Route B at rev3 (B-002-REV3-ACK.md, route-b commit 73d2980).
 * Layout/semantics changes now require a MAJOR bump via a new
 * COLLABORATION.md proposal; MINOR additions are append-only per the rules
 * below. Companion prose: OBSERVATION-API.md.
 *
 * Platform/packing contract (R2):
 *  - x64 Windows only, little-endian, MSVC layout, NO bitfields.
 *  - Every struct uses natural alignment; every event's total size is a
 *    multiple of 8 and events are packed back-to-back at 8-byte alignment.
 *  - All functions are __cdecl. All reserved/padding bytes are written as 0
 *    by the host and ignored by consumers.
 *
 * Decode contract (R1/R2): a consumer can decode every H1 event from this
 * header alone. Events advance by na_event_header.size_bytes. An event whose
 * `kind` is unknown is SKIPPED via size_bytes; if an unknown kind carries
 * NA_EVF_FOUNDATIONAL, the consumer must treat its inventory as incomplete
 * (unknown foundational facts exist) rather than silently trusting what it
 * has. A known event whose size_bytes is SMALLER than this header's struct
 * (for the consumer's compiled minor) is malformed: stop decoding the batch
 * and treat it as loss. Counts implying reads past the batch payload are
 * malformed the same way. Fields appended by future MINOR revisions live
 * between the compiled struct end and size_bytes; inline arrays are located
 * ONLY via explicit offset/stride fields, never by struct end.
 */
#ifndef NA_HOST_OBSERVE_H
#define NA_HOST_OBSERVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NA_OBSERVE_ABI_MAJOR 0
#define NA_OBSERVE_ABI_MINOR 2 /* 0.2 (C8): H2 kinds 24-26 appended */

typedef uint64_t na_object_id;  /* opaque, monotonic, never reused in a session */
typedef uint64_t na_epoch;
typedef uint64_t na_session_id; /* random per na_observe_start (R4): epochs are
                                   only comparable within one session_id */

/* ---- Capability groups (R1): index for status loss counters and the
 * capability report. Values are frozen for MAJOR 0. ---- */
typedef enum na_cap_group {
    NA_GROUP_SESSION          = 0,
    NA_GROUP_RESOURCE         = 1,
    NA_GROUP_DESCRIPTOR       = 2,
    NA_GROUP_LIST             = 3,
    NA_GROUP_QUEUE            = 4,
    NA_GROUP_SWAPCHAIN        = 5,
    NA_GROUP_PIPELINE         = 6,  /* H2 */
    NA_GROUP_PASS             = 7,  /* H2 */
    NA_GROUP_RESOURCE_USE     = 8,  /* H2 */
    NA_GROUP_CPU_CONSTANTS    = 9,  /* H2/H3 */
    NA_GROUP_SNAPSHOT         = 10, /* H3 */
    NA_GROUP_COUNT_V1         = 11  /* remaining slots reserved, zeroed */
} na_cap_group;

typedef enum na_cap_support {
    NA_CAP_UNSUPPORTED = 0,
    NA_CAP_PARTIAL     = 1,
    NA_CAP_SUPPORTED   = 2
} na_cap_support;

/* ---- Event kinds (H1, complete set for ABI 0.1) ---- */
typedef enum na_event_kind {
    NA_EV_CAPABILITY_REPORT       = 1,
    NA_EV_RESOURCE_CREATED        = 2,
    NA_EV_OBJECT_RETIRED          = 3,  /* any tracked object (R1) */
    NA_EV_DESCRIPTOR_WRITE        = 4,
    NA_EV_DESCRIPTOR_COPY         = 5,
    NA_EV_RESOURCE_HEAP_CREATED   = 6,
    NA_EV_DESCRIPTOR_HEAP_CREATED = 7,  /* distinct from resource heaps (R1) */
    NA_EV_LIST_CREATED            = 8,
    NA_EV_LIST_RESET              = 9,
    NA_EV_LIST_CLOSE              = 10,
    NA_EV_QUEUE_CREATED           = 11,
    NA_EV_QUEUE_EXECUTE           = 12,
    NA_EV_QUEUE_SIGNAL            = 13,
    NA_EV_QUEUE_WAIT              = 14,
    NA_EV_FENCE_CREATED           = 15,
    NA_EV_SWAPCHAIN_CREATED       = 16,
    NA_EV_SWAPCHAIN_RESIZED       = 17,
    NA_EV_PRESENT                 = 18,
    NA_EV_LOSS_MARK               = 19,
    NA_EV_EPOCH_BEGIN             = 20,
    NA_EV_INVENTORY_COMPLETE      = 21, /* validity boundary (R4) */
    NA_EV_ORDER_AMBIGUITY_RANGE   = 22, /* retroactive revocation (R3) */
    NA_EV_DEVICE_CREATED          = 23,
    /* ---- ABI 0.2 (C8 / H2), append-only; 0.1 consumers skip them ---- */
    NA_EV_ROOT_SIGNATURE_CREATED  = 24, /* NA_GROUP_PIPELINE, foundational */
    NA_EV_PIPELINE_CREATED        = 25, /* NA_GROUP_PIPELINE, foundational */
    NA_EV_LIST_OP                 = 26, /* PASS / RESOURCE_USE / CPU_CONSTANTS */
    NA_EV_SNAPSHOT                = 27  /* NA_GROUP_SNAPSHOT (C9 / H3) */
} na_event_kind;

/* ---- Event header flags ---- */
#define NA_EVF_ORDER_AMBIGUOUS   0x1u  /* see R3 rule at na_ev_queue_execute */
#define NA_EVF_METHOD_UNOBSERVED 0x2u  /* produced via partially observed path */
#define NA_EVF_TRUNCATED         0x4u  /* variable payload cut by byte budget */
#define NA_EVF_FOUNDATIONAL      0x8u  /* inventory-bearing; unknown+foundational
                                          => consumer inventory incomplete */
#define NA_EVF_PREEXISTING       0x10u /* object first seen already alive (e.g.
                                          hooked late); creation params carry
                                          NA_UNKNOWN_* markers, never invented */

/* Unknown-value markers (R1): fields the host could not observe. */
#define NA_UNKNOWN_U32 0xFFFFFFFFu
#define NA_UNKNOWN_U64 0xFFFFFFFFFFFFFFFFull

typedef struct na_event_header {
    uint32_t size_bytes;      /* total event size incl. header, multiple of 8 */
    uint32_t kind;            /* na_event_kind */
    na_epoch epoch;
    uint64_t thread_seq;      /* per-producer-thread monotonic */
    uint32_t producer_thread; /* OS thread id */
    uint32_t flags;           /* NA_EVF_* */
} na_event_header;

/* ---- Session / capability ---- */
typedef struct na_cap_entry {
    uint32_t group;           /* na_cap_group */
    uint32_t support;         /* na_cap_support */
    uint32_t detail_flags;    /* group-specific, 0 = none; unknown bits ignore */
    uint32_t _pad;
} na_cap_entry;
typedef struct na_ev_capability_report {
    na_event_header hdr;
    na_session_id session;
    uint32_t host_version_major, host_version_minor;
    uint64_t adapter_luid;
    uint32_t entry_count;
    uint32_t entries_offset;  /* from event start; stride = entry_stride */
    uint32_t entry_stride;    /* >= sizeof(na_cap_entry) */
    uint32_t _pad;
} na_ev_capability_report;

typedef struct na_ev_device_created {
    na_event_header hdr;
    na_object_id device;
    uint64_t adapter_luid;
    uint32_t hooked_interface_version; /* highest ID3D12Device<N> with hooked
                                          methods; higher versions in use =>
                                          NA_EVF_METHOD_UNOBSERVED events */
    uint32_t _pad;
} na_ev_device_created;

/* ---- Object lifecycle ---- */
typedef enum na_object_kind {
    NA_OBJ_DEVICE = 1, NA_OBJ_RESOURCE = 2, NA_OBJ_RESOURCE_HEAP = 3,
    NA_OBJ_DESCRIPTOR_HEAP = 4, NA_OBJ_LIST = 5, NA_OBJ_QUEUE = 6,
    NA_OBJ_FENCE = 7, NA_OBJ_SWAPCHAIN = 8, NA_OBJ_ALLOCATOR = 9
} na_object_kind;

/* Identity retirement (final release sentinel). NOT GPU completion. */
typedef struct na_ev_object_retired {
    na_event_header hdr;
    na_object_id object;
    uint32_t object_kind;     /* na_object_kind */
    uint32_t _pad;
} na_ev_object_retired;

typedef enum na_creation_kind {
    NA_CREATE_COMMITTED = 1, NA_CREATE_PLACED = 2, NA_CREATE_RESERVED = 3,
    NA_CREATE_OPENED_SHARED = 4, NA_CREATE_SWAPCHAIN_BUFFER = 5,
    NA_CREATE_UNKNOWN = NA_UNKNOWN_U32
} na_creation_kind;

typedef struct na_ev_resource_created {
    na_event_header hdr;
    na_object_id resource;
    na_object_id device;
    na_object_id heap;        /* resource heap id; 0 = committed/none */
    uint64_t heap_offset;     /* placed only; else NA_UNKNOWN_U64 */
    uint32_t dimension;       /* D3D12_RESOURCE_DIMENSION or NA_UNKNOWN_U32 */
    uint32_t format;          /* DXGI_FORMAT or NA_UNKNOWN_U32 */
    uint64_t width;
    uint32_t height;
    uint16_t depth_or_array;
    uint16_t mip_levels;
    uint32_t sample_count;
    uint32_t creation_kind;   /* na_creation_kind */
    uint64_t resource_flags;  /* D3D12_RESOURCE_FLAGS */
    uint32_t initial_state;   /* D3D12_RESOURCE_STATES or NA_UNKNOWN_U32 */
    uint32_t heap_type;       /* D3D12_HEAP_TYPE or NA_UNKNOWN_U32 */
} na_ev_resource_created;

typedef struct na_ev_resource_heap_created {
    na_event_header hdr;
    na_object_id heap;
    na_object_id device;
    uint64_t size_in_bytes;
    uint32_t heap_type;       /* D3D12_HEAP_TYPE */
    uint32_t heap_flags;      /* D3D12_HEAP_FLAGS */
} na_ev_resource_heap_created;

typedef struct na_ev_descriptor_heap_created {
    na_event_header hdr;
    na_object_id heap;
    na_object_id device;
    uint32_t heap_kind;       /* D3D12_DESCRIPTOR_HEAP_TYPE */
    uint32_t descriptor_count;
    uint32_t shader_visible;  /* 0/1 */
    uint32_t _pad;
} na_ev_descriptor_heap_created;

/* Descriptor events are RECORD-TIME CPU-side observations. version increments
 * per (heap, slot) on each observed write/copy-into; use-time shader-visible
 * contents are NOT established by these events (H1 limit). */
typedef enum na_descriptor_kind {
    NA_DESC_SRV = 1, NA_DESC_UAV = 2, NA_DESC_CBV = 3, NA_DESC_RTV = 4,
    NA_DESC_DSV = 5, NA_DESC_SAMPLER = 6, NA_DESC_UNKNOWN = NA_UNKNOWN_U32
} na_descriptor_kind;
typedef struct na_ev_descriptor_write {
    na_event_header hdr;
    na_object_id heap;
    uint32_t slot;
    uint32_t descriptor_kind;  /* na_descriptor_kind */
    uint64_t slot_version;     /* post-write version of (heap, slot) */
    na_object_id resource;     /* 0 = null view / not resource-backed */
    uint32_t format;           /* view format or NA_UNKNOWN_U32 */
    uint32_t first_subresource;/* first mip/array slice or NA_UNKNOWN_U32 */
    uint32_t subresource_count;/* or NA_UNKNOWN_U32 */
    uint32_t _pad;
} na_ev_descriptor_write;
typedef struct na_ev_descriptor_copy {
    na_event_header hdr;
    na_object_id src_heap;
    na_object_id dst_heap;
    uint32_t src_slot;
    uint32_t dst_slot;
    uint32_t slot_count;       /* contiguous range copied */
    uint32_t _pad;             /* dst slot versions each increment by 1; dst
                                  contents = src RECORD-TIME contents only if
                                  src versions unchanged since observation,
                                  else unknown */
} na_ev_descriptor_copy;

/* ---- Command lists ---- */
/* Initial recording state: CreateCommandList creates the list OPEN with
 * recording_generation 1; CreateCommandList1 creates it CLOSED with
 * recording_generation 0 (first successful Reset makes generation 1).
 * `initially_open` records which; creation via an unobserved path yields an
 * OBJECT event with NA_EVF_PREEXISTING and generation/openness UNKNOWN â€”
 * never invented. */
typedef struct na_ev_list_created {
    na_event_header hdr;
    na_object_id list;
    na_object_id device;
    na_object_id initial_allocator; /* 0 if unobserved/none (CreateCommandList1) */
    uint32_t list_type;        /* D3D12_COMMAND_LIST_TYPE */
    uint32_t is_bundle;        /* 0/1 */
    uint32_t initially_open;   /* 0/1, or NA_UNKNOWN_U32 */
    uint32_t _pad;
} na_ev_list_created;

#define NA_LISTF_RESET_FAILED 0x1u
#define NA_LISTF_IS_BUNDLE    0x2u
/* recording_generation increments ONLY on successful Reset (hresult >= 0);
 * failed Reset repeats the previous generation with NA_LISTF_RESET_FAILED and
 * does NOT establish a usable recording (R5 principle applied to lists). */
typedef struct na_ev_list_reset_close {
    na_event_header hdr;       /* kind = LIST_RESET or LIST_CLOSE */
    na_object_id list;
    na_object_id allocator;    /* reset only; 0 for close */
    uint64_t recording_generation;
    int32_t  hresult;
    uint32_t list_flags;       /* NA_LISTF_* */
} na_ev_list_reset_close;

/* ---- Queues ---- */
typedef struct na_ev_queue_created {
    na_event_header hdr;
    na_object_id queue;
    na_object_id device;
    uint32_t queue_type;       /* D3D12_COMMAND_LIST_TYPE */
    uint32_t queue_flags;      /* D3D12_COMMAND_QUEUE_FLAGS */
} na_ev_queue_created;

typedef struct na_list_ref {   /* layout FROZEN for MAJOR 0 (R2); evolution
                                  happens via ref_stride, never in-place */
    na_object_id list;
    uint64_t recording_generation;
} na_list_ref;

/* Ordering rule (R3): queue_ticket is an occurrence identity per queue. An
 * occurrence is AMBIGUOUS if its hook window [enter, exit] overlaps the hook
 * window of ANY other occurrence on the same queue â€” determined at EXIT time,
 * covering the reviewer's A-enter/B-enter/B-exit/A-exit case: overlap marks
 * BOTH. If an affected event was already delivered without the flag (delivery
 * raced finalization), the host MUST later emit NA_EV_ORDER_AMBIGUITY_RANGE
 * covering its ticket; consumers must treat every execute/signal/wait ticket
 * inside any received range as ambiguous regardless of per-event flags.
 * Ticket order therefore proves same-queue call order only for occurrences
 * that are non-overlapping AND outside every ambiguity range. */
typedef struct na_ev_queue_execute {
    na_event_header hdr;
    na_object_id queue;
    uint64_t queue_ticket;
    uint32_t list_count;
    uint32_t contains_bundles; /* 1 => derivations carry incomplete-bundles */
    uint32_t refs_offset;      /* from event start (R2) */
    uint32_t ref_stride;       /* >= sizeof(na_list_ref) */
} na_ev_queue_execute;

typedef struct na_ev_order_ambiguity_range {
    na_event_header hdr;
    na_object_id queue;
    uint64_t first_ticket;     /* inclusive */
    uint64_t last_ticket;      /* inclusive */
} na_ev_order_ambiguity_range;

/* ---- Fences / sync (R5) ---- */
typedef struct na_ev_fence_created {
    na_event_header hdr;
    na_object_id fence;
    na_object_id device;
    uint64_t initial_value;
    uint32_t fence_flags;      /* D3D12_FENCE_FLAGS; SHARED => external
                                  signalers possible, correlation weakens */
    uint32_t _pad;
} na_ev_fence_created;

typedef enum na_fence_provenance {
    /* SIGNAL events only: identifies the signal's own occurrence. Its hresult
     * proves successful SUBMISSION of the signal, never completion. */
    NA_FENCE_PROV_OBSERVED_QUEUE_SIGNAL = 1,
    /* Reserved: H1 does not hook CPU ID3D12Fence::Signal; this value is NOT
     * emitted in ABI 0.1 (no invented CPU observations). */
    NA_FENCE_PROV_OBSERVED_CPU_SIGNAL   = 2,
    /* WAIT events: the H1 default and, absent a documented closed-world
     * guarantee, the ONLY wait provenance (see rule below). */
    NA_FENCE_PROV_UNKNOWN               = 3,
    /* Conflicting evidence exists (repeated values, rewind, shared fence,
     * multiple writers): correlation refused, not guessed. */
    NA_FENCE_PROV_AMBIGUOUS             = 4
} na_fence_provenance;

/* H1 WAIT-CORRELATION RULE (B-002-REV2-REVIEW): fence waits are satisfied by
 * a THRESHOLD (completed value >= waited value), and CPU ID3D12Fence::Signal
 * is outside H1 observation coverage â€” an unobserved CPU/external signal, or
 * an initial fence value already at/above the threshold, can satisfy a wait
 * even when exactly one queue signal for that (fence, value) was observed.
 * Therefore H1 wait events carry provenance UNKNOWN (or AMBIGUOUS when
 * conflicting evidence exists) with source_queue == 0 and source_ticket == 0,
 * UNLESS a documented closed-world signal-source guarantee applies (e.g. a
 * host-OWNED fence whose every signaler is the host itself; no game fence
 * qualifies in H1). Consumers must derive NO cross-queue ordering edge from
 * game fence waits in H1. A failed operation (hresult < 0) establishes no
 * dependency of any kind. Ambiguity-range events (NA_EV_ORDER_AMBIGUITY_RANGE)
 * are FOUNDATIONAL: they carry NA_EVF_FOUNDATIONAL, their loss invalidates
 * inventory-derived ordering, and consumers must revalidate retained
 * deductions whenever a range arrives. */
typedef struct na_ev_queue_sync {
    na_event_header hdr;       /* kind = QUEUE_SIGNAL or QUEUE_WAIT */
    na_object_id queue;
    na_object_id fence;
    uint64_t value;
    uint64_t queue_ticket;
    int32_t  hresult;          /* of the Signal/Wait submission itself */
    uint32_t provenance;       /* waits: UNKNOWN/AMBIGUOUS per rule above;
                                  signals: PROV_OBSERVED_QUEUE_SIGNAL (self) */
    na_object_id source_queue; /* 0 in H1 for all game-fence waits (rule
                                  above); nonzero only under a documented
                                  closed-world guarantee */
    uint64_t source_ticket;    /* ditto; else 0 */
} na_ev_queue_sync;

/* ---- Swapchain / present ---- */
typedef struct na_ev_swapchain_created {
    na_event_header hdr;
    na_object_id swapchain;
    na_object_id device;
    na_object_id queue;        /* creating queue; 0 if unobserved */
    uint32_t buffer_count;
    uint32_t format;           /* DXGI_FORMAT */
    uint32_t width, height;
    uint32_t color_space;      /* DXGI_COLOR_SPACE_TYPE or NA_UNKNOWN_U32 */
    uint32_t buffer_generation;/* increments on every successful resize */
} na_ev_swapchain_created;
/* Buffer association: a resource id is tied to (swapchain, buffer index,
 * generation) ONLY when the host observes GetBuffer for it; such resources
 * are emitted as NA_EV_RESOURCE_CREATED with creation_kind
 * NA_CREATE_SWAPCHAIN_BUFFER. Absent that observation the association is
 * UNAVAILABLE â€” never inferred from matching dimensions/format. Later
 * color-space changes are likewise unavailable in H1 unless observed. */
typedef struct na_ev_swapchain_resized {
    na_event_header hdr;
    na_object_id swapchain;
    int32_t  hresult;          /* failed resize => old generation persists */
    uint32_t buffer_count;
    uint32_t format;
    uint32_t width, height;
    uint32_t buffer_generation;/* new generation IF hresult >= 0 */
    uint64_t _pad;
} na_ev_swapchain_resized;
/* Present is a correlation boundary, not frame-resource ownership proof. */
typedef struct na_ev_present {
    na_event_header hdr;
    na_object_id swapchain;
    uint64_t present_ordinal;  /* per-swapchain attempt counter, incl. failed */
    int32_t  hresult;
    uint32_t sync_interval;
    uint32_t present_flags;    /* DXGI_PRESENT_* */
    uint32_t backbuffer_index; /* at call time, or NA_UNKNOWN_U32 */
} na_ev_present;

/* ---- Loss / epoch (R4) ---- */
typedef struct na_ev_loss_mark {
    na_event_header hdr;       /* stream position marker only; authoritative
                                  counters live in na_observe_status */
    uint32_t group;            /* na_cap_group where loss occurred */
    uint32_t foundational;     /* 0/1 */
} na_ev_loss_mark;

typedef struct na_ev_epoch_begin {
    na_event_header hdr;       /* first event of an epoch */
    na_session_id session;
    na_epoch epoch;
} na_ev_epoch_begin;

/* Epoch validity boundary (R4): an epoch's inventory is usable ONLY after the
 * consumer receives NA_EV_INVENTORY_COMPLETE for it with complete == 1.
 * Between EPOCH_BEGIN and INVENTORY_COMPLETE the host replays a consistent
 * cut of live objects (creation-order snapshot; retirements of snapshot
 * objects are delivered AFTER the snapshot events, never interleaved before
 * their creation event). Foundational loss during replay forces complete == 0:
 * the epoch stays invalid and na_observe_resync() may be called again.
 * inventory_invalid in status clears only when the host has delivered
 * complete == 1 for the current epoch. Prior EXECUTION HISTORY (past
 * submissions, past presents, past descriptor versions) is NOT reconstructed
 * by any resync â€” snapshots restore only current-liveness inventory; history
 * before EPOCH_BEGIN is permanently unknown to the new epoch. */
typedef struct na_ev_inventory_complete {
    na_event_header hdr;
    na_epoch epoch;
    uint32_t complete;         /* 1 = baseline coherent; 0 = failed, retry */
    uint32_t objects_in_snapshot;
} na_ev_inventory_complete;

/* =====================================================================
 * ABI 0.2 (C8 / H2) — append-only additions. Same R1/R2 rules: decode via
 * size_bytes, arrays only through offset/stride, unknown = NA_UNKNOWN_*.
 * ===================================================================== */

/* Root signature: one entry per root parameter. */
typedef enum na_root_param_type {
    NA_ROOT_TABLE = 0, NA_ROOT_CONSTANTS = 1, NA_ROOT_CBV = 2, NA_ROOT_SRV = 3, NA_ROOT_UAV = 4
} na_root_param_type;
typedef struct na_root_param {
    uint32_t param_type;       /* na_root_param_type */
    uint32_t visibility;       /* D3D12_SHADER_VISIBILITY */
    uint32_t shader_register;  /* constants/root views; tables: first range */
    uint32_t register_space;
    uint32_t count;            /* constants: 32-bit values; tables: range count */
    uint32_t range_type;       /* tables: first range D3D12_DESCRIPTOR_RANGE_TYPE, else NA_UNKNOWN_U32 */
    uint32_t range_descriptors;/* tables: first range descriptor count (UINT_MAX = unbounded) */
    uint32_t _pad;
    /* 0.2 amendment (C8 review): every range of a table, see na_root_range. */
    uint32_t first_range;      /* tables: index of its first range in the event's range array */
    uint32_t _pad2;
} na_root_param;
typedef struct na_root_range {
    uint32_t range_type;       /* D3D12_DESCRIPTOR_RANGE_TYPE */
    uint32_t num_descriptors;  /* UINT_MAX = unbounded */
    uint32_t base_register;
    uint32_t register_space;
    uint32_t table_offset;     /* descriptors from the table start, APPEND resolved;
                                  NA_UNKNOWN_U32 after an unbounded range */
    uint32_t flags;            /* D3D12_DESCRIPTOR_RANGE_FLAGS (1.1), else 0 */
} na_root_range;
typedef struct na_ev_root_signature_created {
    na_event_header hdr;
    na_object_id root_signature;
    na_object_id device;
    uint64_t blob_hash;        /* FNV-1a 64 of the serialized blob */
    uint64_t blob_bytes;
    uint32_t flags;            /* D3D12_ROOT_SIGNATURE_FLAGS, or NA_UNKNOWN_U32 */
    uint32_t param_count;      /* 0 + NA_EVF_METHOD_UNOBSERVED when not decodable */
    uint32_t params_offset;
    uint32_t param_stride;     /* >= sizeof(na_root_param) */
    uint32_t range_count;      /* all tables' ranges, in parameter order */
    uint32_t ranges_offset;
    uint32_t range_stride;     /* >= sizeof(na_root_range) */
    uint32_t _pad;
} na_ev_root_signature_created;

typedef enum na_shader_stage {
    NA_STAGE_VS = 0, NA_STAGE_PS = 1, NA_STAGE_DS = 2, NA_STAGE_HS = 3, NA_STAGE_GS = 4,
    NA_STAGE_CS = 5, NA_STAGE_AS = 6, NA_STAGE_MS = 7, NA_STAGE_COUNT = 8
} na_shader_stage;
typedef enum na_pipeline_kind {
    NA_PIPELINE_GRAPHICS = 1, NA_PIPELINE_COMPUTE = 2, NA_PIPELINE_STREAM = 3
} na_pipeline_kind;
typedef struct na_ev_pipeline_created {
    na_event_header hdr;
    na_object_id pipeline;
    na_object_id device;
    na_object_id root_signature;   /* 0 = none given (embedded) / unobserved */
    uint32_t pipeline_kind;        /* na_pipeline_kind */
    uint32_t rt_count;
    uint64_t stage_hash[NA_STAGE_COUNT];  /* FNV-1a 64 of bytecode; 0 = stage absent */
    uint32_t stage_bytes[NA_STAGE_COUNT];
    uint32_t rtv_formats[8];
    uint32_t dsv_format;
    uint32_t sample_count;
    uint32_t depth_enable;         /* 0/1 or NA_UNKNOWN_U32 */
    uint32_t depth_write;          /* 0/1 (D3D12_DEPTH_WRITE_MASK_ALL) or NA_UNKNOWN_U32 */
    uint32_t depth_func;           /* D3D12_COMPARISON_FUNC or NA_UNKNOWN_U32 */
    uint32_t blend_enable_mask;    /* bit i = RT i blends; NA_UNKNOWN_U32 if unobserved */
} na_ev_pipeline_created;

/* One recorded command-list operation. op_index starts at 1 for every
 * recording (recording_generation as in LIST_RESET). a[] meaning per op:
 *   SET_PIPELINE            a0 pipeline
 *   SET_ROOT_SIGNATURE      a0 root signature, a1 0=graphics 1=compute
 *   SET_DESCRIPTOR_HEAPS    a0 CBV/SRV/UAV heap, a1 sampler heap
 *   SET_ROOT_TABLE          a0 heap, a1 slot, a2 slot_version, a3 param, a4 0=gfx 1=compute
 *   SET_ROOT_CONSTANTS      a0 param, a1 dest offset (32-bit units), a2 count, a3 0=gfx 1=compute;
 *                           payload = count * 4 bytes BY VALUE
 *   SET_ROOT_VIEW           a0 resource (or UNKNOWN), a1 byte offset (or UNKNOWN), a2 param,
 *                           a3 view kind (NA_DESC_CBV/SRV/UAV), a4 0=gfx 1=compute
 *   OM_SET_RENDER_TARGETS   a0 rt count, a1 has dsv; payload = (rt count [+1 dsv last]) na_view_ref
 *   CLEAR_RTV               payload = 1 na_view_ref, a0..a1 = clear colour (4 floats, raw bits)
 *   CLEAR_DSV               payload = 1 na_view_ref, a0 flags, a1 depth (float bits), a2 stencil
 *   DRAW                    a0 vertex count, a1 instance count
 *   DRAW_INDEXED            a0 index count, a1 instance count
 *   DISPATCH                a0 x, a1 y, a2 z
 *   EXECUTE_INDIRECT        a0 max commands; effects unobserved (flag set)
 *   EXECUTE_BUNDLE          a0 bundle list id; contents unobserved (flag set)
 *   COPY_RESOURCE           a0 dst, a1 src
 *   COPY_BUFFER_REGION      a0 dst, a1 dst offset, a2 src, a3 src offset, a4 bytes
 *   COPY_TEXTURE_REGION     a0 dst, a1 dst subresource (NA_UNKNOWN_U32 = footprint), a2 src, a3 src subresource;
 *                           payload = 1 na_copy_region (destination point, source box, footprint offsets)
 *   RESOLVE                 a0 dst, a1 dst sub, a2 src, a3 src sub, a4 format
 *   RESOURCE_BARRIER        a0 barrier count; payload = na_barrier array
 *   RS_VIEWPORT             a0 count; a1 = x|y (float bits lo/hi), a2 = w|h, a3 = min|max depth (first);
 *                           payload = every viewport as 6 floats (x, y, w, h, min, max)
 *   RS_SCISSOR              a0 count; a1 = left|top, a2 = right|bottom (int32 lo/hi) (first);
 *                           payload = every rectangle as 4 int32 (left, top, right, bottom)
 * Ops recorded while the list's current implementation is not fully hooked,
 * or before its Reset was observed, carry NA_EVF_METHOD_UNOBSERVED. */
typedef enum na_list_op_code {
    NA_OP_SET_PIPELINE = 1, NA_OP_SET_ROOT_SIGNATURE = 2, NA_OP_SET_DESCRIPTOR_HEAPS = 3,
    NA_OP_SET_ROOT_TABLE = 4, NA_OP_SET_ROOT_CONSTANTS = 5, NA_OP_SET_ROOT_VIEW = 6,
    NA_OP_OM_SET_RENDER_TARGETS = 7, NA_OP_CLEAR_RTV = 8, NA_OP_CLEAR_DSV = 9,
    NA_OP_DRAW = 10, NA_OP_DRAW_INDEXED = 11, NA_OP_DISPATCH = 12, NA_OP_EXECUTE_INDIRECT = 13,
    NA_OP_EXECUTE_BUNDLE = 14, NA_OP_COPY_RESOURCE = 15, NA_OP_COPY_BUFFER_REGION = 16,
    NA_OP_COPY_TEXTURE_REGION = 17, NA_OP_RESOLVE = 18, NA_OP_RESOURCE_BARRIER = 19,
    NA_OP_RS_VIEWPORT = 20, NA_OP_RS_SCISSOR = 21
} na_list_op_code;
typedef struct na_view_ref {       /* a descriptor as used, resolved at record time */
    na_object_id heap;             /* 0 = handle outside every observed heap */
    uint32_t slot;
    uint32_t descriptor_kind;      /* na_descriptor_kind of the slot's last write */
    uint64_t slot_version;         /* version at record time */
    na_object_id resource;         /* from the slot's last observed write; UNKNOWN if none */
    uint32_t format;
    uint32_t first_subresource;
} na_view_ref;
typedef struct na_copy_region {
    uint32_t dst_x, dst_y, dst_z;
    uint32_t has_box;              /* 0 = whole source */
    uint32_t box[6];               /* left, top, front, right, bottom, back */
    uint64_t dst_footprint_offset; /* footprint copies (subresource UNKNOWN) */
    uint64_t src_footprint_offset;
} na_copy_region;
typedef struct na_barrier {
    uint32_t barrier_type;         /* D3D12_RESOURCE_BARRIER_TYPE */
    uint32_t barrier_flags;        /* D3D12_RESOURCE_BARRIER_FLAGS */
    na_object_id resource;         /* transition / UAV resource; aliasing: before */
    na_object_id resource_after;   /* aliasing only, else 0 */
    uint32_t subresource;
    uint32_t state_before;         /* transitions; else NA_UNKNOWN_U32 */
    uint32_t state_after;
    uint32_t _pad;
} na_barrier;
typedef struct na_ev_list_op {
    na_event_header hdr;
    na_object_id list;
    uint64_t recording_generation; /* NA_UNKNOWN_U64 if the Reset was not observed */
    uint64_t op_index;
    uint32_t op;                   /* na_list_op_code */
    uint32_t payload_offset;       /* from event start; 0 = none */
    uint32_t payload_bytes;
    uint32_t payload_stride;       /* element size for arrays, else 0 */
    uint64_t a[6];
} na_ev_list_op;

/* C9 / H3: bounded CPU copy of constant data a submitted recording binds,
 * taken at the submission boundary (ExecuteCommandLists entry) from the
 * application's own CPU mapping of an UPLOAD-heap buffer. The application may
 * still be writing that memory: every snapshot is APP_SYNC_UNKNOWN; it is
 * never an atomic or execution-time capture. GPU-written memory is not
 * captured (status GPU_WRITTEN). The snapshot is taken at the occurrence's
 * ENTRY, before its ordering is final: queue_ticket names the occurrence, and
 * its NA_EV_QUEUE_EXECUTE (plus any ORDER_AMBIGUITY_RANGE) is authoritative
 * for ordering; a snapshot never proves clean ordering on its own. Every
 * binding is reported separately (no de-duplication across bindings). */
typedef enum na_snapshot_binding {
    NA_SNAP_ROOT_CBV = 1,      /* SET_ROOT_VIEW (CBV) */
    NA_SNAP_TABLE_CBV = 2      /* CBV descriptor reached through SET_ROOT_TABLE */
} na_snapshot_binding;
typedef enum na_snapshot_status {
    NA_SNAP_CAPTURED = 0,      /* bytes follow (possibly truncated, see flags) */
    NA_SNAP_NOT_MAPPED = 1,    /* no observed live CPU mapping of the buffer */
    NA_SNAP_GPU_WRITTEN = 2,   /* not an upload heap: CPU view is not GPU-time data */
    NA_SNAP_UNRESOLVED = 3,    /* binding did not resolve to an observed buffer */
    NA_SNAP_BUDGET = 4         /* per-submission byte/count budget exhausted */
} na_snapshot_status;
#define NA_SNAP_SYNC_APP_UNKNOWN 1u
typedef struct na_ev_snapshot {
    na_event_header hdr;
    uint64_t snapshot_id;          /* per session, monotonic */
    na_object_id queue;
    uint64_t queue_ticket;         /* the ExecuteCommandLists occurrence */
    na_object_id list;
    uint64_t recording_generation;
    uint64_t op_index;             /* the LIST_OP that bound it */
    na_object_id resource;
    uint64_t offset;               /* byte offset in the resource */
    uint32_t binding;              /* na_snapshot_binding */
    uint32_t param;                /* root parameter index */
    uint32_t compute;              /* 0 graphics / 1 compute */
    uint32_t sync;                 /* NA_SNAP_SYNC_* (always APP_UNKNOWN) */
    uint32_t requested_bytes;      /* CBV size, or the host default for root CBVs */
    uint32_t captured_bytes;
    uint32_t payload_offset;       /* from event start; 0 when nothing captured */
    uint32_t status;               /* na_snapshot_status */
} na_ev_snapshot;

/* ---- Batch framing (R2) ----
 * A batch is one contiguous host-owned allocation:
 *   [na_event_batch header][payload: packed events]
 * header_bytes  = size of the header region (>= sizeof(na_event_batch) for
 *                 the host's minor; payload starts here).
 * payload_bytes = exact byte length of the packed events after the header.
 * Total readable bytes = header_bytes + payload_bytes. event_count events
 * live in the payload; decoding walks size_bytes until exactly payload_bytes
 * are consumed â€” anything else is malformed (treat as loss, discard batch).
 * Memory is valid only until the sink returns; consumers copy what they keep.
 * Batches arrive serially on one host worker thread. */
#define NA_BATCHF_FINAL 0x1u   /* last batch before na_observe_stop returns */
typedef struct na_event_batch {
    uint32_t header_bytes;
    uint16_t abi_major;        /* consumer must verify == compiled major */
    uint16_t abi_minor;        /* fields appended beyond the consumer's
                                  compiled structs are safely ignorable */
    na_session_id session;
    na_epoch epoch;
    uint32_t payload_bytes;
    uint32_t event_count;
    uint32_t batch_flags;      /* NA_BATCHF_* */
    uint32_t _pad;
} na_event_batch;

typedef void(__cdecl* na_event_sink)(const na_event_batch* batch, void* user);

/* ---- Status (R2/R4) ----
 * Caller sets size_bytes to its buffer capacity before the call; the host
 * fills min(capacity, host struct size) bytes, never more, and rewrites
 * size_bytes to the byte count actually written (old callers keep working;
 * capacity < 16 or capacity == 0 is invalid and the call fails). The returned
 * snapshot is COHERENT: session/epoch/inventory_invalid/counters are captured
 * together (host uses a seqlock internally). Callable from any thread,
 * INCLUDING inside the sink. */
typedef struct na_observe_status {
    uint32_t size_bytes;
    uint16_t abi_major;
    uint16_t abi_minor;
    na_session_id session;
    na_epoch epoch;
    uint32_t inventory_invalid; /* 0/1; clears only per R4 rule above */
    uint32_t _pad;
    uint64_t foundational_dropped;
    uint64_t detail_dropped;
    uint64_t batches_delivered;
    uint64_t per_group_dropped[16]; /* indexed by na_cap_group; >= COUNT_V1
                                       slots are reserved and 0 */
} na_observe_status;

typedef struct na_observe_config {
    uint32_t size_bytes;       /* caller sets to sizeof its compiled struct;
                                  unknown larger sizes rejected by older hosts,
                                  reserved fields must be 0 */
    uint32_t metadata_budget_mib;      /* 0 = default 32 */
    uint32_t detail_event_budget;      /* 0 = default 100000 (H2) */
    uint32_t constant_bytes_budget;    /* 0 = default 4 MiB (H2) */
    uint32_t presentation_boundaries;  /* 0 = default 8 (H2) */
    uint32_t _reserved[8];             /* must be 0 */
} na_observe_config;

/* Error codes (negative), 0 = success. */
#define NA_OK              0
#define NA_E_ABI          -1  /* major mismatch / bad struct sizes */
#define NA_E_STATE        -2  /* wrong lifecycle state / reentrancy */
#define NA_E_ARG          -3
#define NA_E_HOOK         -4  /* hooking backend failure */

/* Lifecycle. start(): registers the sink, generates a fresh random
 * session_id, begins epoch 1 with EPOCH_BEGIN + snapshot + INVENTORY_COMPLETE
 * (possibly complete == 0). A consumer holding records from an earlier start
 * must discard them on seeing a different session_id â€” epochs never resume
 * across sessions (R4). stop(): halts producers, delivers a final batch
 * flagged NA_BATCHF_FINAL, guarantees no sink call after return. stop() and
 * resync() MUST NOT be called from inside the sink (NA_E_STATE): both may
 * wait for the sink to return. query_status IS sink-safe. resync(): bumps
 * epoch and replays snapshot per R4; fails with NA_E_STATE when stopped. */
int  __cdecl na_observe_abi(uint16_t* out_major, uint16_t* out_minor);
int  __cdecl na_observe_start(const na_observe_config* cfg, na_event_sink sink,
                              void* user);
int  __cdecl na_observe_stop(void);
int  __cdecl na_observe_query_status(na_observe_status* inout);
int  __cdecl na_observe_resync(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NA_HOST_OBSERVE_H */
