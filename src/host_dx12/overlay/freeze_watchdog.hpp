// A69: freeze watchdog. Session 12 (2026-09-26) froze about five seconds
// after Frame Generation started generating in The Last of Us Part I: the
// game's presents stopped for good, the GPU went idle, the process stayed
// alive, and nothing in the logs said which thread was stuck where. This
// module makes the NEXT freeze self-describing:
//
//  - Capture(): walks every thread of the process (each one suspended only
//    for its own walk) with RtlLookupFunctionEntry/RtlVirtualUnwind (no
//    dbghelp, no loader lock while a thread is suspended) and returns raw
//    return addresses; module names are resolved afterwards, with all
//    threads running again.
//  - WriteMiniDump(): a small minidump (thread info, no full memory) via
//    dbghelp, best effort.
//  - The status-log thread calls Check() every write: when the game's
//    presents have not advanced for `stall_ms` while they advanced before,
//    it records the stacks into the status log, writes the dump next to the
//    user settings, and switches Frame Generation off in the saved settings
//    so the next launch runs without it (the menu says so).
#pragma once

#include <cstdint>
struct _EXCEPTION_POINTERS;

namespace na::freeze {

// TLOU's render thread was #103 of 123 in the Home-freeze dump; the old
// 96-thread text log omitted it and pointed investigation at an idle thread.
constexpr uint32_t kMaxThreads = 512;
constexpr uint32_t kMaxFrames = 24;

struct ThreadStack {
    uint32_t thread_id;
    uint32_t frame_count;
    uint32_t suspended;   // 1 = the thread was suspended for the walk (0 = could not be, e.g. the caller)
    uint64_t frames[kMaxFrames]; // raw return addresses, innermost first
};

struct Capture {
    uint32_t thread_count;
    uint32_t threads_total; // threads found (may exceed thread_count when kMaxThreads was reached)
    ThreadStack threads[kMaxThreads];
};

// Walks all threads except the calling one. Returns the number captured.
uint32_t CaptureAllThreads(Capture* out);

// Formats one stack as "tid: module+0xRVA > module+0xRVA > ..." (unresolved
// frames as ?0xADDR). Resolution happens here, with nothing suspended.
// Returns the number of characters written (without the terminator).
uint32_t FormatStack(const ThreadStack& t, char* out, uint32_t capacity);

// Writes a minidump of this process to `path` (MiniDumpWithThreadInfo plus
// indirectly referenced memory and unloaded modules; no full memory).
// Returns false when dbghelp is unavailable or the write failed.
bool WriteMiniDump(const wchar_t* path, _EXCEPTION_POINTERS* exception = nullptr);

// The watchdog itself. `game_presents` is the game's Present counter; call at
// a steady cadence (the status log's 5 s). Returns true on the first call
// that detects the stall; subsequent calls return false until presents
// advance again. `now_ms` is GetTickCount64().
class Watchdog {
public:
    explicit Watchdog(uint64_t stall_ms) : stall_ms_(stall_ms) {}
    bool Check(uint64_t game_presents, uint64_t now_ms);
    bool Tripped() const { return tripped_; }
    uint64_t StalledMs(uint64_t now_ms) const { return last_change_ms_ ? now_ms - last_change_ms_ : 0; }

private:
    uint64_t stall_ms_;
    uint64_t last_presents_ = 0, last_change_ms_ = 0;
    bool seen_progress_ = false, tripped_ = false;
};

} // namespace na::freeze
