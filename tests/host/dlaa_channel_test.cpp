// The message path between the in-game half and the DLAA companion, without any graphics:
// a request reaches the other side and its reply comes back; a silent or vanished peer is
// noticed; halves from different builds refuse each other.
#include "dlaa_remote/channel.hpp"
#include <cstdio>
#include <cstring>
#include <thread>

using namespace na::dlaa_remote;
#define REQUIRE(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main()
{
    wchar_t name[80];
    swprintf_s(name, L"Local\\NA-dlaa-channel-test-%lu", GetCurrentProcessId());
    Channel game;
    REQUIRE(!game.Valid());
    REQUIRE(game.Create(name));
    {
        Channel second;
        REQUIRE(!second.Create(name)); // a name is used once
    }
    Request frame{};
    frame.kind = Kind::Frame;
    frame.frame.slot = 2;
    frame.frame.value = 0x1122334455667788ull;
    frame.frame.jitter_x = 0.25f;
    Reply reply{};
    // Nobody is listening yet: the caller gets its time limit back, not a hang.
    REQUIRE(game.Call(frame, &reply, nullptr, 20) == Channel::Result::Timeout);

    Channel fresh; // a timed-out channel is not reused
    swprintf_s(name, L"Local\\NA-dlaa-channel-test-%lu-b", GetCurrentProcessId());
    REQUIRE(fresh.Create(name));
    int served = 0;
    bool intact = false;
    std::thread companion([&] {
        Channel c;
        if (!c.Open(name)) return;
        for (;;) {
            Request r{};
            if (c.Receive(&r, nullptr, 5000) != Channel::Result::Ok) return;
            Reply a{};
            if (r.kind == Kind::Frame) {
                intact = r.frame.slot == 2 && r.frame.value == 0x1122334455667788ull && r.frame.jitter_x == 0.25f;
                a.ok = 1;
                a.micros = 42;
                strcpy_s(a.reason, "done");
            }
            ++served;
            c.Answer(a);
            if (r.kind == Kind::Bye) return;
        }
    });
    REQUIRE(fresh.Call(frame, &reply, nullptr, 5000) == Channel::Result::Ok);
    REQUIRE(reply.ok == 1 && reply.micros == 42 && std::strcmp(reply.reason, "done") == 0);
    Request bye{};
    bye.kind = Kind::Bye;
    REQUIRE(fresh.Call(bye, &reply, nullptr, 5000) == Channel::Result::Ok);
    REQUIRE(reply.ok == 0);
    companion.join();
    REQUIRE(served == 2 && intact);

    // A peer that has ended is reported at once, well inside the time limit.
    HANDLE ended = CreateEventW(nullptr, TRUE, TRUE, nullptr); // stands in for an ended process: signalled
    REQUIRE(ended);
    const DWORD t0 = GetTickCount();
    REQUIRE(fresh.Call(frame, &reply, ended, 5000) == Channel::Result::PeerGone);
    REQUIRE(GetTickCount() - t0 < 2000);
    CloseHandle(ended);

    // Halves from different builds do not talk.
    Channel old;
    swprintf_s(name, L"Local\\NA-dlaa-channel-test-%lu-c", GetCurrentProcessId());
    REQUIRE(old.Create(name));
    old.Shared()->version = kVersion + 1;
    Channel other;
    REQUIRE(!other.Open(name));
    old.Shared()->version = kVersion;
    REQUIRE(other.Open(name));
    std::printf("dlaa_channel_test: all passed\n");
    return 0;
}
