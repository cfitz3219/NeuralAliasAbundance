// M3 GPU camera fit (camera_fit_gpu.cpp) end to end on a D3D12 device, 2026-11-04.
// Synthetic SE5 pattern: the tracked previous VP lacks the camera translation (the player walks,
// C = identity-like), the flow is the true camera motion. Checks:
//  - the GPU fit is accepted and recovers the eye step;
//  - the arbiter's tier 2 with the fix reproduces the true motion (< 0.1 px mean), and without
//    it the motion is the old ~0 (the bug), so the fix is what changes it;
//  - a still scene is not accepted and the arbiter output is unchanged by the (invalid) buffer;
//  - 25 % corrupted flow (HUD, movers) does not stop the fit.
#include "test_support.hpp"
#include "core/camera_fit_gpu.hpp"
#include "core/motion_arbiter_gpu.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using M = std::array<double, 16>;
namespace {
constexpr unsigned W = 640, H = 360;
constexpr auto S = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, U = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
M mul(const M& a, const M& b) {
    M r{};
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) for (int k = 0; k < 4; ++k) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
    return r;
}
M inverse(const M& m) {
    double a[4][8];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1 : 0);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[p][j]);
        const double d = a[c][c];
        for (int j = 0; j < 8; ++j) a[c][j] /= d;
        for (int r = 0; r < 4; ++r) if (r != c) { const double f = a[r][c]; for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j]; }
    }
    M r{};
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) r[i * 4 + j] = a[i][j + 4];
    return r;
}
M view(const double eye[3], double yaw) {
    const double c = std::cos(yaw), s = std::sin(yaw);
    const M r{c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1};
    const M t{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -eye[0], -eye[1], -eye[2], 1};
    return mul(t, r);
}
M proj() {
    const double n = 0.1, f = 1000, ys = 1.0 / std::tan(0.6), xs = ys / (16.0 / 9.0);
    return M{xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, f / (f - n), 1, 0, 0, -n * f / (f - n), 0};
}
std::array<double, 4> rowmul(const std::array<double, 4>& p, const M& m) {
    std::array<double, 4> r{};
    for (int c = 0; c < 4; ++c) for (int k = 0; k < 4; ++k) r[c] += p[k] * m[k * 4 + c];
    return r;
}
uint32_t rng = 99;
double uniform() { rng = rng * 1664525u + 1013904223u; return double(rng >> 8) / double(1u << 24); }

struct Scene { std::vector<float> depth, flow, valid; };
// A ground plane below the eye and a wall of boxes ahead: smooth depth, so 8x8 blocks are coherent.
Scene make(const M& P, const M& vp_cur, const M& vp_true_prev, const float jit[2], double corrupt) {
    Scene s;
    s.depth.resize(W * H); s.flow.resize(W * H * 2); s.valid.resize(W * H);
    const M inv = inverse(vp_cur);
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x) {
            const double px = x + 0.5 - jit[0], py = y + 0.5 - jit[1];
            const double nx = px / W * 2 - 1, ny = 1 - py / H * 2;
            const double z = 4 + 30 * (0.5 + 0.5 * std::sin(x * 0.02) * std::cos(y * 0.03)) + 6 * (1 - double(y) / H);
            const double d = (P[10] * z + P[14]) / z;
            auto w = rowmul({nx, ny, d, 1}, inv);
            for (auto& v : w) v /= w[3];
            const auto pc = rowmul(w, vp_true_prev);
            const double ppx = (pc[0] / pc[3] + 1) * 0.5 * W, ppy = (1 - pc[1] / pc[3]) * 0.5 * H;
            const size_t i = size_t(y) * W + x;
            s.depth[i] = float(d);
            s.flow[i * 2] = float(ppx - px);
            s.flow[i * 2 + 1] = float(ppy - py);
            s.valid[i] = 1.f;
            // Corrupt whole 8x8 blocks (a HUD panel / a mover), not single pixels.
            if (corrupt > 0) {
                uint32_t h = (x / 8) * 73856093u ^ (y / 8) * 19349663u;
                h ^= h >> 13; h *= 2246822519u; h ^= h >> 16;
                if (double(h % 1000) / 1000.0 < corrupt) { s.flow[i * 2] += 9.f; s.flow[i * 2 + 1] -= 6.f; }
            }
        }
    return s;
}
na::motion::Mat4 to_mat4(const M& m) { na::motion::Mat4 r{}; for (int i = 0; i < 16; ++i) r.m[i] = float(m[i]); return r; }
} // namespace

int main() {
    test::Device d;
    if (!test::CreateDevice(d)) return test::kSkip;
    test::List l;
    REQUIRE(l.Create(d.device.Get()));
    std::vector<test::ComPtr<ID3D12Resource>> keep;
    auto upload = [&](const void* data, unsigned bpp, DXGI_FORMAT fmt) {
        auto r = test::CreateTexture(d.device.Get(), W, H, fmt, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
        auto up = test::CreateBuffer(d.device.Get(), W * H * bpp, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p = nullptr;
        up->Map(0, nullptr, &p);
        std::memcpy(p, data, size_t(W) * H * bpp);
        up->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = up.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint.Footprint = {fmt, W, H, 1, W * bpp};
        dst.pResource = r.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        auto tr = test::Transition(r.Get(), D3D12_RESOURCE_STATE_COPY_DEST, S);
        l.list->ResourceBarrier(1, &tr);
        keep.push_back(up);
        return r;
    };
    auto execute = [&]() {
        l.list->Close();
        ID3D12CommandList* lists[] = {l.list.Get()};
        d.queue->ExecuteCommandLists(1, lists);
        d.WaitIdle();
        l.Reset();
    };
    na::motion::gpu::CameraFit fit;
    REQUIRE(fit.Init(d.device.Get()));
    na::motion::gpu::Arbiter arbiter;
    REQUIRE(arbiter.Init(d.device.Get()));
    auto output = test::CreateTexture(d.device.Get(), W, H, DXGI_FORMAT_R32G32_FLOAT, U, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto prov = test::CreateTexture(d.device.Get(), W, H, DXGI_FORMAT_R8_UINT, U, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto rb = test::CreateBuffer(d.device.Get(), W * H * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    struct Result { bool collected = false; na::motion::gpu::FitResult fit; double err_fixed = 1e9, err_plain = 0; };
    // One case: fit, then arbiter tier 2 with and without the fix; mean error vs the true motion.
    auto run = [&](const double eye_prev[3], const double eye_cur[3], double corrupt) {
        const M P = proj();
        const M vp_cur = mul(view(eye_cur, 0.15), P), vp_true_prev = mul(view(eye_prev, 0.15), P);
        const M vp_tracked_prev = mul(view(eye_cur, 0.15), P); // translation missing
        const M inv_cur = inverse(vp_cur), composed = mul(inv_cur, vp_tracked_prev);
        const float jit[2] = {0.25f, -0.4f};
        const Scene clean = make(P, vp_cur, vp_true_prev, jit, 0), noisy = make(P, vp_cur, vp_true_prev, jit, corrupt);
        auto depth = upload(noisy.depth.data(), 4, DXGI_FORMAT_R32_FLOAT);
        auto flow = upload(noisy.flow.data(), 8, DXGI_FORMAT_R32G32_FLOAT);
        auto valid = upload(noisy.valid.data(), 4, DXGI_FORMAT_R32_FLOAT);
        const auto basis = na::motion::gpu::MakeFitBasis(composed.data(), inv_cur.data(), vp_tracked_prev.data(), eye_cur);
        auto* fix = fit.Record(l.list.Get(), 0, depth.Get(), flow.Get(), valid.Get(), basis, jit, 1.f, W, H);
        if (!fix) { CHECK(fix); return Result{}; }
        double err[2] = {};
        for (int pass = 0; pass < 2; ++pass) {
            na::motion::gpu::Inputs in{};
            in.depth = depth.Get();
            in.tier2_valid_flag = true;
            in.depth_clear = 1.f;
            in.inv_view_proj_current = to_mat4(composed);
            M id{}; id[0] = id[5] = id[10] = id[15] = 1;
            in.view_proj_previous = to_mat4(id);
            in.jitter_current_px[0] = jit[0]; in.jitter_current_px[1] = jit[1];
            in.fix = pass == 0 ? fix : nullptr;
            na::motion::gpu::Outputs o{};
            o.xy = output.Get(); o.provenance = prov.Get(); o.width = W; o.height = H;
            if (!arbiter.Dispatch(l.list.Get(), in, o, 0)) { CHECK(!"arbiter dispatch"); return Result{}; }
            auto tr = test::Transition(output.Get(), U, D3D12_RESOURCE_STATE_COPY_SOURCE);
            l.list->ResourceBarrier(1, &tr);
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = output.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource = rb.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32_FLOAT, W, H, 1, W * 8};
            l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            tr = test::Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, U);
            l.list->ResourceBarrier(1, &tr);
            execute();
            float* p = nullptr;
            rb->Map(0, nullptr, reinterpret_cast<void**>(&p));
            double sum = 0;
            for (size_t i = 0; i < size_t(W) * H; ++i) sum += std::hypot(p[i * 2] - clean.flow[i * 2], p[i * 2 + 1] - clean.flow[i * 2 + 1]);
            D3D12_RANGE none{0, 0};
            rb->Unmap(0, &none);
            err[pass] = sum / double(W * H);
        }
        Result r{};
        r.collected = fit.Collect(0, &r.fit);
        r.err_fixed = err[0];
        r.err_plain = err[1];
        std::printf("fit accepted=%d e0=%.4f n0=%u e=%.4f n=%u t=[%.4f %.4f %.4f] r=[%.5f %.5f %.5f] error fixed=%.4f px plain=%.4f px\n",
                    r.fit.accepted ? 1 : 0, r.fit.energy0, r.fit.samples0, r.fit.energy, r.fit.samples, r.fit.theta[0], r.fit.theta[1],
                    r.fit.theta[2], r.fit.theta[3], r.fit.theta[4], r.fit.theta[5], r.err_fixed, r.err_plain);
        return r;
    };
    {
        const double a[3] = {0, 1.7, 0}, b[3] = {0.15, 1.7, 0.4};
        const auto r = run(a, b, 0);
        CHECK(r.collected && r.fit.accepted);
        CHECK(std::fabs(r.fit.theta[0] - 0.15f) < 0.02f && std::fabs(r.fit.theta[2] - 0.4f) < 0.04f);
        CHECK(r.err_plain > 1.0);   // the bug: no camera translation in the motion
        CHECK(r.err_fixed < 0.1);   // the fix restores it
    }
    {
        const double a[3] = {2, 1.7, 1}, b[3] = {2, 1.7, 1};
        const auto r = run(a, b, 0);
        CHECK(r.collected && !r.fit.accepted);
        CHECK(std::fabs(r.err_fixed - r.err_plain) < 1e-6); // an invalid buffer changes nothing
    }
    {
        const double a[3] = {0, 1.7, 0}, b[3] = {-0.1, 1.72, 0.3};
        const auto r = run(a, b, 0.25);
        CHECK(r.collected && r.fit.accepted);
        CHECK(r.err_fixed < 0.15);
    }
    CHECK(d.device->GetDeviceRemovedReason() == S_OK);
    return test::finish("camera_fit_gpu_test");
}
