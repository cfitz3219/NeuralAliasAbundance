// First-time set-up (devices, DLSS start, the passes' programs, the motion working set) runs on a thread
// of its own in the product, so a game never stops drawing while it happens (Cort 2026-11-27: a game
// froze when it ran on the game's thread). Until it is there, frames are left as the game drew them.
//
// Tests that count delivered frames from the very first call ask for the old order here: set-up done
// inside the first call. Nothing in the product sets it.
#pragma once
#include <atomic>

namespace na::first_setup {
inline std::atomic<bool>& InlineForTests()
{
    static std::atomic<bool> inline_setup{false};
    return inline_setup;
}
} // namespace na::first_setup
