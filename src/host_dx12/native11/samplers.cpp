// Native DX11 route: mip bias + anisotropic filtering on the game's samplers (see samplers.hpp).
#include "host_dx12/native11/samplers.hpp"
#include "host_dx12/native11/input_provider.hpp"

#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/native11/sampler_policy.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace na::native11 {
namespace {

using Microsoft::WRL::ComPtr;

enum : uint32_t { kCreateSamplerState = 23 };                 // ID3D11Device
enum : uint32_t { kPSSetSamplers = 10, kCSSetSamplers = 70 }; // ID3D11DeviceContext

using PFN_CreateSampler = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_SAMPLER_DESC*, ID3D11SamplerState**);
using PFN_SetSamplers = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);

PFN_CreateSampler g_create = nullptr;
PFN_SetSamplers g_ps[2] = {}, g_cs[2] = {}; // [0] immediate class, [1] deferred class

SamplerBiasFn g_bias_fn = nullptr;
SamplerAfFn g_af_fn = nullptr;
std::atomic<ID3D11Device*> g_device{nullptr};
std::atomic<bool> g_installed{false};
std::mutex g_install_mu;

// A game sampler: its description and the variants made for it, keyed by (bias, af).
struct Entry {
    D3D11_SAMPLER_DESC desc{};
    ComPtr<ID3D11SamplerState> original;
    bool wants_bias = false, wants_af = false;
    std::mutex mu;
    std::vector<std::pair<uint64_t, ComPtr<ID3D11SamplerState>>> variants; // never released while the game runs
};
// Keyed by the sampler object. D3D11 returns the same object for an identical description, and a
// reused address is re-recorded at the CreateSamplerState that returned it, so the key stays exact.
std::shared_mutex g_mu;
std::unordered_map<ID3D11SamplerState*, std::shared_ptr<Entry>> g_entries;

std::atomic<uint64_t> g_created{0}, g_elig_bias{0}, g_elig_af{0}, g_variants{0}, g_variant_failures{0};
std::atomic<uint64_t> g_binds{0}, g_swapped{0}, g_unknown{0};
std::atomic<float> g_last_bias{0.0f};
std::atomic<uint32_t> g_last_af{0};
std::atomic<uint64_t> g_material_binds{0};
std::atomic<float> g_original_bias{0};

uint64_t Key(float bias, uint32_t af)
{
    uint32_t b = 0;
    static_assert(sizeof b == sizeof bias, "float bits");
    std::memcpy(&b, &bias, sizeof b);
    return (uint64_t(b) << 32) | af;
}

SamplerFacts Facts(const D3D11_SAMPLER_DESC& d)
{
    SamplerFacts s;
    s.filter = uint32_t(d.Filter);
    s.address_u = uint32_t(d.AddressU);
    s.address_v = uint32_t(d.AddressV);
    s.max_anisotropy = d.MaxAnisotropy;
    s.mip_lod_bias = d.MipLODBias;
    s.max_lod = d.MaxLOD;
    return s;
}

HRESULT STDMETHODCALLTYPE CreateSamplerState_d(ID3D11Device* dev, const D3D11_SAMPLER_DESC* d, ID3D11SamplerState** out)
{
    const HRESULT hr = g_create(dev, d, out);
    if (SUCCEEDED(hr) && d && out && *out && dev == g_device.load(std::memory_order_acquire) && !IsOwnWork()) {
        g_created.fetch_add(1, std::memory_order_relaxed);
        auto e = std::make_shared<Entry>();
        e->desc = *d;
        e->original = *out;
        const SamplerFacts f = Facts(*d);
        e->wants_bias = WantsBias(f);
        e->wants_af = WantsAniso(f);
        std::unique_lock<std::shared_mutex> l(g_mu);
        auto& slot = g_entries[*out];
        const bool fresh = !slot || std::memcmp(&slot->desc, d, sizeof *d) != 0;
        if (fresh) {
            if (e->wants_bias) g_elig_bias.fetch_add(1, std::memory_order_relaxed);
            if (e->wants_af) g_elig_af.fetch_add(1, std::memory_order_relaxed);
            slot = std::move(e);
        }
    }
    return hr;
}

// The variant of `e` for (bias, af), made on first use; null = bind the game's own.
ID3D11SamplerState* VariantOf(Entry& e, float bias, uint32_t af, bool& created)
{
    if (!(e.wants_bias && bias != 0.0f) && !(e.wants_af && af)) return nullptr;
    const uint64_t key = Key(bias, af);
    std::lock_guard<std::mutex> l(e.mu);
    for (auto& v : e.variants)
        if (v.first == key) return v.second.Get();
    SamplerFacts f = Facts(e.desc);
    ComPtr<ID3D11SamplerState> made;
    if (ApplyPolicy(f, bias, af)) {
        D3D11_SAMPLER_DESC d = e.desc;
        d.Filter = D3D11_FILTER(f.filter);
        d.MaxAnisotropy = f.max_anisotropy;
        d.MipLODBias = f.mip_lod_bias;
        ID3D11Device* dev = g_device.load(std::memory_order_acquire);
        // Through the original: our own variants are not game samplers.
        if (!dev || FAILED(g_create(dev, &d, &made)) || !made) {
            g_variant_failures.fetch_add(1, std::memory_order_relaxed);
            made.Reset();
        } else {
            g_variants.fetch_add(1, std::memory_order_relaxed);
            created = true;
        }
    }
    e.variants.emplace_back(key, made); // a null entry remembers "nothing to change" for this key
    return made.Get();
}

void Bind(PFN_SetSamplers original, ID3D11DeviceContext* c, UINT start, UINT n, ID3D11SamplerState* const* s)
{
    if (!IsGameWork(c) || !s || !n || start >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT ||
        n > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - start) return original(c, start, n, s);
    const float bias = g_bias_fn ? g_bias_fn() : 0.0f;
    const uint32_t af = g_af_fn ? g_af_fn() : 0u;
    ID3D11SamplerState* swapped[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT]{};
    std::shared_ptr<Entry> entries[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
    {
        std::shared_lock<std::shared_mutex> lock(g_mu);
        for (UINT i = 0; i < n; ++i) {
            const auto it = g_entries.find(s[i]);
            if (it != g_entries.end()) entries[i] = it->second;
        }
    }
    uint64_t unknown = 0, changed = 0;
    for (UINT i = 0; i < n; ++i) {
        swapped[i] = s[i];
        if (!s[i]) continue;
        const auto& e = entries[i];
        if (!e) { ++unknown; continue; }
        swapped[i] = e->original.Get();
        bool created = false;
        if (auto* v = VariantOf(*e, bias, af, created)) {
            swapped[i] = v;
            if (created) {
                std::unique_lock<std::shared_mutex> lock(g_mu);
                g_entries.emplace(v, e); // lets a cached/GetSamplers variant restore its original
            }
        }
        if (swapped[i] != e->original.Get()) ++changed;
        if (e->wants_bias || e->wants_af) {
            D3D11_SAMPLER_DESC applied{};
            swapped[i]->GetDesc(&applied);
            if (e->wants_bias) {
                g_original_bias.store(e->desc.MipLODBias, std::memory_order_relaxed);
                g_last_bias.store(applied.MipLODBias, std::memory_order_relaxed);
                g_material_binds.fetch_add(1, std::memory_order_relaxed);
            }
            if (e->wants_af) g_last_af.store(sampler_bits::Anisotropic(applied.Filter) ? applied.MaxAnisotropy : 1u,
                                           std::memory_order_relaxed);
        }
    }
    g_binds.fetch_add(n, std::memory_order_relaxed);
    if (unknown) g_unknown.fetch_add(unknown, std::memory_order_relaxed);
    if (changed) g_swapped.fetch_add(changed, std::memory_order_relaxed);
    original(c, start, n, swapped);
}

// Immediate-class detours (also serve the deferred class when both share these functions).
void STDMETHODCALLTYPE PSSetSamplers_d(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11SamplerState* const* s)
{
    Bind(g_ps[0], c, start, n, s);
}
void STDMETHODCALLTYPE CSSetSamplers_d(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11SamplerState* const* s)
{
    Bind(g_cs[0], c, start, n, s);
}
// Deferred-class detours (separate trampolines when the two classes have different functions).
void STDMETHODCALLTYPE PSSetSamplersDeferred_d(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11SamplerState* const* s)
{
    Bind(g_ps[1], c, start, n, s);
}
void STDMETHODCALLTYPE CSSetSamplersDeferred_d(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11SamplerState* const* s)
{
    Bind(g_cs[1], c, start, n, s);
}

} // namespace

bool InstallSamplers(void* device, SamplerBiasFn bias, SamplerAfFn af)
{
    std::lock_guard<std::mutex> g(g_install_mu);
    auto* dev = static_cast<ID3D11Device*>(device);
    if (!dev) return false;
    g_bias_fn = bias;
    g_af_fn = af;
    g_device.store(dev, std::memory_order_release);
    if (g_installed.load()) return true;
    if (!na::hooking::Initialize()) return false;
    ComPtr<ID3D11DeviceContext> imm, def;
    dev->GetImmediateContext(&imm);
    dev->CreateDeferredContext(0, &def);
    bool ok = imm && na::hooking::BeginInstallWait("native11_samplers");
    ok = ok && na::hooking::PrepareVTableSlot(dev, kCreateSamplerState, reinterpret_cast<void*>(&CreateSamplerState_d),
                                              reinterpret_cast<void**>(&g_create));
    ok = ok && na::hooking::PrepareVTableSlot(imm.Get(), kPSSetSamplers, reinterpret_cast<void*>(&PSSetSamplers_d),
                                              reinterpret_cast<void**>(&g_ps[0]));
    ok = ok && na::hooking::PrepareVTableSlot(imm.Get(), kCSSetSamplers, reinterpret_cast<void*>(&CSSetSamplers_d),
                                              reinterpret_cast<void**>(&g_cs[0]));
    if (ok && def) {
        // The deferred class may share the immediate class's functions; the core then returns the
        // existing trampoline and the immediate detour serves both.
        void** vt_imm = *reinterpret_cast<void***>(imm.Get());
        void** vt_def = *reinterpret_cast<void***>(def.Get());
        if (vt_def[kPSSetSamplers] != vt_imm[kPSSetSamplers])
            ok = na::hooking::PrepareVTableSlot(def.Get(), kPSSetSamplers, reinterpret_cast<void*>(&PSSetSamplersDeferred_d),
                                                reinterpret_cast<void**>(&g_ps[1]));
        if (ok && vt_def[kCSSetSamplers] != vt_imm[kCSSetSamplers])
            ok = na::hooking::PrepareVTableSlot(def.Get(), kCSSetSamplers, reinterpret_cast<void*>(&CSSetSamplersDeferred_d),
                                                reinterpret_cast<void**>(&g_cs[1]));
    }
    if (ok) ok = na::hooking::CommitInstall();
    else na::hooking::AbandonInstall();
    if (!g_ps[1]) g_ps[1] = g_ps[0];
    if (!g_cs[1]) g_cs[1] = g_cs[0];
    g_installed.store(ok);
    na::eventlog::Emit("native11_samplers", "install ok=%d deferred=%d", ok ? 1 : 0, def ? 1 : 0);
    return ok;
}

uint64_t SamplerPolicyKey()
{
    return g_installed.load(std::memory_order_relaxed)
        ? Key(g_bias_fn ? g_bias_fn() : 0.f, g_af_fn ? g_af_fn() : 0u) : 0;
}
void RefreshSamplers(void* context)
{
    auto* c = static_cast<ID3D11DeviceContext*>(context);
    if (!g_installed.load() || !IsGameWork(c)) return;
    ID3D11SamplerState* bound[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT]{};
    const int deferred = c->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED ? 1 : 0;
    c->PSGetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, bound);
    Bind(g_ps[deferred], c, 0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, bound);
    for (auto* s : bound) if (s) s->Release();
    c->CSGetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, bound);
    Bind(g_cs[deferred], c, 0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, bound);
    for (auto* s : bound) if (s) s->Release();
}

SamplerStats GetSamplerStats()
{
    SamplerStats s;
    s.created = g_created.load();
    s.eligible_bias = g_elig_bias.load();
    s.eligible_af = g_elig_af.load();
    s.variants = g_variants.load();
    s.variant_failures = g_variant_failures.load();
    s.binds = g_binds.load();
    s.swapped = g_swapped.load();
    s.unknown = g_unknown.load();
    s.material_binds = g_material_binds.load();
    s.original_bias = g_original_bias.load();
    s.bias = g_last_bias.load();
    s.af = g_last_af.load();
    s.installed = g_installed.load();
    return s;
}

int FormatSamplerStatus(char* out, unsigned long n)
{
    const SamplerStats s = GetSamplerStats();
    const float want_bias = g_bias_fn ? g_bias_fn() : 0.0f;
    const uint32_t want_af = g_af_fn ? g_af_fn() : 0u;
    return std::snprintf(out, n,
                         "native11_samplers{installed=%d want_bias=%.3f want_af=%u applied_bias=%.3f applied_af=%u created=%llu "
                         "material=%llu af_eligible=%llu variants=%llu variant_failures=%llu binds=%llu swapped=%llu unknown=%llu}",
                         s.installed ? 1 : 0, want_bias, want_af, s.bias, s.af, static_cast<unsigned long long>(s.created),
                         static_cast<unsigned long long>(s.eligible_bias), static_cast<unsigned long long>(s.eligible_af),
                         static_cast<unsigned long long>(s.variants), static_cast<unsigned long long>(s.variant_failures),
                         static_cast<unsigned long long>(s.binds), static_cast<unsigned long long>(s.swapped),
                         static_cast<unsigned long long>(s.unknown));
}

} // namespace na::native11
