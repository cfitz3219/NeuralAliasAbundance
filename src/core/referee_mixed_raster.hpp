#pragma once

#include "core/referee_jitter.hpp"

namespace na::referee {

// One 11-pixel footprint roughly every 20 display pixels. The GPU returns centered
// gradient power, gradient/time covariance, temporal power and presented edge energy.
// There is no synchronous readback and the search only runs on signed native-grid pairs.
constexpr unsigned mixed_cols(unsigned w) { return (std::min)(192u, (w + 19) / 20); }
constexpr unsigned mixed_rows(unsigned h) { return (std::min)(108u, (h + 19) / 20); }
constexpr unsigned mixed_values = 10;
struct MixedRasterField {
    struct Cell { float v[mixed_values]{}; };
    std::vector<Cell> cells;
    unsigned w = 0, h = 0, pairs = 0;
    uint8_t strength = normal_jitter;
    float xmin = 0, xmax = 0, ymin = 0, ymax = 0, distance2 = 0;
    std::array<float,FlickerTransition::count> scene{};
    bool SamePicture(const std::array<float,FlickerTransition::count>& current) const {
        float sum = 0;
        for (unsigned i = 0; i < scene.size(); ++i) sum += std::fabs(current[i] - scene[i]);
        return sum < .10f * scene.size();
    }
    void Add(const float* values, unsigned width, unsigned height, float dx, float dy,
             const std::array<float,FlickerTransition::count>& view, uint8_t amount = normal_jitter) {
        if (w != width || h != height || strength != amount || !pairs || pairs >= 12 || !SamePicture(view)) {
            w = width; h = height; strength = amount; pairs = 0;
            cells.assign(mixed_cols(w) * mixed_rows(h), {});
            xmin = xmax = ymin = ymax = distance2 = 0;
            scene = view;
        }
        for (auto& cell : cells)
            for (auto& v : cell.v) v += *values++;
        xmin = (std::min)(xmin,dx); xmax = (std::max)(xmax,dx);
        ymin = (std::min)(ymin,dy); ymax = (std::max)(ymax,dy);
        distance2 += dx*dx + dy*dy;
        ++pairs;
    }
    struct Fit { float source = 0, output = 0, screen = 0, detail = 0; bool measured = false; };
    Fit At(unsigned i) const {
        Fit f;
        const float direction = .15f * strength / normal_jitter;
        if (i >= cells.size() || pairs < 6 || xmin > -direction || xmax < direction || ymin > -direction || ymax < direction) return f;
        const float* v = cells[i].v;
        float gain[3]{};
        for (unsigned p = 0; p < 3; ++p) {
            const float power = v[3*p], cross = v[3*p+1], delta = v[3*p+2];
            // Test sample support in units of spatial gradient, independent of jitter strength.
            if (!(power > distance2 * .0004f)) return f;
            gain[p] = cross / power;
            if (!(std::isfinite(gain[p]) && delta - gain[p]*cross <= .90f*power)) return f;
        }
        if (!(std::isfinite(v[9]) && v[9] > .02f*pairs && std::fabs(gain[1]-gain[2]) < .30f)) return f;
        return {gain[0],gain[1],gain[2],v[9]/pairs,true};
    }
    static bool Mismatch(const Fit& f) {
        return f.measured && std::fabs(f.source) < .33f && f.output < -.38f && f.screen < -.38f;
    }
    RegionalTiles::Position Position(unsigned i) const {
        const unsigned cols = mixed_cols(w), rows = mixed_rows(h);
        return {int(((i%cols)*2+1)*w/(2*cols)), int(((i/cols)*2+1)*h/(2*rows))};
    }
    bool Measure(const JitterTrial::Measurement* reference, JitterTrial::Measurement& m,
                 unsigned* matched = nullptr, uint8_t amount = normal_jitter) const {
        if (pairs < 6) return false;
        const float distance = reference ? reference->phase_step * amount / normal_jitter : std::sqrt(distance2/pairs);
        unsigned best = 0, count = 0;
        float weight = 0;
        unsigned controls[4]{};
        for (unsigned i = 0; i < cells.size(); ++i) {
            const auto f = At(i);
            if (!f.measured) continue;
            const auto pos = Position(i);
            const unsigned q = unsigned(2*pos.x >= int(w)) + 2u*unsigned(2*pos.y >= int(h));
            if (f.source > .60f && f.source < 1.50f && std::fabs(f.output)<.18f && std::fabs(f.screen)<.18f)
                ++controls[q];
            if (!Mismatch(f)) continue;
            ++count;
            if (reference) continue;
            m.witnesses.push_back({i, std::fabs(f.screen)*distance, f.detail});
            const float rank = (std::min)(1.f,-f.screen)*(std::min)(1.f,f.detail);
            if (rank > weight) { weight = rank; best = i; }
        }
        if (matched) *matched = count;
        unsigned supported = 0;
        for (auto healthy : controls) supported += healthy >= 4;
        if (supported < 3) return false;
        if (reference) {
            // A steadied edge may no longer be the strongest or may become
            // unmeasurable. Follow only the original group, with a majority
            // still present, instead of asking one extreme sample to survive.
            std::vector<float> screen_ratios, output_ratios, detail_ratios;
            unsigned locked = 0;
            for (const auto& witness : reference->witnesses) {
                const auto current = At(witness.index);
                if (!current.measured || std::fabs(current.source) > .5f) continue;
                ++m.regional_checks;
                screen_ratios.push_back(-current.screen*distance/witness.shift);
                output_ratios.push_back(-current.output*distance/witness.shift);
                detail_ratios.push_back(current.detail/witness.detail);
                locked += Mismatch(current);
                const float shift = std::fabs(current.screen)*distance;
                m.regional_improved += shift <= .75f*witness.shift;
                m.regional_worse += shift > 1.15f*witness.shift + .04f;
            }
            if (screen_ratios.empty() || 2*screen_ratios.size() < reference->witnesses.size()) return false;
            const auto median = [](std::vector<float>& values) {
                auto mid = values.begin()+values.size()/2;
                std::nth_element(values.begin(),mid,values.end());
                return *mid;
            };
            m.at = reference->at;
            m.shift = reference->shift * median(screen_ratios);
            m.reconstructed = reference->shift * median(output_ratios);
            m.detail = reference->detail * median(detail_ratios);
            m.locked = 4*locked >= 3*m.regional_checks;
            m.field = true;
            m.phase_step = distance;
            return true;
        }
        if (!weight) return false;
        const auto fit = At(best);
        m.phase_step = distance;
        m.at = Position(best); m.shift = fit.screen * distance; m.reconstructed = fit.output * distance;
        m.detail = fit.detail; m.locked = Mismatch(fit); m.field = true;
        return true;
    }
};

} // namespace na::referee
