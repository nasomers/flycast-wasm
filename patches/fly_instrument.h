// fly_instrument.h — unified instrumentation ring buffer for Flycast WASM JIT
//
// Single-header library. One TU must define FLY_INSTRUMENT_IMPL before #include
// to pull in the implementation (pointer definitions + fly_init + exports).
//
// Build modes:
//   FLY_INSTRUMENT_RELEASE   — all emits compile to nothing. Zero overhead.
//   FLY_INSTRUMENT_PROFILE   — every event recorded, no sampling.
//   (default)                — FLY_INSTRUMENT_DEV: ring buffer live, sampling TBD.
//
// Wire format v1: 1 MiB ring, 64 B header + 32 768 × 32 B events. Single-producer,
// single-consumer (JS drain), lock-free via monotonic head/tail counters.
//
// Correctness-first rule: the report generator refuses to present perf numbers
// when UNHANDLED_OP / BLOCK_TRAP / AUDIO_UNDERRUN / excessive CACHE_RESET /
// long DUPE_FRAME streaks are present. The JIT's job here is just to emit
// faithfully. Interpretation happens offline.

#pragma once

#include "types.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Wire format constants — DO NOT change without bumping FLY_RING_VERSION
// ---------------------------------------------------------------------------

#define FLY_RING_MAGIC     0x464C5952u   // 'FLYR'
#define FLY_RING_VERSION   1u
#define FLY_RING_CAPACITY  32768u        // events; must be power of two
#define FLY_EVENT_SIZE     32u           // bytes

// Event type enum (wire-stable for v1).
// Adding a type: append at the end. Renumbering: bump FLY_RING_VERSION.
enum FlyEventType {
    FLY_EVT_FRAME_BEGIN      = 0x01,
    FLY_EVT_FRAME_END        = 0x02,
    FLY_EVT_FRAME_STATS      = 0x03,  // per-frame dispatch stats (emitted with FRAME_END)

    FLY_EVT_TIMESLICE_BEGIN  = 0x10,
    FLY_EVT_TIMESLICE_END    = 0x11,

    FLY_EVT_BLOCK_COMPILE    = 0x20,
    FLY_EVT_BLOCK_EVICT      = 0x21,
    FLY_EVT_BLOCK_TRAP       = 0x22,

    FLY_EVT_UNHANDLED_OP     = 0x30,
    FLY_EVT_FALLBACK_CALL    = 0x31,
    FLY_EVT_CACHE_RESET      = 0x32,

    FLY_EVT_PRESENT          = 0x40,
    FLY_EVT_DUPE_FRAME       = 0x41,

    FLY_EVT_AUDIO_CALLBACK   = 0x50,
    FLY_EVT_AUDIO_UNDERRUN   = 0x51,

    FLY_EVT_INTERRUPT        = 0x60,
    FLY_EVT_EXCEPTION        = 0x61,

    FLY_EVT_MARK             = 0x70,

    // --- v1 additions (append-only, no version bump needed) ---
    // Emitted every retro_run() call with per-frame progress stats.
    // a = blocks_this_frame, b = cycle_delta, c = fallback_calls,
    // d = render_called_this_frame.
    FLY_EVT_MAINLOOP_TICK    = 0x80,

    // Emitted ONCE per session when the watchdog detects 2s+ of no
    // cycle_counter advance. a = ms_since_last_advance, b = ctx.pc,
    // c = ctx.sr raw, d = interrupt_pend.
    FLY_EVT_FREEZE_DETECTED  = 0x81,

    // Emitted on each block dispatch (PC trace buffer). a = block_pc,
    // b = block_exit_pc, c = block_vaddr, d = guest_cycles.
    FLY_EVT_BLOCK_DISPATCH   = 0x82,

    // Emitted when an exception escapes Emulator::run()'s top-level catch.
    // a = exception_kind (0=SH4Thrown, 1=std::exception, 2=unknown),
    // b = epc or 0, c = expEvn or 0, d = 0 reserved.
    FLY_EVT_ESCAPE_EXCEPTION = 0x83
};

// 64-byte header (natural 8-byte alignment for u64 fields).
typedef struct FlyRingHeader {
    u32 magic;
    u32 version;
    u32 event_size;
    u32 capacity_evts;
    u64 head;            // producer cursor (C writes)
    u64 tail;            // consumer cursor (JS writes)
    u64 drops;           // events dropped due to overflow
    u64 epoch_ms_hi_lo;  // reserved for epoch wall-clock (set by JS if desired)
    u8  reserved[16];
} FlyRingHeader;

// 32-byte fixed-size event. Four u32 payload slots + type + flags + ts.
typedef struct FlyEvent {
    u64 ts_ns;   // nanoseconds since fly_init()
    u32 type;    // FlyEventType
    u32 a;
    u32 b;
    u32 c;
    u32 d;
    u32 flags;   // reserved; currently zero
} FlyEvent;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

extern FlyRingHeader* g_fly_ring;
extern double         g_fly_epoch_ms;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void fly_init(void);
u32  fly_get_ring_ptr(void);
u32  fly_get_ring_bytes(void);

// ---------------------------------------------------------------------------
// Hot-path emit (inline)
// ---------------------------------------------------------------------------

static inline u64 fly_now_ns(void) {
#ifdef __EMSCRIPTEN__
    double delta_ms = emscripten_get_now() - g_fly_epoch_ms;
    if (delta_ms < 0.0) delta_ms = 0.0;
    return (u64)(delta_ms * 1000000.0);
#else
    return 0;
#endif
}

static inline void fly_emit(u32 type, u32 a, u32 b, u32 c, u32 d) {
#if defined(FLY_INSTRUMENT_RELEASE)
    (void)type; (void)a; (void)b; (void)c; (void)d;
#else
    FlyRingHeader* r = g_fly_ring;
    if (!r) return;
    u64 head = r->head;
    u64 tail = r->tail;
    // Overwrite-oldest policy: always write, count drops when the ring is
    // full. The consumer (JS drain) is responsible for advancing `tail`;
    // if it can't keep up, oldest events get lapped and `drops` increments.
    // Single-writer invariant preserved — only this producer touches `head`
    // and `drops`.
    if ((head - tail) >= (u64)r->capacity_evts) {
        r->drops++;
    }
    u32 slot = (u32)(head & (u64)(r->capacity_evts - 1));
    FlyEvent* e = (FlyEvent*)((u8*)r + sizeof(FlyRingHeader)) + slot;
    e->ts_ns = fly_now_ns();
    e->type  = type;
    e->a     = a;
    e->b     = b;
    e->c     = c;
    e->d     = d;
    e->flags = 0;
    r->head  = head + 1;
#endif
}

#if defined(FLY_INSTRUMENT_RELEASE)
    #define FLY_EVT(type, a, b, c, d) ((void)0)
#else
    #define FLY_EVT(type, a, b, c, d) \
        fly_emit((u32)(type), (u32)(a), (u32)(b), (u32)(c), (u32)(d))
#endif

// ---------------------------------------------------------------------------
// Implementation (one TU defines FLY_INSTRUMENT_IMPL)
// ---------------------------------------------------------------------------

#ifdef FLY_INSTRUMENT_IMPL

#include <stdlib.h>
#include <string.h>

FlyRingHeader* g_fly_ring     = (FlyRingHeader*)0;
double         g_fly_epoch_ms = 0.0;

#ifdef __EMSCRIPTEN__
    #define FLY_EXPORT EMSCRIPTEN_KEEPALIVE
#else
    #define FLY_EXPORT
#endif

void fly_init(void) {
    if (g_fly_ring) return;
    size_t total = sizeof(FlyRingHeader)
                 + (size_t)FLY_RING_CAPACITY * (size_t)FLY_EVENT_SIZE;
    void* mem = malloc(total);
    if (!mem) return;
    memset(mem, 0, total);
    FlyRingHeader* r = (FlyRingHeader*)mem;
    r->magic         = FLY_RING_MAGIC;
    r->version       = FLY_RING_VERSION;
    r->event_size    = FLY_EVENT_SIZE;
    r->capacity_evts = FLY_RING_CAPACITY;
    r->head          = 0;
    r->tail          = 0;
    r->drops         = 0;
#ifdef __EMSCRIPTEN__
    g_fly_epoch_ms = emscripten_get_now();
#endif
    g_fly_ring = r;
}

FLY_EXPORT u32 fly_get_ring_ptr(void) {
    return (u32)(uintptr_t)g_fly_ring;
}

FLY_EXPORT u32 fly_get_ring_bytes(void) {
    return (u32)(sizeof(FlyRingHeader)
               + (size_t)FLY_RING_CAPACITY * (size_t)FLY_EVENT_SIZE);
}

#endif  // FLY_INSTRUMENT_IMPL

#ifdef __cplusplus
}  // extern "C"
#endif
