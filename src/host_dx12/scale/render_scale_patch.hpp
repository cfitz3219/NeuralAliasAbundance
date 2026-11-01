// P4-4 (slider route): read-time render-scale substitution (approved by Cort 2026-11-01).
//
// At start-up a game reads its own settings file. The file-read hook (separate module) hands
// the bytes the game just read to PatchRenderScale, which finds a known render-scale key and
// rewrites its value IN THOSE BYTES ONLY, keeping the exact length (the game may have sized
// its buffer from the file size). The file on disk is never written. J2 then confirms in the
// same launch that the scene got smaller; if not, the overlay's guide takes over.
//
// Text formats: INI / CFG ("Key = 1.0"), JSON ("\"Key\": 1.0"), YAML ("Key: 1.0").
// Comment lines (; # //) are skipped. A key must be a whole identifier (DynamicResolutionScale
// is not ResolutionScale). Pure C++, no Windows dependency.
#pragma once
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace na::scale {

enum class Unit { Fraction, Percent };

struct Match {
    size_t key_pos = 0, value_pos = 0, value_len = 0;
    double current = 0;   // the value as the game stored it, in its own unit
    Unit unit = Unit::Fraction;
    bool quoted = false;  // the value is inside quotes ("1.0")
    char key[40] = {};
};

inline const char* const kKeys[] = {
    "RenderScale", "RenderScaling", "RenderResolutionScale", "ResolutionScale", "ResolutionScaling",
    "ScreenPercentage", "RenderScalePercent", "ResolutionPercentage", "InternalResolutionScale",
};

namespace detail {
inline bool ident(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
inline bool ieq(const char* a, const char* b, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
// True when position p lies on a comment line (first non-blank characters ; # or //).
inline bool comment_line(const char* d, size_t p)
{
    size_t s = p;
    while (s > 0 && d[s - 1] != '\n' && d[s - 1] != '\r') --s;
    while (s < p && (d[s] == ' ' || d[s] == '\t')) ++s;
    return d[s] == ';' || d[s] == '#' || (d[s] == '/' && s + 1 < p && d[s + 1] == '/');
}
// Parses `key <ws> ["] <ws> (= | :) <ws> ["] number` at d[p] (p at the key's first char).
inline bool parse_at(const char* d, size_t n, size_t p, size_t klen, Match* m)
{
    if (p > 0 && ident(d[p - 1])) return false;
    size_t q = p + klen;
    if (q < n && ident(d[q])) return false;
    if (q < n && d[q] == '"') ++q;
    while (q < n && (d[q] == ' ' || d[q] == '\t')) ++q;
    if (q >= n || (d[q] != '=' && d[q] != ':')) return false;
    ++q;
    while (q < n && (d[q] == ' ' || d[q] == '\t')) ++q;
    bool quoted = false;
    if (q < n && d[q] == '"') { quoted = true; ++q; }
    const size_t v = q;
    bool dot = false, digit = false;
    while (q < n && (std::isdigit(static_cast<unsigned char>(d[q])) || (d[q] == '.' && !dot))) {
        if (d[q] == '.') dot = true; else digit = true;
        ++q;
    }
    if (!digit || q - v > 15) return false;
    if (q < n && (ident(d[q]) || d[q] == '.')) return false; // 1.0e5, 100px, 1.2.3: not a plain number
    char buf[16] = {};
    std::memcpy(buf, d + v, q - v);
    m->key_pos = p;
    m->value_pos = v;
    m->value_len = q - v;
    m->current = std::atof(buf);
    m->quoted = quoted;
    return true;
}
} // namespace detail

// Finds the first non-comment render-scale entry. Unit: a key naming a percentage, or a
// current value above 4, is a percentage; otherwise a fraction (1.0 = native).
inline bool FindRenderScale(const char* d, size_t n, Match* m, size_t from = 0)
{
    if (!d || !m) return false;
    for (size_t p = from; p < n; ++p) {
        if (!std::isalpha(static_cast<unsigned char>(d[p]))) continue;
        for (const char* k : kKeys) {
            const size_t kl = std::strlen(k);
            if (p + kl > n || !detail::ieq(d + p, k, kl)) continue;
            Match t;
            if (!detail::parse_at(d, n, p, kl, &t) || detail::comment_line(d, p)) continue;
            std::snprintf(t.key, sizeof t.key, "%s", k);
            const bool pct_key = std::strstr(k, "Percent") != nullptr;
            t.unit = (pct_key || t.current > 4.0) ? Unit::Percent : Unit::Fraction;
            if (t.current <= 0) continue;
            *m = t;
            return true;
        }
    }
    return false;
}

// `fraction` (render / output, e.g. 0.666667) in the match's unit, written in exactly
// value_len characters. Shorter numbers get trailing zeros after a decimal point, or a leading
// space when the value is unquoted. False when it cannot be written within 1 % in that width.
// `tag`: nudge the last decimal digit by one (0.666667 -> 0.666668) so the value the game parses
// is distinguishable in its memory (live mode switching); only with at least 4 decimals.
inline bool FormatSameWidth(double fraction, const Match& m, char* out /* value_len + 1 */, bool tag = false)
{
    const size_t w = m.value_len;
    if (!w || w > 15 || !(fraction > 0)) return false;
    const double v = m.unit == Unit::Percent ? fraction * 100.0 : fraction;
    char s[32] = {};
    int len = -1;
    for (int dec = 12; dec >= 0; --dec) {
        len = std::snprintf(s, sizeof s, "%.*f", dec, v);
        if (len > 0 && static_cast<size_t>(len) <= w) break;
        len = -1;
    }
    if (len <= 0 || std::fabs(std::atof(s) - v) > 0.01 * v) return false;
    size_t l = static_cast<size_t>(len);
    if (l < w && !std::strchr(s, '.') && w - l >= 2) { s[l++] = '.'; }
    if (l < w && std::strchr(s, '.')) { while (l < w) s[l++] = '0'; }
    if (l < w) {
        if (m.quoted) return false;
        const size_t pad = w - l;
        std::memmove(s + pad, s, l);
        std::memset(s, ' ', pad);
        l = w;
    }
    s[l] = 0;
    if (tag) {
        const char* dot = std::strchr(s, '.');
        if (!dot || static_cast<size_t>(l - (dot - s) - 1) < 4) return false;
        char& last = s[l - 1];
        last = last == '9' ? '8' : static_cast<char>(last + 1);
    }
    std::memcpy(out, s, w + 1);
    return true;
}

// Rewrites every non-comment render-scale value in place (same length). Returns how many were
// rewritten; `first` gets the first match (also when nothing could be written).
inline int PatchRenderScale(char* d, size_t n, double fraction, Match* first = nullptr, bool tag = false)
{
    int patched = 0;
    bool have_first = false;
    Match m;
    for (size_t from = 0; FindRenderScale(d, n, &m, from); from = m.value_pos + m.value_len) {
        if (!have_first && first) { *first = m; have_first = true; }
        char v[16] = {};
        if (!FormatSameWidth(fraction, m, v, tag) && !(tag && FormatSameWidth(fraction, m, v))) continue;
        std::memcpy(d + m.value_pos, v, m.value_len);
        ++patched;
    }
    return patched;
}

} // namespace na::scale
