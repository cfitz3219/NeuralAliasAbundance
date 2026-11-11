// Windows plumbing shared by both ends of the bridge: object names, handles,
// the shared-memory block and events. Rebuilt from the recovered Alien bridge
// (recovered/dx11-enhancement/src/core/bridge_transport.hpp and
// neural_transport.hpp). No graphics API here.
#pragma once

#include "bridge/frame_record.hpp"

#include <windows.h>

#include <cstdint>
#include <string>

namespace na::bridge {

// One owned kernel handle.
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    Handle& operator=(Handle&& o) noexcept
    {
        if (this != &o) { reset(); h_ = o.h_; o.h_ = nullptr; }
        return *this;
    }
    void reset(HANDLE h = nullptr)
    {
        if (h_) CloseHandle(h_);
        h_ = (h == INVALID_HANDLE_VALUE) ? nullptr : h;
    }
    HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    HANDLE h_ = nullptr;
};

// The name stem of one session's shared objects, e.g.
// "Local\\NABridge-1234-5678". Both processes derive every object name from it.
inline std::wstring SessionStem(DWORD pid, uint64_t tick)
{
    return L"Local\\NABridge-" + std::to_wstring(pid) + L"-" + std::to_wstring(tick);
}

enum class Object { Color, Depth, Motion, Output, InputFence, OutputFence, Block, RecordEvent, SubmittedEvent, StopEvent };

inline std::wstring ObjectName(const std::wstring& stem, Object o)
{
    switch (o) {
    case Object::Color: return stem + L".color";
    case Object::Depth: return stem + L".depth";
    case Object::Motion: return stem + L".motion";
    case Object::Output: return stem + L".output";
    case Object::InputFence: return stem + L".input-fence";
    case Object::OutputFence: return stem + L".output-fence";
    case Object::Block: return stem + L".block";
    case Object::RecordEvent: return stem + L".record";
    case Object::SubmittedEvent: return stem + L".submitted";
    case Object::StopEvent: return stem + L".stop";
    }
    return stem;
}

// The shared-memory block (FrameRecord + HelperStatus). The game side creates
// it; the helper opens it. Both map it read/write: the game writes `frame`,
// the helper writes `status`.
class SharedMemory {
public:
    SharedMemory() = default;
    ~SharedMemory() { close(); }
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    // Returns nullptr on success, else a reason.
    const char* create(const std::wstring& name)
    {
        close();
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedBlock), name.c_str());
        const DWORD err = GetLastError();
        if (!h) return "The bridge could not create its shared memory.";
        if (err == ERROR_ALREADY_EXISTS) { CloseHandle(h); return "The bridge's shared-memory name is already in use."; }
        handle_.reset(h);
        return map();
    }
    const char* open(const std::wstring& name)
    {
        close();
        handle_.reset(OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str()));
        if (!handle_) return "The helper could not open the bridge's shared memory.";
        return map();
    }
    void close()
    {
        if (block_) UnmapViewOfFile(block_);
        block_ = nullptr;
        handle_.reset();
    }
    SharedBlock* block() const { return block_; }

private:
    const char* map()
    {
        block_ = static_cast<SharedBlock*>(MapViewOfFile(handle_.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedBlock)));
        if (!block_) { handle_.reset(); return "The bridge could not map its shared memory."; }
        return nullptr;
    }
    Handle handle_;
    SharedBlock* block_ = nullptr;
};

inline Handle CreateNamedEvent(const std::wstring& name, bool manual_reset)
{
    return Handle(CreateEventW(nullptr, manual_reset ? TRUE : FALSE, FALSE, name.c_str()));
}
inline Handle OpenNamedEvent(const std::wstring& name)
{
    return Handle(OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name.c_str()));
}

// Copies a C string into a fixed char array, always terminated.
template <size_t N>
inline void CopyReason(char (&dst)[N], const char* src)
{
    size_t i = 0;
    for (; src && src[i] && i + 1 < N; ++i) dst[i] = src[i];
    dst[i] = 0;
}

} // namespace na::bridge
