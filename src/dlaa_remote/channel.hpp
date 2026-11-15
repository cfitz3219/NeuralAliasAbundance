// The message path between the in-game half and the companion: one shared block and two events,
// found by a name both sides are given. No pictures travel here, only the small requests in
// protocol.hpp.
#pragma once
#include "protocol.hpp"
#include <windows.h>

namespace na::dlaa_remote {

class Channel final {
public:
    Channel() = default;
    ~Channel() { Close(); }
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    // The in-game half creates the objects; `name` must be new (fails if it already exists).
    bool Create(const wchar_t* name);
    // The companion opens what the in-game half created.
    bool Open(const wchar_t* name);
    void Close();
    bool Valid() const { return block_ != nullptr; }
    Block* Shared() { return block_; }

    enum class Result { Ok, Timeout, PeerGone, Failed };
    // In-game half: send one request and wait for its reply, or for `peer` (the companion process)
    // to end, or for the time limit. After anything but Ok the channel must not be used again:
    // a late reply would be mistaken for the next request's.
    Result Call(const Request& request, Reply* reply, HANDLE peer, DWORD timeout_ms);
    // Companion: wait for the next request, or for `peer` (the game process) to end.
    Result Receive(Request* request, HANDLE peer, DWORD timeout_ms);
    void Answer(const Reply& reply);

private:
    bool Attach(const wchar_t* name, bool create);
    HANDLE map_ = nullptr, request_ = nullptr, reply_ = nullptr;
    Block* block_ = nullptr;
};

} // namespace na::dlaa_remote
