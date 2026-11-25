// Our own optical flow, run offline on a capture-format folder, with the motion it hands on saved
// as planes the replay tool takes with --motion-dir.
//
// Why it exists (Cort, 2026-11-25, the "fold the shading" trials): at half size DLSS leans on the
// motion it is given far more than DLAA does. The made-up trial scene (scripts/fold_trial_scene.py)
// has leaves that sway without that sway being in the supplied motion. The question: what does OUR
// flow, the program a game run uses (compute_flow -> arbiter, nothing rewritten here), make of
// those leaves at full and at half size, at the strict acceptance rules and at looser ones, alone
// and together with the supplied motion.
//
//   flow_motion_dump <capture folder> <out folder> --mode none|camera|total [--loosen 0..100] [--frames N]
//                    [--fold-source quality|performance | --fold-input quality|performance]
//
//   --fold-input L    run on the folded picture of level L (one pixel of each block kept, the last one, as
//                   the live build keeps it) instead of the full-size one: what the motion stage costs and
//                   measures when it runs at the size DLSS gets.
//   --fold-source L   the full-size picture was shaded in level L's blocks (the flow smooths over a block).
//   --sample L        the whole-picture search, but `measure` and the arbiter decide only the pixel level L
//                   keeps of each block (Astra's plan, COMMS 2026-11-25): the output is folded size, and
//                   each kept vector should be exactly the whole-picture run's vector at that pixel.
//   --compare DIR     a whole-picture run's output folder (its motion.raw planes): every frame's kept
//                   vectors are compared with that run's at the same pixels (bit for bit, and the largest
//                   difference), when this run samples or folds.
//   Every run logs the GPU time of the motion stage (the flow and the arbiter, timestamps) per frame and
//   its median. Real game captures (rows padded to 256 bytes, the last row not) are read as well.
//
//   --mode none     no camera motion is known (the camera says "nothing moved"); the flow's
//                   accepted measurements are all the motion there is. Optical flow alone, with its
//                   acceptance rules (--loosen moves them: 0 strict, 100 loosest).
//   --mode camera   the supplied motion is the camera's (the scene's pan, exact for everything that
//                   only moves with the camera) and the flow corrects it where the pictures show
//                   something moving by itself. The folder's metadata must carry "trial_shear_px":
//                   motion in pixels per unit of depth (the scene's parallax is its depth times a pan).
//   --mode total    the program's no-camera path: total motion measured on the two pictures, with
//                   the screen's own motion fitted where they cannot measure. --loosen is not read there.
//
// Out: NNNNN-motion.raw (two 32-bit floats a pixel, tight rows, pixels, previous minus current: the
// replay tool's --motion-dir format) and NNNNN-prov.raw (one byte a pixel, the arbiter's provenance:
// 0 nothing known, 2 camera only, 3 camera corrected by flow, 4 measured total), and flow.log.
//
// A measuring tool: it reads files and runs our GPU programs on this machine's card. It touches no
// game, install or setting.
#include "test_support.hpp"
#include "core/motion_arbiter_gpu.hpp"
#include "core/fold_shading.hpp"
#include "host_dx12/ffx_flow/ffx_flow.hpp"
#include "host_dx12/ffx_flow/ffx_flow_shaders.hpp"
#include "host_dx12/j2/compute_flow.hpp"
#include "host_dx12/nr/native_fg.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {
FILE* g_log = nullptr;
void Log(const char* fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    std::printf("%s\n", line);
    std::fflush(stdout);
    if (g_log) { std::fprintf(g_log, "%s\n", line); std::fflush(g_log); }
}
bool ReadBytes(const std::string& path, std::vector<uint8_t>* out)
{
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "rb") != 0 || !fp) return false;
    out->clear();
    uint8_t buffer[65536];
    size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof buffer, fp)) > 0) out->insert(out->end(), buffer, buffer + n);
    std::fclose(fp);
    return true;
}
bool WriteBytes(const std::string& path, const std::vector<uint8_t>& bytes)
{
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "wb") != 0 || !fp) return false;
    const size_t put = std::fwrite(bytes.data(), 1, bytes.size(), fp);
    std::fclose(fp);
    return put == bytes.size();
}
// "key": [a, b] in the capture's flat metadata.
bool Two(const std::string& s, const char* key, float* a, float* b)
{
    const std::string quoted = std::string("\"") + key + "\"";
    size_t p = s.find(quoted);
    if (p == std::string::npos) return false;
    p = s.find('[', p);
    if (p == std::string::npos) return false;
    ++p;
    char* end = nullptr;
    const double x = std::strtod(s.c_str() + p, &end);
    if (end == s.c_str() + p) return false;
    const char* q = end;
    while (*q == ' ' || *q == ',') ++q;
    char* end2 = nullptr;
    const double y = std::strtod(q, &end2);
    if (end2 == q) return false;
    *a = static_cast<float>(x);
    *b = static_cast<float>(y);
    return true;
}
std::string FrameFile(const std::string& folder, uint32_t index, const char* what)
{
    char name[64];
    snprintf(name, sizeof name, "\\%05u-%s", index, what);
    return folder + name;
}

struct Input {
    test::ComPtr<ID3D12Resource> tex, upload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    uint32_t bpp = 0;
    bool filled = false;
};
bool MakeInput(ID3D12Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT format, uint32_t bpp, Input* t)
{
    t->bpp = bpp;
    t->tex = test::CreateTexture(dev, w, h, format, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    if (!t->tex) return false;
    const D3D12_RESOURCE_DESC desc = t->tex->GetDesc();
    UINT64 bytes = 0;
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &t->fp, nullptr, nullptr, &bytes);
    t->upload = test::CreateBuffer(dev, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    return t->upload != nullptr;
}
bool Fill(ID3D12GraphicsCommandList* list, Input* t, const std::vector<uint8_t>& rows,
          uint32_t source_w, uint32_t source_h, na::fold::Block block)
{
    constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const size_t tight = static_cast<size_t>(source_w) * t->bpp;
    const size_t height = source_h;
    const size_t padded = (tight + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~size_t(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    size_t source_pitch = tight;
    if (rows.size() != tight * height) {
        // A live capture stores the D3D footprint. Its final row can omit trailing alignment.
        if (rows.size() != padded * height && rows.size() != padded * (height - 1) + tight) return false;
        source_pitch = padded;
    }
    uint8_t* p = nullptr;
    if (FAILED(t->upload->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return false;
    for (UINT y = 0; y < t->fp.Footprint.Height; ++y) {
        auto* dst = p + static_cast<size_t>(y) * t->fp.Footprint.RowPitch;
        const auto* src = rows.data() + (static_cast<size_t>(y) * block.y + block.y - 1) * source_pitch;
        if (block.x == 1) std::memcpy(dst, src, tight);
        else for (UINT x = 0; x < t->fp.Footprint.Width; ++x)
            std::memcpy(dst + size_t(x) * t->bpp, src + (size_t(x) * block.x + block.x - 1) * t->bpp, t->bpp);
    }
    t->upload->Unmap(0, nullptr);
    if (t->filled) {
        const D3D12_RESOURCE_BARRIER b = test::Transition(t->tex.Get(), read, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = t->upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = t->fp;
    dst.pResource = t->tex.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const D3D12_RESOURCE_BARRIER b = test::Transition(t->tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read);
    list->ResourceBarrier(1, &b);
    t->filled = true;
    return true;
}

struct Result {
    test::ComPtr<ID3D12Resource> tex, readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 bytes = 0;
    uint32_t bpp = 0;
};
bool MakeResult(ID3D12Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT format, uint32_t bpp, Result* r)
{
    r->bpp = bpp;
    r->tex = test::CreateTexture(dev, w, h, format, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!r->tex) return false;
    const D3D12_RESOURCE_DESC desc = r->tex->GetDesc();
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &r->fp, nullptr, nullptr, &r->bytes);
    r->readback = test::CreateBuffer(dev, r->bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    return r->readback != nullptr;
}
void CopyOut(ID3D12GraphicsCommandList* list, Result* r)
{
    constexpr auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_BARRIER b = test::Transition(r->tex.Get(), uav, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = r->tex.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = r->readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = r->fp;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b = test::Transition(r->tex.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, uav);
    list->ResourceBarrier(1, &b);
}
bool ReadOut(Result* r, std::vector<uint8_t>* rows)
{
    const size_t tight = static_cast<size_t>(r->fp.Footprint.Width) * r->bpp;
    rows->assign(tight * r->fp.Footprint.Height, 0);
    uint8_t* p = nullptr;
    const D3D12_RANGE whole{0, static_cast<SIZE_T>(r->bytes)};
    if (FAILED(r->readback->Map(0, &whole, reinterpret_cast<void**>(&p)))) return false;
    for (UINT y = 0; y < r->fp.Footprint.Height; ++y)
        std::memcpy(rows->data() + tight * y, p + static_cast<size_t>(y) * r->fp.Footprint.RowPitch, tight);
    const D3D12_RANGE none{0, 0};
    r->readback->Unmap(0, &none);
    return true;
}

int Usage()
{
    std::printf("flow_motion_dump <capture folder> <out folder> --mode none|camera|total [--loosen 0..100] [--frames N] [--fold-source quality|performance | --fold-input quality|performance | --sample quality|performance] [--compare DIR]\n");
    return 2;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) return Usage();
    const std::string folder = argv[1], out_dir = argv[2];
    std::string mode = "total";
    na::fold::Block source_block{};
    na::fold::Block input_block{};
    uint32_t loosen = 0, max_frames = 0;
    na::fold::Block sample_block{};
    std::string compare_dir;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (a == "--mode" && value) { mode = value; ++i; }
        else if ((a == "--fold-source" || a == "--fold-input") && value) {
            const std::string level(value);
            if (source_block.x != 1 || input_block.x != 1 || (level != "quality" && level != "performance")) return Usage();
            auto& block = a == "--fold-source" ? source_block : input_block;
            block = na::fold::BlockFor(level == "quality" ? na::fold::Mode::Quality : na::fold::Mode::Performance);
            ++i;
        }
        else if (a == "--sample" && value) {
            const std::string level(value);
            if (level != "quality" && level != "performance") return Usage();
            sample_block = na::fold::BlockFor(level == "quality" ? na::fold::Mode::Quality : na::fold::Mode::Performance);
            ++i;
        }
        else if (a == "--compare" && value) { compare_dir = value; ++i; }
        else if (a == "--loosen" && value) { loosen = static_cast<uint32_t>(std::strtoul(value, nullptr, 10)); ++i; }
        else if (a == "--frames" && value) { max_frames = static_cast<uint32_t>(std::strtoul(value, nullptr, 10)); ++i; }
        else return Usage();
    }
    const bool total = mode == "total", camera = mode == "camera";
    if (!total && !camera && mode != "none") return Usage();
    if (!total && input_block.x != 1) return Usage();
    const bool sampled = sample_block.x != 1 || sample_block.y != 1;
    if (sampled && (!total || input_block.x != 1 || input_block.y != 1)) return Usage();
    CreateDirectoryA(out_dir.c_str(), nullptr);
    fopen_s(&g_log, (out_dir + "\\flow.log").c_str(), "wb");
    Log("flow_motion_dump: folder=%s mode=%s loosen=%u", folder.c_str(), mode.c_str(), loosen);

    std::vector<uint8_t> bytes;
    if (!ReadBytes(FrameFile(folder, 0, "meta.json"), &bytes)) { Log("FATAL: no 00000-meta.json in %s", folder.c_str()); return 1; }
    std::string meta(bytes.begin(), bytes.end());
    float fw = 0, fh = 0, shear_x = 0, shear_y = 0;
    if (!Two(meta, "size", &fw, &fh) || fw < 16 || fh < 16) { Log("FATAL: the metadata gives no size"); return 1; }
    const uint32_t source_w = static_cast<uint32_t>(fw + 0.5f), source_h = static_cast<uint32_t>(fh + 0.5f);
    const uint32_t w = na::fold::Folded(source_w, input_block.x), h = na::fold::Folded(source_h, input_block.y);
    // The output's size: the flow's own, or with --sample the kept pixels' grid.
    const uint32_t ow = sampled ? na::fold::Folded(w, sample_block.x) : w, oh = sampled ? na::fold::Folded(h, sample_block.y) : h;
    const na::fold::Block kept_block = sampled ? sample_block : input_block; // the block each output vector stands for
    if (sampled) Log("sampled: the search runs on %u x %u; `measure` and the arbiter decide the last pixel of each %u x %u block: output %u x %u",
                     w, h, sample_block.x, sample_block.y, ow, oh);
    const bool has_shear = Two(meta, "trial_shear_px", &shear_x, &shear_y);
    if (camera && !has_shear) { Log("FATAL: --mode camera needs \"trial_shear_px\" in the metadata"); return 1; }
    Log("size %u x %u, camera shear %g, %g pixels per unit of depth%s", w, h, static_cast<double>(shear_x), static_cast<double>(shear_y),
        camera ? "" : " (not used in this mode)");

    if (!na::ffx_flow::Built()) { Log("FATAL: the AMD flow programs are not part of this build"); return 1; }
    test::Device d;
    if (!test::CreateDevice(d, false)) { Log("FATAL: no hardware D3D12 device"); return test::kSkip; }
    if (!na::ffx_flow::DeviceSupported(d.device.Get())) { Log("FATAL: the AMD search needs 32/64-lane hardware waves"); return test::kSkip; }
    ID3D12Device* dev = d.device.Get();

    Input color, depth;
    REQUIRE(MakeInput(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, &color));
    REQUIRE(MakeInput(dev, w, h, DXGI_FORMAT_R32_FLOAT, 4, &depth));
    Result motion, prov;
    REQUIRE(MakeResult(dev, ow, oh, DXGI_FORMAT_R32G32_FLOAT, 8, &motion));
    REQUIRE(MakeResult(dev, ow, oh, DXGI_FORMAT_R8_UINT, 1, &prov));
    uint64_t compared = 0, identical = 0, compare_frames = 0;
    double compare_max = 0;
    na::motion::gpu::Arbiter arbiter;
    REQUIRE(arbiter.Init(dev));
    test::List l;
    REQUIRE(l.Create(dev));
    test::ComPtr<ID3D12QueryHeap> queries;
    test::ComPtr<ID3D12Resource> timing = test::CreateBuffer(dev, 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 frequency = 0;
    {
        const D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
        REQUIRE(timing && SUCCEEDED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&queries))) && SUCCEEDED(d.queue->GetTimestampFrequency(&frequency)) && frequency);
    }
    std::vector<double> stage_ms;

    namespace cf = na::j2::compute_flow;
    cf::SetLoosen(loosen);
    {
        const auto g = cf::GatesFor(loosen);
        Log("acceptance rules at %u: image ratio %.2f, image error %.2f, contrast needed %.3f, agreement %.1f px / cos %.2f, "
            "earlier agreeing pairs needed %u, neighbour spread %.1f px, sky %d",
            loosen, static_cast<double>(g.image_ratio), static_cast<double>(g.image_abs), static_cast<double>(g.texture_min),
            static_cast<double>(g.agree_px), static_cast<double>(g.agree_cos), g.agree_min, static_cast<double>(g.consistency_px), g.sky ? 1 : 0);
    }

    std::vector<uint8_t> rows, tags;
    uint64_t tag_count[8]{};
    std::vector<double> pair_ms;
    uint32_t written = 0;
    const uint64_t first_frame = 100;
    for (uint32_t index = 0; !max_frames || index < max_frames; ++index) {
        if (!ReadBytes(FrameFile(folder, index, "meta.json"), &bytes)) break;
        meta.assign(bytes.begin(), bytes.end());
        float jx = 0, jy = 0;
        REQUIRE(Two(meta, "jitter_px", &jx, &jy));
        REQUIRE(ReadBytes(FrameFile(folder, index, "color.raw"), &bytes));
        REQUIRE(Fill(l.list.Get(), &color, bytes, source_w, source_h, input_block));
        REQUIRE(ReadBytes(FrameFile(folder, index, "depth.raw"), &bytes));
        REQUIRE(Fill(l.list.Get(), &depth, bytes, source_w, source_h, input_block));
        jx = na::fold::DeclaredJitter(input_block.x, input_block.x - 1, jx);
        jy = na::fold::DeclaredJitter(input_block.y, input_block.y - 1, jy);
        l.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

        // The scene's sample sits at pixel centre - jitter: the raster's own sample, known exactly.
        cf::Frame f;
        f.frame = first_frame + index;
        f.previous = index ? f.frame - 1 : 0;
        f.generation = 1;
        f.source_generation = 1;
        f.hdr = true;
        f.block_x = static_cast<uint8_t>(source_block.x);
        f.block_y = static_cast<uint8_t>(source_block.y);
        f.jitter[0] = f.raster_jitter[0] = jx;
        f.jitter[1] = f.raster_jitter[1] = jy;
        f.raster_sample = f.raster_confirmed = true;
        f.residual = true;
        f.total = total;
        f.last_reset_frame = first_frame;
        if (sampled) {
            const auto pick = na::fold::Pick(sample_block.y == 2 ? na::fold::Mode::Performance : na::fold::Mode::Quality);
            f.sample_x = static_cast<uint8_t>(sample_block.x);
            f.sample_y = static_cast<uint8_t>(sample_block.y);
            f.sample_pick_x = static_cast<uint8_t>(pick.x);
            f.sample_pick_y = static_cast<uint8_t>(pick.y);
        }

        na::motion::gpu::Inputs in{};
        in.depth = depth.tex.Get();
        in.depth_clear = 0.0f; // reversed depth: 0 is "nothing drawn"; the scene draws every pixel
        if (total) {
            const float nan = std::numeric_limits<float>::quiet_NaN();
            for (int j = 0; j < 16; ++j) in.inv_view_proj_current.m[j] = in.view_proj_previous.m[j] = nan;
            in.jitter_current_px[0] = in.jitter_current_px[1] = nan;
            in.tier1_source = na::motion::Tier1Source::Estimated;
        } else {
            // Row vectors, p' = p * M. Unprojection is the identity; the previous camera shifts a point
            // by its depth times the shear (previous minus current, pixels): x' = x + z * 2 sx / W,
            // y' = y - z * 2 sy / H in clip space. With --mode none the shear is zero: nothing moved.
            for (int j = 0; j < 16; ++j) in.inv_view_proj_current.m[j] = in.view_proj_previous.m[j] = (j % 5 == 0) ? 1.0f : 0.0f;
            if (camera) {
                in.view_proj_previous.m[8] = 2.0f * shear_x / static_cast<float>(w);
                in.view_proj_previous.m[9] = -2.0f * shear_y / static_cast<float>(h);
            }
            in.jitter_current_px[0] = jx;
            in.jitter_current_px[1] = jy;
            in.tier2_valid_flag = true;
        }

        cf::Tier3 tier;
        const cf::Ticket ticket = cf::Record(l.list.Get(), d.queue.Get(), color.tex.Get(), depth.tex.Get(), in, f, tier);
        if (!ticket.context) { Log("FATAL: the flow refused frame %u: %s", index, cf::GetStats().reason); return 1; }
        bool dispatched = false;
        if (total) {
            if (ticket.pair && tier.total && tier.total_valid) {
                in.tier1_motion = tier.total;
                in.tier1_valid = tier.total_valid;
                in.tier1_valid_flag = true;
                dispatched = true;
            }
        } else {
            if (ticket.pair && tier.residual) {
                in.tier3_valid_flag = true;
                in.tier3_residual = tier.residual;
                in.tier3_valid = tier.valid;
                in.tier3_mask = tier.mask;
                in.fix = tier.fix;
            }
            dispatched = true;
        }
        if (dispatched) {
            na::motion::gpu::Outputs o;
            o.xy = motion.tex.Get();
            o.provenance = prov.tex.Get();
            o.width = ow;
            o.height = oh;
            if (sampled) {
                o.block_x = sample_block.x;
                o.block_y = sample_block.y;
                o.pick_x = f.sample_pick_x;
                o.pick_y = f.sample_pick_y;
                o.source_width = w;
                o.source_height = h;
            }
            REQUIRE(arbiter.Dispatch(l.list.Get(), in, o, 0));
        }
        l.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        l.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, timing.Get(), 0);
        if (dispatched) {
            CopyOut(l.list.Get(), &motion);
            CopyOut(l.list.Get(), &prov);
        }
        REQUIRE(SUCCEEDED(l.list->Close()));
        {
            ID3D12CommandList* lists[] = {l.list.Get()};
            na::nr::native::Submission track(d.queue.Get(), 1, lists);
            d.queue->ExecuteCommandLists(1, lists);
        }
        d.WaitIdle();
        cf::Committed(ticket);
        if (dev->GetDeviceRemovedReason() != S_OK) { Log("FATAL: the device was removed at frame %u", index); return 1; }
        {
            UINT64* t = nullptr;
            const D3D12_RANGE range{0, 16}, none{};
            REQUIRE(SUCCEEDED(timing->Map(0, &range, reinterpret_cast<void**>(&t))));
            const double ms = static_cast<double>(t[1] - t[0]) * 1000.0 / static_cast<double>(frequency);
            timing->Unmap(0, &none);
            Log("time: frame %u motion stage %.3f ms at %u x %u%s", index, ms, w, h, ticket.pair ? "" : " (no pair yet: not counted)");
            if (ticket.pair) stage_ms.push_back(ms);
        }
        na::nr::native::Reset(l.list.Get());
        na::nr::native::Maintain();
        if (index >= 5 && ticket.pair) {
            const auto pair_stats = cf::GetStats();
            if (pair_stats.sample_frame == f.frame && pair_stats.ms > 0) pair_ms.push_back(pair_stats.ms);
        }
        l.Reset();

        if (dispatched) {
            REQUIRE(ReadOut(&motion, &rows) && ReadOut(&prov, &tags));
        } else {
            rows.assign(static_cast<size_t>(ow) * oh * 8, 0); // no pair yet: nothing known
            tags.assign(static_cast<size_t>(ow) * oh, 0);
        }
        if (!compare_dir.empty() && dispatched && (kept_block.x != 1 || kept_block.y != 1)) {
            // The whole-picture run's vectors at the kept pixels (the last of each block) against this run's.
            std::vector<uint8_t> other;
            if (ReadBytes(FrameFile(compare_dir, index, "motion.raw"), &other) && other.size() == static_cast<size_t>(source_w) * source_h * 8) {
                uint64_t same = 0, n = 0;
                double worst = 0;
                for (uint32_t y = 0; y < oh; ++y)
                    for (uint32_t x = 0; x < ow; ++x) {
                        const size_t sx = static_cast<size_t>(x) * kept_block.x + kept_block.x - 1, sy = static_cast<size_t>(y) * kept_block.y + kept_block.y - 1;
                        const uint8_t* a = rows.data() + (static_cast<size_t>(y) * ow + x) * 8;
                        const uint8_t* b = other.data() + (sy * source_w + sx) * 8;
                        ++n;
                        if (sampled && std::memcmp(a, b, 8) == 0) { ++same; continue; }
                        float va[2], vb[2];
                        std::memcpy(va, a, 8);
                        std::memcpy(vb, b, 8);
                        va[0] *= static_cast<float>(input_block.x); // a folded run's vectors are folded pixels
                        va[1] *= static_cast<float>(input_block.y);
                        if (va[0] == vb[0] && va[1] == vb[1]) { ++same; continue; }
                        const double dx = static_cast<double>(va[0]) - vb[0], dy = static_cast<double>(va[1]) - vb[1];
                        const double dist = std::sqrt(dx * dx + dy * dy);
                        worst = std::isfinite(dist) ? (std::max)(worst, dist) : 1e30;
                    }
                compared += n; identical += same; ++compare_frames;
                compare_max = (std::max)(compare_max, worst);
                Log("compare: frame %u: %llu of %llu kept vectors bit-identical to the whole-picture run, largest difference %.4f px", index,
                    static_cast<unsigned long long>(same), static_cast<unsigned long long>(n), worst);
            } else {
                Log("compare: frame %u: no whole-picture motion plane of %u x %u in %s", index, source_w, source_h, compare_dir.c_str());
            }
        }
        uint64_t count[8]{};
        for (const uint8_t t : tags) ++count[t & 7u];
        for (int j = 0; j < 8; ++j) tag_count[j] += count[j];
        const double all = static_cast<double>(tags.size());
        Log("frame %u: pair=%d  nothing known %.1f%%  camera only %.1f%%  camera corrected by flow %.1f%%  measured total %.1f%%", index,
            ticket.pair ? 1 : 0, 100.0 * static_cast<double>(count[0]) / all, 100.0 * static_cast<double>(count[2]) / all,
            100.0 * static_cast<double>(count[3]) / all, 100.0 * static_cast<double>(count[4]) / all);
        if (!WriteBytes(FrameFile(out_dir, index, "motion.raw"), rows) || !WriteBytes(FrameFile(out_dir, index, "prov.raw"), tags)) {
            Log("FATAL: cannot write frame %u (disk full?)", index);
            return 1;
        }
        ++written;
    }
    if (!written) { Log("FATAL: no frame read from %s", folder.c_str()); return 1; }
    if (compare_frames)
        Log("compare summary: %llu frames, %llu of %llu kept vectors bit-identical (%.4f%%), largest difference %.4f px",
            static_cast<unsigned long long>(compare_frames), static_cast<unsigned long long>(identical), static_cast<unsigned long long>(compared),
            compared ? 100.0 * static_cast<double>(identical) / static_cast<double>(compared) : 0.0, compare_max);
    if (!pair_ms.empty()) {
        std::sort(pair_ms.begin(), pair_ms.end());
        const size_t n = pair_ms.size();
        Log("motion GPU timestamp: %zu pairs after warm-up, median %.4f ms, p95 %.4f ms", n,
            (pair_ms[(n - 1) / 2] + pair_ms[n / 2]) * .5, pair_ms[n - 1 - n / 20]);
    }
    char text[1024]{};
    const cf::Stats st = cf::GetStats();
    cf::DescribeStats(st, text, sizeof text);
    Log("flow counters: %s", text);
    if (!stage_ms.empty()) {
        std::sort(stage_ms.begin(), stage_ms.end());
        double mean = 0;
        for (const double v : stage_ms) mean += v;
        Log("time summary: motion stage (flow and arbiter) at %u x %u over %zu pairs: median %.3f ms, mean %.3f, fastest %.3f, slowest %.3f", w, h,
            stage_ms.size(), stage_ms[stage_ms.size() / 2], mean / static_cast<double>(stage_ms.size()), stage_ms.front(), stage_ms.back());
    }
    double all = 0;
    for (int j = 0; j < 8; ++j) all += static_cast<double>(tag_count[j]);
    Log("done: %u frames written to %s; over all frames nothing known %.1f%%, camera only %.1f%%, camera corrected by flow %.1f%%, measured total %.1f%%",
        written, out_dir.c_str(), 100.0 * static_cast<double>(tag_count[0]) / all, 100.0 * static_cast<double>(tag_count[2]) / all,
        100.0 * static_cast<double>(tag_count[3]) / all, 100.0 * static_cast<double>(tag_count[4]) / all);
    d.WaitIdle();
    cf::Reset();
    if (g_log) std::fclose(g_log);
    g_log = nullptr;
    return 0;
}
