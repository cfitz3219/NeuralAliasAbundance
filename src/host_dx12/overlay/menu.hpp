// NeuralAlias menu framework: the in-game menu is four pages (Picture,
// Enhancements, Preferences, Details & support) whose content comes from
// feature sections registered by the module that owns them. Both routes use
// this; a route registers its sections at startup and the overlay host draws
// the chrome (title, navigation, footer) and the sections of the current page.
//
// A30 (additive, proposal U-001): `Section::page` says where a section lives.
// It defaults to Auto, which maps by title so existing registrations keep
// working unchanged: "DLSS"/"Picture" -> Picture; "Frame Generation",
// "Neural Rendering", "Ray Reconstruction" -> Enhancements; anything else ->
// Details. Aggregate initialisation with four fields is still valid.
#pragma once

namespace na::menu {

enum class Availability {
    Available,    // fully working; draw() renders its controls
    ComingSoon,   // designed in, not implemented: shown as a row tagged "Planned"
    Unavailable,  // detected as not usable here: shown as a row tagged "Unavailable"
};

enum class Page {
    Auto = 0,     // decide from the title (see header comment)
    Picture,
    Enhancements,
    Preferences,
    Details,
};
constexpr int kPageCount = 4; // Picture..Details
const char* PageName(Page p); // "Picture", "Enhancements", "Preferences", "Details & support"

struct Section {
    const char* title;      // e.g. "Neural Rendering" â€” the row label
    Availability avail;
    const char* note;       // one short sentence shown under the label when not available
    void (*draw)();         // controls; may be null for placeholders
    Page page = Page::Auto; // A30, additive
};

// Up to 16 sections; later registrations are ignored (returns false).
bool Register(const Section& s);
void Clear();
int Count();
const Section* At(int index);
Page ResolvePage(const Section& s); // Auto -> concrete page

// Current page (navigation state; host and tests may set it).
Page CurrentPage();
void SetCurrentPage(Page p);
bool PageHasContent(Page p); // any registered section resolves to p

// Draws the sections of one page. On Picture, available sections draw
// freely (they own the page's layout). On the other pages every section is
// a row inside one group box: available sections call draw() to render
// their row(s); placeholders get label + note + "Planned"/"Unavailable".
// Controls are disabled while `enabled` is false.
void DrawPage(Page p, bool enabled);

// Legacy shared renderer (A21): draws every registered section in order
// with a section header. Kept for Route B's CPU fixture; the host no longer
// calls it.
void DrawSections(bool enabled);

} // namespace na::menu
