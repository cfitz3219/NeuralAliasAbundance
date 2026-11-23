// NeuralAlias UI style â€” the single shared implementation of docs/UI-STYLE.md.
// Both routes call na::ui::ApplyStyle() / LoadFonts() and the widget helpers;
// do not fork per route. Header-only so Route B can include it without
// linking Route A code.
//
// Both routes share graphite surfaces, warm text, and one italic wordmark.
// Metrics use one scale factor (SetScale, default 1.25 since A42) plus an optional
// "Bold" text weight preference (A44; it replaced the earlier "Large" size).
// Colour always accompanies words: no state is conveyed by colour alone.
#pragma once

#include <imgui.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace na::ui {

inline ImVec4 Rgb(unsigned hex, float a = 1.0f)
{
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f,
                  (hex & 0xFF) / 255.0f, a);
}

// ---- Tokens (DESIGN.md "Tokens and layout") ----
inline const ImVec4 kBg       = Rgb(0x111716);        // window
inline const ImVec4 kBgNav    = Rgb(0x151C1A);        // navigation and title bar
inline const ImVec4 kBgRow    = Rgb(0x1E2524);        // rows / groups
inline const ImVec4 kBgRaised = Rgb(0x2E3733);        // buttons, inputs
inline const ImVec4 kBgHover  = Rgb(0x464942);
inline const ImVec4 kBgCard   = Rgb(0x30322E);        // mode cards
inline const ImVec4 kBgCardOn = Rgb(0x37493E);        // selected card
inline const ImVec4 kBgNavOn  = Rgb(0x323C35);        // current page
inline const ImVec4 kLine     = Rgb(0x28302E);
inline const ImVec4 kLineSoft = Rgb(0x62675F);
inline const ImVec4 kText     = Rgb(0xF2F2EA);
inline const ImVec4 kTextDim  = Rgb(0xB8C2BB);
inline const ImVec4 kAccent   = Rgb(0xB7CF97);        // selection
inline const ImVec4 kFocus    = Rgb(0xC7E4CE);        // keyboard/controller focus
inline const ImVec4 kSwitchOn = Rgb(0x729B82);
inline const ImVec4 kSwitchOff= Rgb(0x5E655F);
inline const ImVec4 kOk       = Rgb(0x9BDBB5);        // "good", with words
inline const ImVec4 kWarn     = Rgb(0xE2C685);        // "attention", with words
inline const ImVec4 kError    = Rgb(0xFF8A94);        // failures, with words
// Compatibility aliases (earlier tokens still referenced by route code).
inline const ImVec4 kBgPanel  = kBg;
inline const ImVec4 kSignal   = kAccent;
inline const ImVec4 kSignalDim= kSwitchOn;

constexpr float kBaseBodyPx = 16.5f * 1.60f, kBaseLabelPx = 15.0f * 1.60f, kBaseHelpPx = 12.0f * 1.60f;
constexpr float kBaseTitlePx = 29.0f * 1.60f, kBaseBrandPx = 20.0f * 2.25f;
constexpr float kBaseWindowWidth = 1160.0f, kBaseWindowHeight = 760.0f;
constexpr float kBaseMinWindowWidth = 560.0f, kBaseMinWindowHeight = 380.0f;
constexpr float kRadiusWindow = 22.0f, kRadiusGroup = 15.0f, kRadiusControl = 9.0f;

struct Config {
    float scale = 1.25f;  // A42: the user asked for half of the 2.5 default after session 6
    bool bold = false;    // A44: Preferences > Text weight: Bold (body text set in the heavier face)
    char font_body[260]{};
    char font_title[260]{};
    ImFont* display = nullptr;
    ImFont* wordmark_italic = nullptr;
    ImFont* body = nullptr;
    ImFont* title = nullptr;
};
inline Config& Cfg()
{
    static Config c;
    return c;
}
inline void SetScale(float s)
{
    if (s < 0.5f) s = 0.5f;
    if (s > 5.0f) s = 5.0f;
    Cfg().scale = s;
}
inline void SetBoldText(bool bold) { Cfg().bold = bold; }
inline void SetFontPaths(const char* body, const char* title)
{
    if (body && body[0]) strncpy_s(Cfg().font_body, body, _TRUNCATE);
    if (title && title[0]) strncpy_s(Cfg().font_title, title, _TRUNCATE);
}
// Viewport-relative scale: the user's scale (default 2.5) is defined for a
// 1440-line screen, and the menu keeps the same proportion of the screen on
// smaller displays (1080p -> 1.875, 720p -> 1.25). Documented in UI-STYLE.md;
// the ini scale= remains the user's knob.
inline float& DisplayHeightRef()
{
    static float h = 1440.0f;
    return h;
}
inline void SetDisplayHeight(float h) { if (h > 0) DisplayHeightRef() = h; }
inline float EffectiveScale()
{
    float f = DisplayHeightRef() / 1440.0f;
    if (f > 1.0f) f = 1.0f;
    return Cfg().scale * f;
}
inline float S(float base) { return base * EffectiveScale(); }
inline float BodyPx() { return S(kBaseBodyPx); }
inline float LabelPx() { return S(kBaseLabelPx); }
inline float HelpPx() { return S(kBaseHelpPx); }
inline float TitlePx() { return S(kBaseTitlePx); }
inline float BrandPx() { return S(kBaseBrandPx); }
inline float WindowWidth() { return S(kBaseWindowWidth); }
inline float WindowHeight() { return S(kBaseWindowHeight); }
inline float MinWindowWidth() { return S(kBaseMinWindowWidth); }
inline float MinWindowHeight() { return S(kBaseMinWindowHeight); }
// The face used for running text: the regular face, or the heavier title
// face when the user chose Bold (A44).
inline ImFont* TextFace() { return (Cfg().bold && Cfg().title) ? Cfg().title : Cfg().body; }

// Call once after ImGui::CreateContext(), before the backend init. ImGui
// 1.92 fonts are dynamically sized, so one face per weight is enough.
inline ImFont* BundledPoise(WORD resource, const char* name, float size)
{
    // The same shared style runs in either proxy. Get the module containing this code.
    static const int module_anchor = 0;
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&module_anchor), &module)) return nullptr;
    HRSRC entry = FindResourceW(module, MAKEINTRESOURCEW(resource), MAKEINTRESOURCEW(10));
    if (!entry) return nullptr;
    HGLOBAL handle = LoadResource(module, entry);
    void* bytes = handle ? LockResource(handle) : nullptr;
    const DWORD length = SizeofResource(module, entry);
    if (!bytes || !length) return nullptr;
    ImFontConfig font;
    font.FontDataOwnedByAtlas = false; // the module owns the immutable bytes
    std::snprintf(font.Name, sizeof font.Name, "%s", name);
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(bytes, static_cast<int>(length), size, &font);
}

inline void LoadFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    Config& c = Cfg();
    c.body = c.font_body[0] ? io.Fonts->AddFontFromFileTTF(c.font_body, BodyPx()) : nullptr;
    if (!c.body) c.body = BundledPoise(111, "Poise Regular", BodyPx());
    c.title = c.font_title[0] ? io.Fonts->AddFontFromFileTTF(c.font_title, TitlePx()) : nullptr;
    c.display = c.title ? c.title : BundledPoise(110, "Poise Light", TitlePx());
    if (!c.title) c.title = BundledPoise(112, "Poise Medium", LabelPx());
    if (!c.body) {
        ImFontConfig fc;
        fc.SizePixels = S(kBaseBodyPx);
        c.body = io.Fonts->AddFontDefault(&fc);
    }
    if (!c.title) c.title = c.body;
    if (!c.display) c.display = c.title;
    // Use the installed italic face for the signature only. A slanted Poise fallback
    // keeps the wordmark available on Windows installations without Georgia.
    char windows[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryA(windows, MAX_PATH);
    c.wordmark_italic = nullptr;
    if (length && length < MAX_PATH - 20) {
        strcat_s(windows, "\\Fonts\\georgiai.ttf");
        if (GetFileAttributesA(windows) != INVALID_FILE_ATTRIBUTES)
            c.wordmark_italic = io.Fonts->AddFontFromFileTTF(windows, BrandPx());
    }
    io.FontDefault = c.body;
}

inline void ApplyStyle()
{
    ImGuiStyle& s = ImGui::GetStyle();
    s.DisabledAlpha = 0.62f;
    s.WindowRounding = S(kRadiusWindow);
    s.ChildRounding = S(kRadiusGroup);
    s.FrameRounding = S(kRadiusControl);
    s.GrabRounding = S(kRadiusControl);
    s.PopupRounding = S(kRadiusControl);
    s.ScrollbarRounding = S(kRadiusControl);
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.WindowPadding = ImVec2(0, 0);            // chrome lays itself out
    s.FramePadding = ImVec2(S(12), S(8));
    s.ItemSpacing = ImVec2(S(12), S(9));
    s.ItemInnerSpacing = ImVec2(S(8), S(8));
    s.ScrollbarSize = S(10);
    s.GrabMinSize = S(16.0f);
    s.SeparatorTextBorderSize = 1.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = kBg;
    c[ImGuiCol_TextSelectedBg] = kBgHover;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = kBgRaised;
    c[ImGuiCol_Border] = kLine;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = kBgRaised;
    c[ImGuiCol_FrameBgHovered] = kBgHover;
    c[ImGuiCol_FrameBgActive] = kBgHover;
    c[ImGuiCol_TitleBg] = kBg;
    c[ImGuiCol_TitleBgActive] = kBg;
    c[ImGuiCol_TitleBgCollapsed] = kBg;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kTextDim;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kFocus;
    c[ImGuiCol_Button] = kBgRaised;
    c[ImGuiCol_ButtonHovered] = kBgHover;
    c[ImGuiCol_ButtonActive] = kBgNavOn;
    c[ImGuiCol_Header] = kBgRaised;
    c[ImGuiCol_HeaderHovered] = kBgHover;
    c[ImGuiCol_HeaderActive] = kBgNavOn;
    c[ImGuiCol_Separator] = kLine;
    c[ImGuiCol_SeparatorHovered] = kLineSoft;
    c[ImGuiCol_SeparatorActive] = kAccent;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = kLine;
    c[ImGuiCol_ScrollbarGrabHovered] = kLineSoft;
    c[ImGuiCol_ScrollbarGrabActive] = kAccent;
    c[ImGuiCol_ResizeGrip] = kLine;
    c[ImGuiCol_ResizeGripHovered] = kLineSoft;
    c[ImGuiCol_ResizeGripActive] = kFocus;
    c[ImGuiCol_NavCursor] = kFocus;
    c[ImGuiCol_TableBorderLight] = kLine;
    c[ImGuiCol_TableBorderStrong] = kLine;
}

// ---- Typography helpers ----
inline void PushBody() { ImGui::PushFont(TextFace(), BodyPx()); }
inline void PushLabel() { ImGui::PushFont(TextFace(), LabelPx()); }
inline void PushLabelStrong() { ImGui::PushFont(Cfg().title, LabelPx()); }
inline void PushHelp() { ImGui::PushFont(TextFace(), HelpPx()); }
inline void PushTitle() { ImGui::PushFont(Cfg().display, TitlePx()); }
inline void PopFont() { ImGui::PopFont(); }

// Small uppercase-ish line above a page heading ("IMAGE QUALITY").
inline void Eyebrow(const char* text)
{
    PushHelp();
    ImGui::TextColored(kTextDim, "%s", text);
    PopFont();
}
inline void Heading(const char* text)
{
    PushTitle();
    ImGui::TextColored(kText, "%s", text);
    PopFont();
}
inline void Lead(const char* text)
{
    PushLabel();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.72f);
    ImGui::TextColored(kTextDim, "%s", text);
    ImGui::PopTextWrapPos();
    PopFont();
}
inline void Hint(const char* text)
{
    PushHelp();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kTextDim, "%s", text);
    ImGui::PopTextWrapPos();
    PopFont();
}
// Section label inside a page ("Picture mode").
inline void SectionTitle(const char* text)
{
    ImGui::Dummy(ImVec2(0, S(6)));
    PushHelp();
    ImGui::TextColored(kTextDim, "%s", text);
    PopFont();
}
// Legacy section header (route code may still call it): title + rule.
inline void SectionHeader(const char* title, bool available = true,
                          const char* note = nullptr)
{
    ImGui::Dummy(ImVec2(0, S(8)));
    PushLabelStrong();
    ImGui::TextColored(available ? kText : kTextDim, "%s", title);
    PopFont();
    if (!available && note && note[0]) Hint(note);
    ImGui::Separator();
}

// One-line status: state = 0 dim, 1 ok (green), 2 attention (amber).
inline void StatusLine(int state, const char* text)
{
    ImVec4 c = state == 1 ? kOk : (state == 2 ? kWarn : kTextDim);
    PushLabel();
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextColored(c, "%s", text);
    ImGui::PopTextWrapPos();
    PopFont();
}

// Status banner: rounded box, dot + sentence. state: 1 confirmed (green),
// 2 pending/attention (amber), 0 neutral. The sentence carries the meaning.
inline void StatusBanner(int state, const char* text)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ImGui::GetContentRegionAvail().x;
    float pad = S(14);
    PushLabel();
    ImVec2 ts = ImGui::CalcTextSize(text, nullptr, false, w - pad * 2 - S(24));
    PopFont();
    float h = ts.y + pad * 2;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImU32 border = ImGui::GetColorU32(state == 2 ? Rgb(0x635139) : kLine);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(kBgRow), S(kRadiusGroup));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), border, S(kRadiusGroup));
    ImVec4 dotc = state == 1 ? kOk : (state == 2 ? kWarn : kTextDim);
    float r = S(4.5f);
    dl->AddCircleFilled(ImVec2(p.x + pad + r, p.y + h * 0.5f), r, ImGui::GetColorU32(dotc));
    ImGui::SetCursorScreenPos(ImVec2(p.x + pad + S(24), p.y + pad));
    PushLabel();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - pad * 2 - S(24));
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::Dummy(ImVec2(w, 0));
}

// Grouped rows: a rounded container; rows separated by a thin line.
inline void BeginGroupBox(const char* id)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgRow);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(kRadiusGroup));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(19), S(14)));
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                                            ImGuiChildFlags_AlwaysUseWindowPadding);
}
inline void EndGroupBox()
{
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}
inline void RowDivider()
{
    ImGui::Dummy(ImVec2(0, S(2)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, S(2)));
}

// A row: label + help on the left, a control of `control_w` on the right.
// Call BeginRow, draw the control, EndRow (which moves the cursor below the
// taller of the two, back at the row's left edge).
struct RowState { float x; float bottom; };
inline RowState& Row_()
{
    static RowState r{};
    return r;
}
inline void BeginRow(const char* label, const char* help, float control_w, const char* tooltip = nullptr)
{
    float avail = ImGui::GetContentRegionAvail().x;
    const bool stack = control_w > 0 && avail - control_w < S(230);
    const float text_w = stack ? avail : (std::max)(1.0f, avail - control_w - S(24));
    ImGui::PushID(label);
    Row_().x = ImGui::GetCursorScreenPos().x;
    ImGui::BeginGroup();
    PushLabel();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + text_w);
    ImGui::TextColored(kText, "%s", label);
    ImGui::PopTextWrapPos();
    PopFont();
    if (tooltip) ImGui::SetItemTooltip("%s", tooltip);
    if (help && help[0]) {
        PushHelp();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + text_w);
        ImGui::TextColored(kTextDim, "%s", help);
        ImGui::PopTextWrapPos();
        PopFont();
    }
    ImGui::EndGroup();
    ImVec2 grp_min = ImGui::GetItemRectMin(), grp_max = ImGui::GetItemRectMax();
    Row_().bottom = grp_max.y;
    if (stack) {
        ImGui::SetCursorScreenPos(ImVec2(grp_min.x, grp_max.y + S(7)));
    } else {
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(grp_min.x + avail - control_w, grp_min.y));
    }
}
inline void EndRow()
{
    float bottom = Row_().bottom;
    float item_bottom = ImGui::GetItemRectMax().y;
    if (item_bottom > bottom) bottom = item_bottom;
    ImGui::SetCursorScreenPos(ImVec2(Row_().x, bottom + ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
}

// The stock integer slider owns editing, navigation and its grab. Paint a tactile track
// over it using the same integer endpoints, leaving its keyboard focus outline visible.
inline void DrawSharpnessTrack(int percent, bool show_label)
{
    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    const float width = hi.x - lo.x;
    if (width < S(60)) return;
    const float grab = (std::min)(width - 4.f, (std::max)(ImGui::GetStyle().GrabMinSize, (width - 4.f) / 101.f));
    const float start = lo.x + 2.f + grab * .5f, end = hi.x - 2.f - grab * .5f;
    const float x = start + (end - start) * (percent / 100.f);
    const float r = S(5), y = hi.y - r - S(4);
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(lo, hi, ImGui::GetColorU32(kBgRaised), S(kRadiusControl));
    dl->AddLine({start, y}, {end, y}, ImGui::GetColorU32(kLineSoft), S(2));
    dl->AddLine({start, y}, {x, y}, ImGui::GetColorU32(kAccent), S(2));
    dl->AddCircleFilled({x, y + S(1)}, r, ImGui::GetColorU32(kBgNav));
    dl->AddCircleFilled({x, y}, r, ImGui::GetColorU32(Rgb(0xC7C2B4)));
    dl->AddLine({x - .55f*r, y - .48f*r}, {x + .55f*r, y - .48f*r}, ImGui::GetColorU32(kText));
    char value[8];
    std::snprintf(value, sizeof(value), "%d%%", percent);
    const float label_y = lo.y + S(2);
    dl->AddText({hi.x - S(8) - ImGui::CalcTextSize(value).x, label_y}, ImGui::GetColorU32(kText), value);
    if (show_label) dl->AddText({lo.x + S(8), label_y}, ImGui::GetColorU32(kText), "Sharpness");
}

// Small outlined tag ("Unavailable", "Planned"). Width for BeginRow via BadgeWidth.
inline float BadgeWidth(const char* text)
{
    PushHelp();
    float w = ImGui::CalcTextSize(text).x + S(18);
    PopFont();
    return w;
}
inline void Badge(const char* text)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    PushHelp();
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 sz(ts.x + S(18), ts.y + S(8));
    dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), ImGui::GetColorU32(kLineSoft), S(6));
    dl->AddText(ImVec2(p.x + S(9), p.y + S(4)), ImGui::GetColorU32(kTextDim), text);
    ImGui::Dummy(sz);
    PopFont();
}
inline void ValueText(const char* text)
{
    PushHelp();
    ImGui::TextColored(kTextDim, "%s", text);
    PopFont();
}

// ---- Controls ----
inline float SwitchWidth() { return S(45); }
inline float SwitchHeight() { return S(27); }
// Pill switch. Draws at the cursor; id from `id`.
inline bool Switch(const char* id, bool* value)
{
    const float w = SwitchWidth(), h = SwitchHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    bool clicked = ImGui::InvisibleButton("##switch", ImVec2(w, h));
    if (clicked) *value = !*value;
    bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = h * 0.5f;
    ImVec4 bgc = *value ? kSwitchOn : kSwitchOff;
    if (disabled) bgc.w = 0.5f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(bgc), r);
    if (ImGui::IsItemFocused() || ImGui::IsItemHovered())
        dl->AddRect(ImVec2(p.x - S(3), p.y - S(3)), ImVec2(p.x + w + S(3), p.y + h + S(3)),
                    ImGui::GetColorU32(kFocus), r + S(3), S(2));
    float pad = S(3);
    float kd = h - 2 * pad;
    float kx = *value ? p.x + w - pad - kd : p.x + pad;
    ImVec4 knob = Rgb(0xF5F6F8);
    if (disabled) knob.w = 0.6f;
    dl->AddCircleFilled(ImVec2(kx + kd * 0.5f, p.y + pad + kd * 0.5f), kd * 0.5f,
                        ImGui::GetColorU32(knob));
    ImGui::PopID();
    return clicked;
}
// Legacy one-row toggle: label left, switch right.
inline bool Toggle(const char* label, bool* value)
{
    BeginRow(label, nullptr, SwitchWidth());
    bool c = Switch(label, value);
    EndRow();
    return c;
}

inline float DropdownWidth() { return S(200); }
// Dropdown at the cursor (use inside BeginRow/EndRow).
inline bool Combo(const char* id, int* index, const char* const* items, int count, float width)
{
    ImGui::SetNextItemWidth(width);
    ImGui::PushID(id);
    PushLabel();
    bool changed = ImGui::Combo("##combo", index, items, count);
    PopFont();
    ImGui::PopID();
    return changed;
}
// Legacy labeled dropdown row.
inline bool Dropdown(const char* label, int* index, const char* const* items, int count)
{
    BeginRow(label, nullptr, DropdownWidth());
    bool c = Combo(label, index, items, count, DropdownWidth());
    EndRow();
    return c;
}
// Legacy labeled checkbox row.
inline bool CheckRow(const char* label, bool* value)
{
    BeginRow(label, nullptr, SwitchWidth());
    bool c = Switch(label, value);
    EndRow();
    return c;
}

// Mode card (radio): title, two short lines, selected state. `w` is the
// card width. Returns true when clicked.
inline bool ModeCard(const char* title, const char* line1, const char* line2,
                     bool selected, float w)
{
    ImGui::PushID(title);
    const float pad = S(14);
    const float text_w = (std::max)(1.f, w - pad * 2);
    const float title_w = (std::max)(1.f, text_w - S(13));
    PushLabelStrong();
    const float title_h = ImGui::CalcTextSize(title, nullptr, false, title_w).y;
    PopFont();
    PushHelp();
    const float line1_h = ImGui::CalcTextSize(line1, nullptr, false, text_w).y;
    const float line2_h = ImGui::CalcTextSize(line2, nullptr, false, text_w).y;
    PopFont();
    const float h = 2 * pad + title_h + S(8) + line1_h + S(4) + line2_h;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##card", ImVec2(w, h));
    bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    bool hovered = ImGui::IsItemHovered();
    bool focused = ImGui::IsItemFocused();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 bg = selected ? kBgCardOn : (hovered ? kBgHover : kBgCard);
    if (disabled) bg.w = 0.65f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(bg), S(kRadiusGroup));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h),
                ImGui::GetColorU32(selected ? kAccent : kLine), S(kRadiusGroup),
                selected ? S(2) : 1.0f);
    if (focused)
        dl->AddRect(ImVec2(p.x - S(3), p.y - S(3)), ImVec2(p.x + w + S(3), p.y + h + S(3)),
                    ImGui::GetColorU32(kFocus), S(kRadiusGroup) + S(3), S(2));
    ImVec4 tc = kText, hc = kTextDim;
    if (disabled) { tc.w = 0.65f; hc.w = 0.65f; }
    float x = p.x + pad, y = p.y + pad;
    PushLabelStrong();
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, y), ImGui::GetColorU32(tc), title, nullptr, title_w);
    PopFont();
    y += title_h + S(8);
    PushHelp();
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, y), ImGui::GetColorU32(hc), line1, nullptr, text_w);
    y += line1_h + S(4);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, y), ImGui::GetColorU32(hc), line2, nullptr, text_w);
    PopFont();
    if (selected) {
        // Check mark drawn with lines (no glyph dependency).
        float cs = S(11);
        ImVec2 c(p.x + w - pad - cs, p.y + S(11));
        ImU32 col = ImGui::GetColorU32(kAccent);
        dl->AddLine(ImVec2(c.x, c.y + cs * 0.55f), ImVec2(c.x + cs * 0.38f, c.y + cs), col, S(2));
        dl->AddLine(ImVec2(c.x + cs * 0.38f, c.y + cs), ImVec2(c.x + cs, c.y + cs * 0.1f), col, S(2));
    }
    ImGui::PopID();
    return clicked;
}

// Tabs share one toolbar; the selected page has an ivory capsule.
inline float NavButtonWidth(const char* label)
{
    PushLabel();
    const float width = ImGui::CalcTextSize(label).x + S(30);
    PopFont();
    return width;
}
inline bool NavButton(const char* label, bool current, float width)
{
    PushLabel();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetTextLineHeight() + S(12);
    const bool clicked = ImGui::InvisibleButton(label, {width,height}, ImGuiButtonFlags_EnableNav);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (current || ImGui::IsItemHovered())
        draw->AddRectFilled(pos, {pos.x+width,pos.y+height},
            ImGui::GetColorU32(current ? kBgNavOn : kBgHover), height/2);
    if (ImGui::IsItemFocused()) draw->AddRect(pos,{pos.x+width,pos.y+height},ImGui::GetColorU32(kFocus),height/2);
    draw->AddText({pos.x+S(15),pos.y+S(6)}, ImGui::GetColorU32(current ? kText : kTextDim),label);
    PopFont();
    return clicked;
}

// Plain button in the shared style.
inline bool Button(const char* label)
{
    PushLabel();
    bool c = ImGui::Button(label);
    PopFont();
    return c;
}
inline float ButtonWidth(const char* label)
{
    PushLabel();
    float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
    PopFont();
    return w;
}

// Collapsed technical/explanatory block ("How this affects the picture").
inline bool BeginDetails(const char* label)
{
    ImGui::Dummy(ImVec2(0, S(10)));
    ImGui::Separator();
    PushHelp();
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    bool open = ImGui::CollapsingHeader(label);
    ImGui::PopStyleColor(2);
    PopFont();
    return open;
}

// Render the product name as one italic run so both halves share the same face and spacing.
inline bool TitleBar(const char* game_name)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float size = BrandPx(), close_width = S(36);
    const ImU32 ink = ImGui::GetColorU32(kText);
    constexpr char wordmark[] = "NeuralAlias";
    ImGui::PushFont(Cfg().wordmark_italic ? Cfg().wordmark_italic : Cfg().display, size);
    const int begin = draw->VtxBuffer.Size;
    draw->AddText(pos, ink, wordmark);
    if (!Cfg().wordmark_italic) {
        for (int i = begin; i < draw->VtxBuffer.Size; ++i)
            draw->VtxBuffer[i].pos.x += (pos.y + size*.80f - draw->VtxBuffer[i].pos.y)*.20f;
    }
    const float brand_width = ImGui::CalcTextSize(wordmark).x + S(10);
    ImGui::PopFont();
    const float close_x = pos.x + width - close_width;
    if (game_name && *game_name) {
        PushHelp();
        const float name_width = ImGui::CalcTextSize(game_name).x;
        const float x = close_x - S(18) - name_width;
        if (x >= pos.x + brand_width + S(12))
            draw->AddText({x,pos.y+(size-HelpPx())*.5f}, ImGui::GetColorU32(kTextDim), game_name);
        PopFont();
    }
    ImGui::SetCursorScreenPos({close_x,pos.y+(size-close_width)*.5f});
    const bool close = ImGui::InvisibleButton("Close menu", {close_width,close_width}, ImGuiButtonFlags_EnableNav);
    const ImVec2 center{close_x+close_width*.5f,pos.y+size*.5f};
    draw->AddCircleFilled(center,close_width*.46f,ImGui::GetColorU32(ImGui::IsItemHovered() ? kBgHover : kBgRaised));
    const float r=S(5.5f);
    draw->AddLine({center.x-r,center.y-r},{center.x+r,center.y+r},ink,S(1.6f));
    draw->AddLine({center.x-r,center.y+r},{center.x+r,center.y-r},ink,S(1.6f));
    if (ImGui::IsItemFocused()) draw->AddCircle(center,close_width*.48f,ink);
    ImGui::SetCursorScreenPos({pos.x,pos.y});
    ImGui::Dummy({brand_width,size});
    return close;
}

} // namespace na::ui
