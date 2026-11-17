#pragma once
// Which game and process produced a capture folder (Astra/Cort 2026-11-03: Anno captures kept
// landing in Sniper results because every game shares LocalAppData\NeuralAlias\captures).
// Each folder gets <kind>-YYYYMMDD-HHMMSS-<exe>-p<pid> as its name and a session.json with the
// exe, its full path, the pid and the process start time; results scripts select by those.
#include <cstddef>
#include <cstdint>

namespace na::capture {
struct Session {
    wchar_t exe[64] = {};        // file name without extension, e.g. "Anno1800"
    wchar_t path[260] = {};      // full path of the game executable
    uint32_t pid = 0;
    uint64_t started_utc = 0;    // process creation time, FILETIME (100 ns since 1601, UTC)
    char started_iso[32] = {};   // the same, "YYYY-MM-DDTHH:MM:SSZ"
};
const Session& ThisProcess();
// LocalAppData\NeuralAlias\captures\<kind>-<local time>-<exe>-p<pid> (directories created). False when
// LocalAppData is unavailable.
bool MakeDir(const wchar_t* kind, wchar_t* out, size_t out_chars);
// Writes <dir>\session.json once (kind, exe, path, pid, process start). Safe to call repeatedly.
void TagDir(const wchar_t* dir, const char* kind);
}
