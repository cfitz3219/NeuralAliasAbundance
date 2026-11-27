# Initial cache for a shipping lighting recorder. The host stub cannot supply scene inputs.
# Use cmake -C cmake/lighting-recorder.cmake -S . -B <build>.
set(NA_GAME_ADAPTER_LIB na_discovery_diagnostics CACHE STRING "Recorder scene-discovery provider" FORCE)
