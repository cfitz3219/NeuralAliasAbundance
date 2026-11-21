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
// LocalAppData is unavailable or the drive has less than the floor left (the reason is logged).
bool MakeDir(const wchar_t* kind, wchar_t* out, size_t out_chars);
// Old captures are removed by build count (core/capture_retention.hpp; Cort 2026-11-17, after 819 GB
// of captures filled his drive). Every capture folder's session.json names the build that made it
// ([host] build_commit). MakeDir starts a background pass: when captures of `builds_trigger` builds
// are present, only the newest `builds_keep` builds keep theirs. Folders made by this process or in
// the last two hours, and their builds, are never removed. MakeDir also refuses a new capture when the
// drive is nearly full. Read from `ini` at every MakeDir, so a change needs no restart:
//   [capture] builds_trigger (default 10, 0 = keep everything), builds_keep (default 5),
//             min_free_gb (default 20, 0 = no guard).
// `log` receives one line per removal, per refusal and per pass, each with sizes and the reason.
using RetentionLog = void (*)(const char* line);
void SetRetention(const wchar_t* ini, RetentionLog log);
struct RetentionStats {
    uint64_t passes = 0, removed_folders = 0, removed_bytes = 0, remove_errors = 0, refused_disk_full = 0;
    uint64_t last_total_bytes = 0, last_free_bytes = 0;
};
RetentionStats GetRetentionStats();
// Writes <dir>\session.json once (kind, exe, path, pid, process start). Safe to call repeatedly.
void TagDir(const wchar_t* dir, const char* kind);
}
