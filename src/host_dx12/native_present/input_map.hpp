// S2: one coordinate transform, shared by the EXISTING overlay input hooks.
#pragma once
#include "host_dx12/native_present/size_spoof.hpp"

namespace na::native_present {
POINT ToGameClient(const InputGeometry& g, POINT p);
POINT ToNativeClient(const InputGeometry& g, POINT p);
POINT ToGameScreen(const InputGeometry& g, POINT p);
POINT ToNativeScreen(const InputGeometry& g, POINT p);
RECT ToNativeClip(const InputGeometry& g, RECT virtual_screen_rect);

// The caller argument is the game's original return address, passed from the
// existing detour, not the helper's caller. NativeScope overrides it.
bool MapCursorRead(POINT* p, const void* caller);
bool MapCursorWrite(int& x, int& y, const void* caller);
bool MapClipRect(const RECT* requested, RECT& physical, const void* caller);
bool MapClipRead(RECT* physical, const void* caller);
// Call ONLY after the overlay has recorded/swallowed its real input. Do not
// scale ScreenToClient/ClientToScreen again: both use the same real origin,
// with coherent VIRTUAL screen coordinates on the game-facing side.
bool MapGameMouseMessage(HWND hwnd, UINT msg, LPARAM& lp);
// Desktop rect is the real primary or virtual desktop selected by the RAWMOUSE
// flags. Relative raw deltas are deliberately returned unchanged.
bool MapAbsoluteRaw(RAWMOUSE& mouse, const InputGeometry& g, RECT real_desktop);
bool MapRawMouseRead(RAWMOUSE& mouse, const void* caller);

struct InputStats { uint64_t cursor_read = 0, cursor_write = 0, clipped = 0, messages = 0, absolute_raw = 0; };
InputStats GetInputStats();
void FormatInputStatus(char* out, uint32_t capacity);
} // namespace na::native_present
