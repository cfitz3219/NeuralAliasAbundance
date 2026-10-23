// Host-owned stub for the game-adapter contract (game_adapter.h, A54).
// Linked when the build selects no adapter library (CMake NA_GAME_ADAPTER_LIB
// unset). Reports "no adapter" so the host runs with Route A services only.
// Reconstructed 2026-10-23 (C1): the original stub was not in the export.
#include "host_dx12/host_boot/game_adapter.h"

extern "C" {
int na_adapter_prepare(void) { return 0; }
int na_adapter_start(void) { return 0; }
void na_adapter_stop(void) {}
const char* na_adapter_name(void) { return "no adapter"; }
}
