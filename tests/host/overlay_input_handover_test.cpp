#include "test_support.hpp"
#include "host_dx12/overlay/input_handover.hpp"
namespace {
DWORD result=ERROR_SUCCESS;
XINPUT_GAMEPAD physical{};
DWORD WINAPI Poll(DWORD index,XINPUT_STATE* out) {
    CHECK(index==0);
    out->Gamepad=physical; // even dirty failure output must not leak into UI
    return result;
}
}
int main() {
    using namespace na::overlay::input;
    ReleaseBeforeUse mouse;
    CHECK(!mouse.Update(true,true));
    for (int frame=0;frame<8;++frame) CHECK(!mouse.Update(true,false));
    CHECK(!mouse.Update(false,false));
    CHECK(mouse.Update(true,false)); // genuine new click after release
    CHECK(!mouse.Update(false,true)); // opening with no button held
    CHECK(mouse.Update(true,false));
    ReleaseBeforeUse controller;
    physical.wButtons=XINPUT_GAMEPAD_A;
    CHECK(!controller.Update(Navigating(FreshPad(Poll)),true));
    CHECK(!controller.Update(Navigating(FreshPad(Poll)),false));
    physical={}; CHECK(!controller.Update(Navigating(FreshPad(Poll)),false));
    physical.wButtons=XINPUT_GAMEPAD_A;
    CHECK(controller.Update(Navigating(FreshPad(Poll)),false));
    result=ERROR_DEVICE_NOT_CONNECTED;
    CHECK(!Navigating(FreshPad(Poll))); // no old A press after disconnect
    CHECK(!controller.Update(Navigating(FreshPad(Poll)),false));
    CHECK(!Navigating(FreshPad(nullptr)));
    result=ERROR_SUCCESS; physical={}; physical.sThumbLY=32767;
    CHECK(Navigating(FreshPad(Poll)));
    result=ERROR_DEVICE_NOT_CONNECTED; CHECK(FreshPad(Poll).sThumbLY==0);
    return test::finish("overlay_input_handover_test");
}
