// Process-exit regression: applications may exit without stopping discovery.
#include <Windows.h>
#include "host_dx12/host_observe.h"
static void __cdecl Sink(const na_event_batch*,void*) {}
int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    if(na_observe_start(nullptr,Sink,nullptr)!=NA_OK) return 1;
    Sleep(20);
    return 0; // Deliberately leave the transport running through CRT teardown.
}
