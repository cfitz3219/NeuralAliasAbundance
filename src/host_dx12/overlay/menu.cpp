#include "host_dx12/overlay/menu.hpp"

#include <cstring>

#include <imgui.h>

#include "host_dx12/overlay/na_style.hpp"

namespace na::menu {
namespace {
constexpr int kMax = 16;
Section g_sections[kMax];
int g_count = 0;
Page g_current = Page::Picture;

bool title_is(const Section& s, const char* t) { return s.title && _stricmp(s.title, t) == 0; }
} // namespace

const char* PageName(Page p)
{
    switch (p) {
    case Page::Picture: return "Picture";
    case Page::Enhancements: return "Enhancements";
    case Page::Preferences: return "Preferences";
    case Page::Details: return "Details & support";
    default: return "";
    }
}

bool Register(const Section& s)
{
    if (g_count >= kMax || !s.title) return false;
    g_sections[g_count++] = s;
    return true;
}

void Clear() { g_count = 0; g_current = Page::Picture; }
int Count() { return g_count; }
const Section* At(int index)
{
    return (index >= 0 && index < g_count) ? &g_sections[index] : nullptr;
}

Page ResolvePage(const Section& s)
{
    if (s.page != Page::Auto) return s.page;
    if (title_is(s, "DLSS") || title_is(s, "Picture")) return Page::Picture;
    if (title_is(s, "Frame Generation") || title_is(s, "Neural Rendering") ||
        title_is(s, "Ray Reconstruction"))
        return Page::Enhancements;
    return Page::Details;
}

Page CurrentPage() { return g_current; }
void SetCurrentPage(Page p)
{
    if (p == Page::Auto) p = Page::Picture;
    g_current = p;
}

bool PageHasContent(Page p)
{
    for (int i = 0; i < g_count; ++i)
        if (ResolvePage(g_sections[i]) == p) return true;
    return false;
}

void DrawPage(Page p, bool enabled)
{
    if (p == Page::Picture) {
        for (int i = 0; i < g_count; ++i) {
            const Section& s = g_sections[i];
            if (ResolvePage(s) != p) continue;
            if (s.avail == Availability::Available && s.draw) {
                s.draw();
            } else {
                na::ui::BeginGroupBox(s.title);
                na::ui::BeginRow(s.title, s.note, na::ui::BadgeWidth("Unavailable"));
                na::ui::Badge(s.avail == Availability::ComingSoon ? "Planned" : "Unavailable");
                na::ui::EndRow();
                na::ui::EndGroupBox();
            }
        }
        return;
    }
    bool any = false;
    for (int i = 0; i < g_count; ++i) {
        const Section& s = g_sections[i];
        if (ResolvePage(s) != p) continue;
        if (!any) {
            na::ui::BeginGroupBox(PageName(p));
            any = true;
        } else {
            na::ui::RowDivider();
        }
        if (s.avail == Availability::Available && s.draw) {
            ImGui::BeginDisabled(!enabled);
            s.draw();
            ImGui::EndDisabled();
        } else {
            const char* tag = s.avail == Availability::ComingSoon ? "Planned" : "Unavailable";
            na::ui::BeginRow(s.title, s.note, na::ui::BadgeWidth(tag));
            na::ui::Badge(tag);
            na::ui::EndRow();
        }
    }
    if (any) na::ui::EndGroupBox();
}

void DrawSections(bool enabled)
{
    for (int i = 0; i < g_count; ++i) {
        const Section& s = g_sections[i];
        bool avail = s.avail == Availability::Available;
        na::ui::SectionHeader(s.title, avail, s.note);
        if (avail && s.draw) {
            ImGui::BeginDisabled(!enabled);
            s.draw();
            ImGui::EndDisabled();
        }
    }
}

} // namespace na::menu
