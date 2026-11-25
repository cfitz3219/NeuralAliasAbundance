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
//   flow_motion_dump <capture folder> <out folder> --mode none|camera|total [--loosen 0..100] [--frames N] [--fold 2x2|2x1]
//
//   --fold 2x2 | 2x1  run on the folded picture (one pixel of each block kept, the last one, as the live
//                   build keeps it) instead of the full-size one: what the motion stage costs and measures
//                   when it runs at the size DLSS gets in Performance (2x2) or Quality (2x1).
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
#include "host_dx12/ffx_flow/ffx_flow.hpp"
#include "host_dx12/ffx_flow/ffx_flow_shaders.hpp"
#include "host_dx12/j2/compute_flow.hpp"
#include "host_dx12/nr/native_fg.hpp"

#include <algorithm>
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
// A saved plane as tight rows of the folded size: rows may be padded (a game capture pads them to 256
// bytes and stores the last row without padding); of each bx x by block the last pixel is kept.
bool Gather(const std::vector<uint8_t>& in, uint32_t w, uint32_t h, uint32_t bpp, uint32_t bx, uint32_t by, std::vector<uint8_t>* out)
{
    const size_t tight = static_cast<size_t>(w) * bpp;
    size_t pitch = tight;
    if (in.size() != tight * h) {
        if (h < 2 || in.size() < tight || (in.size() - tight) % (h - 1)) return false;
        pitch = (in.size() - tight) / (h - 1);
        if (pitch < tight) return false;
    }
    const uint32_t fw = w / bx, fh = h / by;
    out->resize(static_cast<size_t>(fw) * fh * bpp);
    for (uint32_t y = 0; y < fh; ++y) {
        const uint8_t* src = in.data() + static_cast<size_t>(by * y + by - 1) * pitch + static_cast<size_t>(bx - 1) * bpp;
        uint8_t* dst = out->data() + static_cast<size_t>(y) * fw * bpp;
        for (uint32_t x = 0; x < fw; ++x) std::memcpy(dst + static_cast<size_t>(x) * bpp, src + static_cast<size_t>(bx) * x * bpp, bpp);
    }
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
bool Fill(ID3D12GraphicsCommandList* list, Input* t, const std::vector<uint8_t>& rows)
{
    constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const size_t tight = static_cast<size_t>(t->fp.Footprint.Width) * t->bpp;
    if (rows.size() != tight * t->fp.Footprint.Height) return false;
    uint8_t* p = nullptr;
    if (FAILED(t->upload->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return false;
    for (UINT y = 0; y < t->fp.Footprint.Height; ++y)
        std::memcpy(p + static_cast<size_t>(y) * t->fp.Footprint.RowPitch, rows.data() + tight * y, tight);
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
    std::printf("flow_motion_dump <capture folder> <out folder> --mode none|camera|total [--loosen 0..100] [--frames N] [--fold 2x2|2x1]\n");
    return 2;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) return Usage();
    const std::string folder = argv[1], out_dir = argv[2];
    std::string mode = "total";
    uint32_t loosen = 0, max_frames = 0, bx = 1, by = 1;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (a == "--mode" && value) { mode = value; ++i; }
        else if (a == "--loosen" && value) { loosen = static_cast<uint32_t>(std::strtoul(value, nullptr, 10)); ++i; }
        else if (a == "--frames" && value) { max_frames = static_cast<uint32_t>(std::strtoul(value, nullptr, 10)); ++i; }
        else if (a == "--fold" && value) {
            const std::string v = value;
            ++i;
            if (v == "2x2") bx = by = 2;
            else if (v == "2x1") { bx = 2; by = 1; }
            else return Usage();
        }
        else return Usage();
    }
    const bool total = mode == "total", camera = mode == "camera";
    if (!total && !camera && mode != "none") return Usage();
    CreateDirectoryA(out_dir.c_str(), nullptr);
    fopen_s(&g_log, (out_dir + "\\flow.log").c_str(), "wb");
    Log("flow_motion_dump: folder=%s mode=%s loosen=%u", folder.c_str(), mode.c_str(), loosen);

    std::vector<uint8_t> bytes;
    if (!ReadBytes(FrameFile(folder, 0, "meta.json"), &bytes)) { Log("FATAL: no 00000-meta.json in %s", folder.c_str()); return 1; }
    std::string meta(bytes.begin(), bytes.end());
    float fw = 0, fh = 0, shear_x = 0, shear_y = 0;
    if (!Two(meta, "size", &fw, &fh) || fw < 16 || fh < 16) { Log("FATAL: the metadata gives no size"); return 1; }
    const uint32_t full_w = static_cast<uint32_t>(fw + 0.5f), full_h = static_cast<uint32_t>(fh + 0.5f);
    const uint32_t w = full_w / bx, h = full_h / by; // the size the flow runs at
    if (bx != 1 || by != 1) Log("fold %ux%u: the flow runs on %u x %u, one pixel of each block of the %u x %u picture (the last)", bx, by, w, h, full_w, full_h);
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
    REQUIRE(MakeResult(dev, w, h, DXGI_FORMAT_R32G32_FLOAT, 8, &motion));
    REQUIRE(MakeResult(dev, w, h, DXGI_FORMAT_R8_UINT, 1, &prov));
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
    std::vector<uint8_t> gathered;

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
    uint32_t written = 0;
    const uint64_t first_frame = 100;
    for (uint32_t index = 0; !max_frames || index < max_frames; ++index) {
        if (!ReadBytes(FrameFile(folder, index, "meta.json"), &bytes)) break;
        meta.assign(bytes.begin(), bytes.end());
        float jx = 0, jy = 0;
        REQUIRE(Two(meta, "jitter_px", &jx, &jy));
        // The kept pixel (the last of its block) sits (b/2 - (b-1) - 0.5) full-size pixels before the
        // block's centre: its jitter in folded pixels (core/fold_shading.hpp DeclaredJitter).
        jx = (static_cast<float>(bx) * 0.5f - static_cast<float>(bx - 1) - 0.5f + jx) / static_cast<float>(bx);
        jy = (static_cast<float>(by) * 0.5f - static_cast<float>(by - 1) - 0.5f + jy) / static_cast<float>(by);
        REQUIRE(ReadBytes(FrameFile(folder, index, "color.raw"), &bytes));
        REQUIRE(Gather(bytes, full_w, full_h, 8, bx, by, &gathered));
        REQUIRE(Fill(l.list.Get(), &color, gathered));
        REQUIRE(ReadBytes(FrameFile(folder, index, "depth.raw"), &bytes));
        REQUIRE(Gather(bytes, full_w, full_h, 4, bx, by, &gathered));
        REQUIRE(Fill(l.list.Get(), &depth, gathered));
        l.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

        // The scene's sample sits at pixel centre - jitter: the raster's own sample, known exactly.
        cf::Frame f;
        f.frame = first_frame + index;
        f.previous = index ? f.frame - 1 : 0;
        f.generation = 1;
        f.source_generation = 1;
        f.hdr = true;
        f.jitter[0] = f.raster_jitter[0] = jx;
        f.jitter[1] = f.raster_jitter[1] = jy;
        f.raster_sample = f.raster_confirmed = true;
        f.residual = true;
        f.total = total;
        f.last_reset_frame = first_frame;

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
            o.width = w;
            o.height = h;
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
        l.Reset();

        if (dispatched) {
            REQUIRE(ReadOut(&motion, &rows) && ReadOut(&prov, &tags));
        } else {
            rows.assign(static_cast<size_t>(w) * h * 8, 0); // no pair yet: nothing known
            tags.assign(static_cast<size_t>(w) * h, 0);
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
