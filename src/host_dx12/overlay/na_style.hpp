// NeuralAlias UI style â€” the single shared implementation of docs/UI-STYLE.md.
// Both routes call na::ui::ApplyStyle() / LoadFonts() and the widget helpers;
// do not fork per route. Header-only so Route B can include it without
// linking Route A code.
//
// A30 direction (Astra's redesign, approved by the user): quiet dark
// surfaces, readable type, generous spacing, one restrained blue selection
// accent. Premium through alignment and clarity; no glass, charts, gradients
// or a coding-tool look. One logo, in the title bar. Every metric derives
// from one scale factor (SetScale, default 1.25 since A42) plus an optional
// "Bold" text weight preference (A44; it replaced the earlier "Large" size).
// Colour always accompanies words: no state is conveyed by colour alone.
#pragma once

#include <imgui.h>

#include <cstring>

namespace na::ui {

inline ImVec4 Rgb(unsigned hex, float a = 1.0f)
{
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f,
                  (hex & 0xFF) / 255.0f, a);
}

// ---- Tokens (DESIGN.md "Tokens and layout") ----
inline const ImVec4 kBg       = Rgb(0x191B1F);        // window
inline const ImVec4 kBgNav    = Rgb(0x16181B);        // navigation column
inline const ImVec4 kBgRow    = Rgb(0x202328);        // rows / groups
inline const ImVec4 kBgRaised = Rgb(0x2B2E34);        // buttons, inputs
inline const ImVec4 kBgHover  = Rgb(0x343840);
inline const ImVec4 kBgCard   = Rgb(0x23262B);        // mode cards
inline const ImVec4 kBgCardOn = Rgb(0x283548);        // selected card
inline const ImVec4 kBgNavOn  = Rgb(0x2A3039);        // current page
inline const ImVec4 kLine     = Rgb(0x373B42);
inline const ImVec4 kLineSoft = Rgb(0x505660);
inline const ImVec4 kText     = Rgb(0xF1F2F4);
inline const ImVec4 kTextDim  = Rgb(0xB2B7C0);
inline const ImVec4 kAccent   = Rgb(0x85B5FF);        // selection
inline const ImVec4 kFocus    = Rgb(0xA9CDFF);        // keyboard/controller focus
inline const ImVec4 kSwitchOn = Rgb(0x5283D0);
inline const ImVec4 kSwitchOff= Rgb(0x59616B);
inline const ImVec4 kOk       = Rgb(0x9BDBB5);        // "good", with words
inline const ImVec4 kWarn     = Rgb(0xF1CA8A);        // "attention", with words
inline const ImVec4 kError    = Rgb(0xFF8A94);        // failures, with words
// Compatibility aliases (earlier tokens still referenced by route code).
inline const ImVec4 kBgPanel  = kBg;
inline const ImVec4 kSignal   = kAccent;
inline const ImVec4 kSignalDim= kSwitchOn;

// ---- Base metrics at scale 1.0 (default scale 1.25 since A42). ----
// A44: every text size is 25 % smaller than A30's (22/20/16/39/27), per the
// user after session 7; the default window follows, and the window is now
// movable and resizable down to kBaseMinWindow* (text does not shrink with
// it; the layout reflows: navigation moves on top below kBaseNarrowWidth,
// mode cards go 2x2, rows wrap their text).
constexpr float kBaseBodyPx = 16.5f;
constexpr float kBaseLabelPx = 15.0f;
constexpr float kBaseHelpPx = 12.0f;
constexpr float kBaseTitlePx = 29.0f;   // page heading
constexpr float kBaseBrandPx = 20.0f;   // "NeuralAlias" in the title bar
constexpr float kBaseWindowWidth = 1120.0f;
constexpr float kBaseWindowHeight = 640.0f;
constexpr float kBaseMinWindowWidth = 560.0f;
constexpr float kBaseMinWindowHeight = 380.0f;
constexpr float kBaseNavWidth = 200.0f;
constexpr float kBaseNarrowWidth = 700.0f;  // below: nav on top (cards go 2x2 by available width)
constexpr float kRadiusWindow = 22.0f, kRadiusGroup = 12.0f, kRadiusControl = 10.0f;

struct Config {
    float scale = 1.25f;  // A42: the user asked for half of the 2.5 default after session 6
    bool bold = false;    // A44: Preferences > Text weight: Bold (body text set in the heavier face)
    char font_body[260] = "C:\\Windows\\Fonts\\Inter-Regular.ttf";
    char font_title[260] = "C:\\Windows\\Fonts\\Inter-SemiBold.ttf";
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
inline float NavWidth() { return S(kBaseNavWidth); }
// The face used for running text: the regular face, or the heavier title
// face when the user chose Bold (A44).
inline ImFont* TextFace() { return (Cfg().bold && Cfg().title) ? Cfg().title : Cfg().body; }

// Call once after ImGui::CreateContext(), before the backend init. ImGui
// 1.92 fonts are dynamically sized, so one face per weight is enough.
inline void LoadFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    Config& c = Cfg();
    c.body = io.Fonts->AddFontFromFileTTF(c.font_body, S(kBaseBodyPx));
    if (!c.body) c.body = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", S(kBaseBodyPx));
    c.title = io.Fonts->AddFontFromFileTTF(c.font_title, S(kBaseTitlePx));
    if (!c.title) c.title = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuisb.ttf", S(kBaseTitlePx));
    if (!c.title) c.title = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", S(kBaseTitlePx));
    if (!c.body) {
        ImFontConfig fc;
        fc.SizePixels = S(kBaseBodyPx);
        c.body = io.Fonts->AddFontDefault(&fc);
    }
    if (!c.title) c.title = c.body;
    io.FontDefault = c.body;
}

inline void ApplyStyle()
{
    ImGuiStyle& s = ImGui::GetStyle();
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
    c[ImGuiCol_NavCursor] = kFocus;
    c[ImGuiCol_TableBorderLight] = kLine;
    c[ImGuiCol_TableBorderStrong] = kLine;
}

// ---- Typography helpers ----
inline void PushBody() { ImGui::PushFont(TextFace(), BodyPx()); }
inline void PushLabel() { ImGui::PushFont(TextFace(), LabelPx()); }
inline void PushLabelStrong() { ImGui::PushFont(Cfg().title, LabelPx()); }
inline void PushHelp() { ImGui::PushFont(TextFace(), HelpPx()); }
inline void PushTitle() { ImGui::PushFont(Cfg().title, TitlePx()); }
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
    ImGui::TextColored(c, "%s", text);
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
    ImU32 border = ImGui::ColorConvertFloat4ToU32(state == 2 ? Rgb(0x635139) : kLine);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::ColorConvertFloat4ToU32(kBgRow), S(kRadiusGroup));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), border, S(kRadiusGroup));
    ImVec4 dotc = state == 1 ? kOk : (state == 2 ? kWarn : kTextDim);
    float r = S(4.5f);
    dl->AddCircleFilled(ImVec2(p.x + pad + r, p.y + h * 0.5f), r, ImGui::ColorConvertFloat4ToU32(dotc));
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
inline void BeginRow(const char* label, const char* help, float control_w)
{
    float avail = ImGui::GetContentRegionAvail().x;
    float text_w = avail - control_w - S(24);
    if (text_w < avail * 0.4f) text_w = avail * 0.4f;
    ImGui::PushID(label);
    Row_().x = ImGui::GetCursorScreenPos().x;
    ImGui::BeginGroup();
    PushLabel();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + text_w);
    ImGui::TextColored(kText, "%s", label);
    ImGui::PopTextWrapPos();
    PopFont();
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
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(grp_min.x + avail - control_w, grp_min.y));
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
    dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), ImGui::ColorConvertFloat4ToU32(kLineSoft), S(6));
    dl->AddText(ImVec2(p.x + S(9), p.y + S(4)), ImGui::ColorConvertFloat4ToU32(kTextDim), text);
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
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::ColorConvertFloat4ToU32(bgc), r);
    if (ImGui::IsItemFocused() || ImGui::IsItemHovered())
        dl->AddRect(ImVec2(p.x - S(3), p.y - S(3)), ImVec2(p.x + w + S(3), p.y + h + S(3)),
                    ImGui::ColorConvertFloat4ToU32(kFocus), r + S(3), S(2));
    float pad = S(3);
    float kd = h - 2 * pad;
    float kx = *value ? p.x + w - pad - kd : p.x + pad;
    ImVec4 knob = Rgb(0xF5F6F8);
    if (disabled) knob.w = 0.6f;
    dl->AddCircleFilled(ImVec2(kx + kd * 0.5f, p.y + pad + kd * 0.5f), kd * 0.5f,
                        ImGui::ColorConvertFloat4ToU32(knob));
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
    PushLabel();
    float title_h = ImGui::GetTextLineHeight();
    PopFont();
    PushHelp();
    float help_h = ImGui::GetTextLineHeight();
    PopFont();
    float pad = S(14);
    float h = pad + title_h + S(8) + help_h * 2 + S(4) + pad;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##card", ImVec2(w, h));
    bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    bool hovered = ImGui::IsItemHovered();
    bool focused = ImGui::IsItemFocused();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 bg = selected ? kBgCardOn : (hovered ? kBgHover : kBgCard);
    if (disabled) bg.w = 0.65f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::ColorConvertFloat4ToU32(bg), S(kRadiusGroup));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h),
                ImGui::ColorConvertFloat4ToU32(selected ? kAccent : kLine), S(kRadiusGroup),
                selected ? S(2) : 1.0f);
    if (focused)
        dl->AddRect(ImVec2(p.x - S(3), p.y - S(3)), ImVec2(p.x + w + S(3), p.y + h + S(3)),
                    ImGui::ColorConvertFloat4ToU32(kFocus), S(kRadiusGroup) + S(3), S(2));
    ImVec4 tc = kText, hc = kTextDim;
    if (disabled) { tc.w = 0.65f; hc.w = 0.65f; }
    float x = p.x + pad, y = p.y + pad;
    PushLabelStrong();
    dl->AddText(ImVec2(x, y), ImGui::ColorConvertFloat4ToU32(tc), title);
    PopFont();
    y += title_h + S(8);
    PushHelp();
    dl->AddText(ImVec2(x, y), ImGui::ColorConvertFloat4ToU32(hc), line1);
    y += help_h + S(2);
    dl->AddText(ImVec2(x, y), ImGui::ColorConvertFloat4ToU32(hc), line2);
    PopFont();
    if (selected) {
        // Check mark drawn with lines (no glyph dependency).
        float cs = S(11);
        ImVec2 c(p.x + w - pad - cs, p.y + S(11));
        ImU32 col = ImGui::ColorConvertFloat4ToU32(kAccent);
        dl->AddLine(ImVec2(c.x, c.y + cs * 0.55f), ImVec2(c.x + cs * 0.38f, c.y + cs), col, S(2));
        dl->AddLine(ImVec2(c.x + cs * 0.38f, c.y + cs), ImVec2(c.x + cs, c.y + cs * 0.1f), col, S(2));
    }
    ImGui::PopID();
    return clicked;
}

// Navigation entry. Returns true when clicked. w <= 0 sizes to the label.
inline float NavButtonWidth(const char* label)
{
    PushLabel();
    float w = ImGui::CalcTextSize(label).x + S(32);
    PopFont();
    return w;
}
inline bool NavButton(const char* label, bool current, float w)
{
    if (w <= 0) w = NavButtonWidth(label);
    ImGui::PushID(label);
    PushLabel();
    float h = ImGui::GetTextLineHeight() + S(24);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##nav", ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (current || ImGui::IsItemHovered())
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                          ImGui::ColorConvertFloat4ToU32(current ? kBgNavOn : kBgHover), S(kRadiusControl));
    if (ImGui::IsItemFocused())
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), ImGui::ColorConvertFloat4ToU32(kFocus), S(kRadiusControl), S(2));
    dl->AddText(ImVec2(p.x + S(16), p.y + S(12)), ImGui::ColorConvertFloat4ToU32(current ? kText : kTextDim), label);
    PopFont();
    ImGui::PopID();
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

// The logo: a three-step pixel staircase. Used ONCE, in the title bar.
inline void Logo(ImVec4 color, float size)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float line_h = ImGui::GetTextLineHeight();
    p.y += (line_h - size) * 0.5f;
    float u = size / 3.0f;
    ImU32 col = ImGui::ColorConvertFloat4ToU32(color);
    dl->AddRectFilled(ImVec2(p.x, p.y + 2 * u), ImVec2(p.x + 3 * u, p.y + 3 * u), col);
    dl->AddRectFilled(ImVec2(p.x + u, p.y + u), ImVec2(p.x + 3 * u, p.y + 2 * u), col);
    dl->AddRectFilled(ImVec2(p.x + 2 * u, p.y), ImVec2(p.x + 3 * u, p.y + u), col);
    ImGui::Dummy(ImVec2(size, line_h));
    ImGui::SameLine(0.0f, S(12.0f));
}

// Title bar: logo, product name, optional game name, close button at right.
// Returns true when the close button was pressed.
inline bool TitleBar(const char* game_name)
{
    ImGui::PushFont(Cfg().title, BrandPx());
    Logo(Rgb(0xDFE5EF), BrandPx() * 0.8f);
    ImGui::TextColored(kText, "NeuralAlias");
    ImGui::PopFont();
    if (game_name && game_name[0]) {
        ImGui::SameLine(0.0f, S(18));
        PushHelp();
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y), ImVec2(p.x, p.y + BrandPx()),
                                            ImGui::ColorConvertFloat4ToU32(kLine));
        ImGui::SetCursorScreenPos(ImVec2(p.x + S(18), p.y + (BrandPx() - ImGui::GetTextLineHeight()) * 0.5f));
        ImGui::TextColored(kTextDim, "%s", game_name);
        PopFont();
    }
    // Close button, right-aligned in the window.
    float bw = S(44);
    ImGui::SameLine(ImGui::GetWindowWidth() - S(30) - bw);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool close = ImGui::InvisibleButton("##close", ImVec2(bw, BrandPx()));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
        dl->AddRectFilled(p, ImVec2(p.x + bw, p.y + BrandPx()), ImGui::ColorConvertFloat4ToU32(kBgHover), S(kRadiusControl));
    float cx = p.x + bw * 0.5f, cy = p.y + BrandPx() * 0.5f, a = S(7);
    ImU32 col = ImGui::ColorConvertFloat4ToU32(kTextDim);
    dl->AddLine(ImVec2(cx - a, cy - a), ImVec2(cx + a, cy + a), col, S(2));
    dl->AddLine(ImVec2(cx - a, cy + a), ImVec2(cx + a, cy - a), col, S(2));
    return close;
}

} // namespace na::ui
