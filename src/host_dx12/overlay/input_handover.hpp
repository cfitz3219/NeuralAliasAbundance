#pragma once
#include <windows.h>
#include <Xinput.h>
#include <cstdlib>

namespace na::overlay::input {
// An input held when Home opens belongs to the game. Require its release
// before accepting a new menu press, not just one synthesized up frame.
class ReleaseBeforeUse {
public:
    bool Update(bool down, bool opening) {
        if (opening) blocked_=down;
        if (!down) blocked_=false;
        return down && !blocked_;
    }
private:
    bool blocked_=false;
};
using PollPad = DWORD(WINAPI*)(DWORD,XINPUT_STATE*);
inline XINPUT_GAMEPAD FreshPad(PollPad poll, bool* connected=nullptr) {
    XINPUT_STATE state{};
    const bool ok=poll && poll(0,&state)==ERROR_SUCCESS;
    if (connected) *connected=ok;
    if (!ok) return {};
    return state.Gamepad;
}
inline bool Navigating(const XINPUT_GAMEPAD& pad) {
    return pad.wButtons!=0 || std::abs(static_cast<int>(pad.sThumbLX))>XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE ||
        std::abs(static_cast<int>(pad.sThumbLY))>XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
}
} // namespace na::overlay::input
