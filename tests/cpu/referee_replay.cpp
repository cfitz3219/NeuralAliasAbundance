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
        if (screen) {
            Image comparison{width, height, std::vector<float>(count)};
            SampleThumb(current, values.data() + BlockOffset(kThumbInput));
            SampleTiles(current, values.data() + BlockOffset(kTileInput), active_tiles);
            if (!Read(comparison.v.data(), count)) return 2;
            SampleThumb(comparison, values.data() + BlockOffset(kThumbOutput));
            SampleTiles(comparison, values.data() + BlockOffset(kTileOutput), active_tiles);
            if (!Read(comparison.v.data(), count)) return 2;
            SampleThumb(comparison, values.data() + BlockOffset(kThumbScreen));
            SampleTiles(comparison, values.data() + BlockOffset(kTileScreen), active_tiles);
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
        const auto score = Score({values.data(), screen, screen, pair, std::hypot(dx, dy)});
        recent.Add(context, ordinal, score);
        all_frames.Add(score);
        if (screen && score.tiles_textured < kTiles / 2) {
            locations = FindTexturedTiles(values.data());
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
    std::printf("entire_recording samples=%u output_frames=%u input_frames=%u neither_frames=%u motion_pairs=%u\n",
                all_frames.samples, all_frames.output_frames, all_frames.input_frames, all_frames.neither_frames, all_frames.motions);
    return 0;
}
