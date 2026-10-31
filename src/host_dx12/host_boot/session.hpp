#pragma once
#include <cstdint>
namespace na::host {
void StartSession(); // init worker: defer effects/persistence and install clean-exit hook
void RendererObserved(); // real non-TEST Present/evaluate; atomic + SetEvent only
using RendererReadyFn=void(*)();
void RunSessionWorker(RendererReadyFn ready=nullptr); // tail of existing init worker
bool SessionReady();
bool CleanExitObserved();
void ActivateSavedRequests(const wchar_t* ini);
void StopSessionForTests(uint32_t exit_code=0);
}
