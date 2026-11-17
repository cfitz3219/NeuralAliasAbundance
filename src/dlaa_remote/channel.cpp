#include "channel.hpp"
#include <cstring>
#include <string>

namespace na::dlaa_remote {

bool Channel::Attach(const wchar_t* name, bool create)
{
    Close();
    if (!name || !*name) return false;
    const std::wstring base(name);
    const auto made = [create](HANDLE h) { return h && (!create || GetLastError() != ERROR_ALREADY_EXISTS); };
    if (create) {
        map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Block), (base + L"-map").c_str());
        if (!made(map_)) return Close(), false;
        request_ = CreateEventW(nullptr, FALSE, FALSE, (base + L"-req").c_str());
        if (!made(request_)) return Close(), false;
        reply_ = CreateEventW(nullptr, FALSE, FALSE, (base + L"-rep").c_str());
        if (!made(reply_)) return Close(), false;
    } else {
        map_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, (base + L"-map").c_str());
        request_ = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (base + L"-req").c_str());
        reply_ = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (base + L"-rep").c_str());
        if (!map_ || !request_ || !reply_) return Close(), false;
    }
    block_ = static_cast<Block*>(MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Block)));
    if (!block_) return Close(), false;
    if (create) {
        std::memset(block_, 0, sizeof(Block));
        block_->magic = kMagic;
        block_->version = kVersion;
    } else if (block_->magic != kMagic || block_->version != kVersion) {
        return Close(), false; // the two halves are from different builds
    }
    return true;
}

bool Channel::Create(const wchar_t* name) { return Attach(name, true); }
bool Channel::Open(const wchar_t* name) { return Attach(name, false); }

void Channel::Close()
{
    if (block_) UnmapViewOfFile(block_);
    for (HANDLE* h : {&map_, &request_, &reply_})
        if (*h) CloseHandle(*h), *h = nullptr;
    block_ = nullptr;
}

namespace {
Channel::Result Wait(HANDLE event, HANDLE peer, DWORD timeout_ms)
{
    HANDLE both[2] = {event, peer};
    const DWORD r = WaitForMultipleObjects(peer ? 2 : 1, both, FALSE, timeout_ms);
    if (r == WAIT_OBJECT_0) return Channel::Result::Ok;
    if (r == WAIT_OBJECT_0 + 1) return Channel::Result::PeerGone;
    return r == WAIT_TIMEOUT ? Channel::Result::Timeout : Channel::Result::Failed;
}
}

Channel::Result Channel::Call(const Request& request, Reply* reply, HANDLE peer, DWORD timeout_ms)
{
    if (!block_) return Result::Failed;
    block_->request = request;
    if (!SetEvent(request_)) return Result::Failed;
    const Result r = Wait(reply_, peer, timeout_ms);
    if (r == Result::Ok && reply) *reply = block_->reply;
    return r;
}

Channel::Result Channel::Receive(Request* request, HANDLE peer, DWORD timeout_ms)
{
    if (!block_) return Result::Failed;
    const Result r = Wait(request_, peer, timeout_ms);
    if (r == Result::Ok && request) *request = block_->request;
    return r;
}

void Channel::Answer(const Reply& reply)
{
    if (!block_) return;
    block_->reply = reply;
    SetEvent(reply_);
}

} // namespace na::dlaa_remote
