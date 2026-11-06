#pragma once
#include <cstdint>

namespace na::scale::native {
struct Stats {
    bool available=false,controlled=false,confirmed=false;
    uint32_t mode=0,thread=0,render_w=0,render_h=0,out_w=0,out_h=0;
    uint64_t ticks=0,applies=0,requests=0,evaluation=0;
    float fraction=1;
    char reason[192]{};
};
// Setup only: locate named native settings and transactionally hook their own
// update/apply calls. Never executes a native rebuild on the setup/UI thread.
bool Start(int mode,const wchar_t* log_path);
bool Available();
void Request(int mode);
// Observation/intent only, at a real Present. Native calls consume this intent
// at their normal settings tick. Two dimensions AND a newer full-output DLSS
// evaluation are required for confirmation; writing the setting is not success.
void Observe(bool enabled,uint32_t rw,uint32_t rh,uint32_t ow,uint32_t oh,uint64_t evaluation,bool evaluated);
Stats GetStats();
#ifdef NA_NATIVE_SCALE_TEST
namespace locator { struct Result; }
void ConfigureForTest(uint8_t* image,const locator::Result&,void(*tick)(),void(*apply)(void*));
void TickForTest();
void ApplyForTest(void*);
#endif
} // namespace na::scale::native
