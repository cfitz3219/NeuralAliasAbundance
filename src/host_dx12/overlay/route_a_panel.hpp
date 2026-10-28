#pragma once

namespace na::overlay {
// Registers Route A's menu sections on their pages (call once before
// Start()). Route A has no PanelFn any more: the host draws the pages.
void RegisterRouteAMenu();
void RegisterLiveDlaaMenu(); // J2 section (Picture page), when J2 is enabled
}
