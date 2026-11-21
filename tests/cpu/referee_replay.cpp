#include "core/referee_selection.hpp"
#include <array>
#include <cstdio>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
using namespace na::referee;
using namespace na::referee::shadow;

template<class T>
bool Read(T* data, size_t count)
{
    return std::fread(data, sizeof(T), count, stdin) == count;
}

void Report(const RecentSamples& recent, const Context& context, uint64_t last)
{
    uint64_t first;
    const auto evidence = recent.For(context, first);
    if (!evidence.measured.samples) return;
    const auto choice = recent.Choose(evidence);
    char details[2048];
    Format(details, sizeof details, evidence.measured, Judge(evidence.measured));
    std::printf("frames=%llu..%llu current=%s action=%s candidate=%s why=\"%s\"\n%s\n",
                static_cast<unsigned long long>(first), static_cast<unsigned long long>(last),
                Name(evidence.context.placement.path), choice.action, Name(choice.candidate.path), choice.reason, details);
}

// Mirror the GPU's linear-luminance screen resampling and centred copy alternatives.
void SampleDisplay(const Image& screen, uint32_t w, uint32_t h, const TileLocations& locations,
                   uint32_t layout, float* dest)
{
    for (uint32_t k = 0; k < kTiles; ++k) {
        for (int j = 0; j < int(kTile); ++j) for (int i = 0; i < int(kTile); ++i) {
            const int x = TileLeft(k, w, locations) + i, y = TileTop(k, h, locations) + j;
            float luma;
            if (layout) {
                const uint32_t dx = screen.w - w, dy = screen.h - h, nx = 1 + (dx & 1u);
                luma = screen.At(x + int(dx / 2 + (layout - 1) % nx), y + int(dy / 2 + (layout - 1) / nx));
            } else {
                const float sx = (float(x) + .5f) * screen.w / w - .5f, sy = (float(y) + .5f) * screen.h / h - .5f;
                const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                const float fx = sx - ix, fy = sy - iy;
                auto tap = [&](int a, int b) { return (std::max)(0.f, screen.At(a, b)); };
                const float a = tap(ix, iy) * (1 - fx) + tap(ix + 1, iy) * fx;
                const float b = tap(ix, iy + 1) * (1 - fx) + tap(ix + 1, iy + 1) * fx;
                luma = a * (1 - fy) + b * fy;
            }
            dest[(k * kTile + j) * kTile + i] = LogLuma(luma);
        }
    }
}
}

int main()
{
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) return 2;
#endif
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    RecentSamples recent;
    Window all_frames;
    Context previous_context;
    bool have_previous = false;
    Image previous;
    TileLocations locations;
    Context tile_context;
    std::array<float, 2> prior_jitter{};
    uint64_t previous_ordinal = 0;
    std::vector<float> values(kResultFloats);
    std::vector<float> padded;
    uint32_t padded_frames = 0, centered_frames = 0, scaled_output_votes = 0, selected_output_votes = 0;
    for (;;) {
        uint64_t ids[5]; // ordinal, colour, depth, boundary and history generation
        const size_t got = std::fread(ids, 1, sizeof ids, stdin);
        if (got == 0 && std::feof(stdin)) break;
        uint32_t info[5]; // route, subresource, width, height and flags (screen, motion, reset)
        float temporal[4]; // jitter xy and motion scale xy
        if (got != sizeof ids || !Read(info, 5) || !Read(temporal, 4)) return 2;
        const uint32_t width = info[2], height = info[3], flags = info[4];
        if (!width || !height || width > 8192 || height > 8192 || info[0] > uint32_t(Path::Native11)) return 2;
        const uint64_t ordinal = ids[0];
        uint32_t display[2]{width, height};
        if ((flags & 8) && (!(flags & 1) || !Read(display, 2) || !display[0] || !display[1] ||
            display[0] > 8192 || display[1] > 8192)) return 2;
        if (have_previous && ordinal <= previous_ordinal) return 2;
        const Context context{{Path(info[0]), ids[3]}, ids[1], ids[2], ids[4], info[1], width, height};
        if (have_previous && (!(previous_context == context) || (flags & 4))) {
            Report(recent, previous_context, previous_ordinal);
            previous = {};
        }
        if (flags & 4) { recent.Clear(); locations = {}; }
        const TileLocations active_tiles = context == tile_context ? locations : TileLocations{};

        const size_t count = size_t(width) * height;
        Image current{width, height, std::vector<float>(count)};
        if (!Read(current.v.data(), count)) return 2;
        const bool screen = (flags & 1) != 0;
        bool tiles = screen && width == display[0] && height == display[1];
        const uint32_t layouts = screen ? PaddedLayouts(width, height, display[0], display[1]) : 0;
        const float* registered = nullptr;
        if (screen) {
            Image comparison{width, height, std::vector<float>(count)};
            SampleThumb(current, values.data() + BlockOffset(kThumbInput));
            SampleTiles(current, values.data() + BlockOffset(kTileInput), active_tiles);
            if (!Read(comparison.v.data(), count)) return 2;
            SampleThumb(comparison, values.data() + BlockOffset(kThumbOutput));
            SampleTiles(comparison, values.data() + BlockOffset(kTileOutput), active_tiles);
            comparison.w = display[0];
            comparison.h = display[1];
            comparison.v.resize(size_t(display[0]) * display[1]);
            if (!Read(comparison.v.data(), comparison.v.size())) return 2;
            SampleThumb(comparison, values.data() + BlockOffset(kThumbScreen));
            if (tiles) SampleTiles(comparison, values.data() + BlockOffset(kTileScreen), active_tiles);
            if (layouts) {
                SampleDisplay(comparison, width, height, active_tiles, 0, values.data() + BlockOffset(kTileScreen));
                padded.resize(layouts * kTileFloats);
                for (uint32_t n = 0; n < layouts; ++n)
                    SampleDisplay(comparison, width, height, active_tiles, n + 1, padded.data() + n * kTileFloats);
                bool scaled_matches = false;
                const auto mapping = RegisterScreenTiles(values.data(), padded.data(), layouts, scaled_matches);
                if (mapping) registered = padded.data() + (mapping - 1) * kTileFloats;
                tiles = mapping || scaled_matches || (context.placement.path == Path::Native11 &&
                    std::abs(double(display[0]) / display[1] - double(width) / height) < .01 * width / height);
                ++padded_frames;
                if (mapping) ++centered_frames;
                if (VoteScreen(Score({values.data(), true, true, false, 0})) == ScreenVote::Output) ++scaled_output_votes;
            }
        }
        bool pair = false;
        const float dx = temporal[0] - prior_jitter[0], dy = temporal[1] - prior_jitter[1];
        if (flags & 2) {
            std::vector<float> motion(count * 2);
            if (!Read(motion.data(), motion.size())) return 2;
            pair = previous.w == width && previous.h == height && previous_ordinal + 1 == ordinal;
            if (pair) {
                MotionInputs input{&current, &previous, &motion, width, height, temporal[2], temporal[3],
                                   {temporal[0], temporal[1]}, {prior_jitter[0], prior_jitter[1]}};
                SamplePoints(input, values.data() + BlockOffset(kPoints));
            }
        }
        const auto score = Score({values.data(), screen, tiles, pair, std::hypot(dx, dy), registered});
        if (layouts && VoteScreen(score) == ScreenVote::Output) ++selected_output_votes;
        recent.Add(context, ordinal, score);
        all_frames.Add(score);
        const bool ambiguous = score.tiles_textured >= kTiles / 2 && score.tiles_input + score.tiles_output < 2;
        if ((tiles || layouts) && (score.tiles_textured < kTiles / 2 || ambiguous)) {
            locations = FindTexturedTiles(values.data(), ambiguous, active_tiles);
            tile_context = context;
        }
        previous_context = context;
        have_previous = true;
        previous = std::move(current);
        previous_ordinal = ordinal;
        prior_jitter = {temporal[0], temporal[1]};
    }
    if (!have_previous) return 2;
    Report(recent, previous_context, previous_ordinal);
    if (padded_frames) std::printf("padded_displays=%u centered=%u output_votes_scaled=%u output_votes_registered=%u\n",
                                  padded_frames, centered_frames, scaled_output_votes, selected_output_votes);
    std::printf("entire_recording samples=%u output_frames=%u input_frames=%u neither_frames=%u motion_pairs=%u\n",
                all_frames.samples, all_frames.output_frames, all_frames.input_frames, all_frames.neither_frames, all_frames.motions);
    return 0;
}
