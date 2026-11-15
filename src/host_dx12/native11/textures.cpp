// Native DX11 route: material texture census and replacement (see textures.hpp).
#include "host_dx12/native11/textures.hpp"

#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/native11/input_provider.hpp"
#include "host_dx12/native11/samplers.hpp"
#include "host_dx12/native11/texture_policy.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace na::native11 {
namespace {

using Microsoft::WRL::ComPtr;

enum : uint32_t { kCreateShaderResourceView = 7 }; // ID3D11Device
enum : uint32_t { kPSGetShaderResources = 73 };    // ID3D11DeviceContext
constexpr unsigned kSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
constexpr uint64_t kCensusEvery = 16;   // frames; binds are counted on one frame in this many
constexpr uint64_t kGraveFrames = 8;    // tidy-up delay only: a retired replacement is released this many Presents later.
                                        // Safety does not rest on it: every use between lookup and the real bind holds a reference.
constexpr size_t kMaxEntries = 262144;  // views tracked at once; beyond it new views are not judged
constexpr size_t kCensusRows = 48;

using PFN_CreateSrv = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                                  ID3D11ShaderResourceView**);
using PFN_GetSrvs = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView**);

PFN_CreateSrv g_create = nullptr;
PFN_GetSrvs g_get[2] = {}; // [0] immediate class, [1] deferred class

struct Entry {
    uint64_t serial = 0;
    MaterialTextureFacts facts;
    TextureVerdict verdict = TextureVerdict::NotTexture2D;
    std::atomic<uint64_t> binds{0}, scene_binds{0};
    std::atomic<uint32_t> slot{~0u}, set_key{0}, set_size{0};
    ComPtr<ID3D11ShaderResourceView> replacement, pending; // under State::mu
    uint64_t bytes = 0, pending_bytes = 0;
    bool pending_set = false; // a publish or a removal waits for the next Present
};

// Never destroyed: a view's destruction can reach Retire after this module's statics are gone.
struct State {
    std::shared_mutex mu;
    std::unordered_map<ID3D11ShaderResourceView*, std::shared_ptr<Entry>> entries;
    std::vector<ID3D11ShaderResourceView*> pending;                                   // game views with pending_set
    std::vector<std::pair<uint64_t, ComPtr<ID3D11ShaderResourceView>>> grave;         // (frame retired, view)
};
State& S()
{
    static State* s = new State;
    return *s;
}

std::atomic<ID3D11Device*> g_device{nullptr};
std::atomic<bool> g_installed{false}, g_enabled{true}, g_census_frame{true};
std::mutex g_install_mu;
std::atomic<uint64_t> g_serial{1}, g_generation{1}, g_frame{0};
std::atomic<uint64_t> g_views{0}, g_candidates{0}, g_retired{0}, g_overflow{0};
std::atomic<uint64_t> g_replaced{0}, g_replaced_bytes{0}, g_pending{0};
std::atomic<uint64_t> g_swapped{0}, g_swapped_deferred{0}, g_returned{0};
std::atomic<uint64_t> g_verdicts[static_cast<size_t>(TextureVerdict::kCount)]{};
uint64_t g_census_next_ms = 0; // Present thread only

// {3f9c2d71-8a54-4b0e-b6c3-5e1a9d7f2c48}: a view's entry, by the lifetime of its private data.
const GUID kLifeGuid = {0x3f9c2d71, 0x8a54, 0x4b0e, {0xb6, 0xc3, 0x5e, 0x1a, 0x9d, 0x7f, 0x2c, 0x48}};

void Retire(ID3D11ShaderResourceView* key, uint64_t serial)
{
    State& s = S();
    std::unique_lock<std::shared_mutex> l(s.mu);
    const auto it = s.entries.find(key);
    if (it == s.entries.end() || it->second->serial != serial) return;
    Entry& e = *it->second;
    const uint64_t frame = g_frame.load(std::memory_order_relaxed);
    if (e.replacement) {
        g_replaced.fetch_sub(1, std::memory_order_relaxed);
        g_replaced_bytes.fetch_sub(e.bytes, std::memory_order_relaxed);
        s.grave.emplace_back(frame, std::move(e.replacement)); // no D3D release inside a destruction
        g_generation.fetch_add(1, std::memory_order_relaxed);
    }
    if (e.pending) s.grave.emplace_back(frame, std::move(e.pending));
    if (e.pending_set) {
        s.pending.erase(std::remove(s.pending.begin(), s.pending.end(), key), s.pending.end());
        g_pending.fetch_sub(1, std::memory_order_relaxed);
    }
    s.entries.erase(it);
    g_retired.fetch_add(1, std::memory_order_relaxed);
}

// Lives in a game view's private data; D3D releases it when the view is destroyed.
struct Life final : IUnknown {
    std::atomic<ULONG> refs{1};
    ID3D11ShaderResourceView* key;
    uint64_t serial;
    Life(ID3D11ShaderResourceView* k, uint64_t n) : key(k), serial(n) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        if (IsEqualIID(iid, IID_IUnknown)) {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (!left) {
            Retire(key, serial);
            delete this;
        }
        return left;
    }
};

MaterialTextureFacts FactsOf(ID3D11Resource* resource, ID3D11ShaderResourceView* view)
{
    MaterialTextureFacts t;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return t;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&tex))) || !tex) return t;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    view->GetDesc(&vd);
    t.texture2d = true;
    t.width = td.Width;
    t.height = td.Height;
    t.mips = td.MipLevels;
    t.array_size = td.ArraySize;
    t.samples = td.SampleDesc.Count;
    t.format = static_cast<uint32_t>(td.Format);
    t.view_format = static_cast<uint32_t>(vd.Format);
    t.bind_flags = td.BindFlags;
    t.usage = static_cast<uint32_t>(td.Usage);
    t.cube = (td.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0 || vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBE ||
             vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
    UINT first = 0, count = td.MipLevels;
    if (vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
        first = vd.Texture2D.MostDetailedMip;
        count = vd.Texture2D.MipLevels;
    } else if (vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY) {
        first = vd.Texture2DArray.MostDetailedMip;
        count = vd.Texture2DArray.MipLevels;
    }
    if (first > td.MipLevels) first = td.MipLevels;
    if (count == ~0u || count > td.MipLevels - first) count = td.MipLevels - first;
    t.view_first_mip = first;
    t.view_mips = count;
    return t;
}

HRESULT STDMETHODCALLTYPE CreateShaderResourceView_d(ID3D11Device* dev, ID3D11Resource* r, const D3D11_SHADER_RESOURCE_VIEW_DESC* d,
                                                     ID3D11ShaderResourceView** out)
{
    const HRESULT hr = g_create(dev, r, d, out);
    if (FAILED(hr) || !r || !out || !*out || dev != g_device.load(std::memory_order_acquire) || IsOwnWork()) return hr;
    ID3D11ShaderResourceView* view = *out;
    // The same live object handed out again keeps its entry.
    IUnknown* have = nullptr;
    UINT size = sizeof(have);
    if (SUCCEEDED(view->GetPrivateData(kLifeGuid, &size, &have)) && have) {
        have->Release();
        return hr;
    }
    State& s = S();
    auto e = std::make_shared<Entry>();
    e->serial = g_serial.fetch_add(1, std::memory_order_relaxed);
    e->facts = FactsOf(r, view);
    e->verdict = Judge(e->facts);
    {
        std::unique_lock<std::shared_mutex> l(s.mu);
        if (s.entries.size() >= kMaxEntries) {
            g_overflow.fetch_add(1, std::memory_order_relaxed);
            return hr;
        }
        s.entries[view] = e; // an entry left at this address by an untagged earlier object is replaced
    }
    g_views.fetch_add(1, std::memory_order_relaxed);
    g_verdicts[static_cast<size_t>(e->verdict)].fetch_add(1, std::memory_order_relaxed);
    if (e->verdict == TextureVerdict::Candidate) g_candidates.fetch_add(1, std::memory_order_relaxed);
    Life* life = new Life(view, e->serial);
    view->SetPrivateDataInterface(kLifeGuid, life); // the view now holds the only lasting reference
    life->Release();
    return hr;
}

// What the game asked for, slot by slot: where a replacement is bound in its place, the game's own
// view from this context's shadow (which holds the reference that makes handing it out safe).
void GetShaderResources(PFN_GetSrvs original, ID3D11DeviceContext* c, UINT start, UINT n, ID3D11ShaderResourceView** out)
{
    original(c, start, n, out);
    if (!out || !n || start >= kSlots || n > kSlots - start || IsOwnWork()) return;
    PsShadow* shadow = TextureShadowOf(c);
    if (!shadow || !shadow->held) return;
    uint64_t returned = 0;
    for (UINT i = 0; i < n; ++i) {
        ID3D11ShaderResourceView* game = shadow->game[start + i];
        if (!game || !out[i] || out[i] == game) continue;
        game->AddRef();
        out[i]->Release(); // not the last reference: the slot still binds it
        out[i] = game;
        ++returned;
    }
    if (returned) g_returned.fetch_add(returned, std::memory_order_relaxed);
}
void STDMETHODCALLTYPE PSGetShaderResources_d(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11ShaderResourceView** out)
{
    GetShaderResources(g_get[0], c, start, n, out);
}
void STDMETHODCALLTYPE PSGetShaderResourcesDeferred_d(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11ShaderResourceView** out)
{
    GetShaderResources(g_get[1], c, start, n, out);
}

void EmitCensus()
{
    struct Row {
        MaterialTextureFacts f;
        uint64_t scene_binds, binds;
        uint32_t slot, set_key, set_size;
        bool replaced;
    };
    std::vector<Row> rows;
    uint64_t candidates = 0, bytes = 0;
    {
        State& s = S();
        std::shared_lock<std::shared_mutex> l(s.mu);
        for (const auto& kv : s.entries) {
            const Entry& e = *kv.second;
            if (e.verdict != TextureVerdict::Candidate) continue;
            ++candidates;
            bytes += ChainBytes(e.facts.width, e.facts.height, e.facts.mips, e.facts.view_format);
            const uint64_t scene = e.scene_binds.load(std::memory_order_relaxed);
            if (!scene) continue;
            rows.push_back({e.facts, scene, e.binds.load(std::memory_order_relaxed), e.slot.load(std::memory_order_relaxed),
                            e.set_key.load(std::memory_order_relaxed), e.set_size.load(std::memory_order_relaxed),
                            e.replacement.Get() != nullptr});
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.scene_binds > b.scene_binds; });
    const size_t used = rows.size();
    if (rows.size() > kCensusRows) rows.resize(kCensusRows);
    uint64_t top_bytes = 0, top_double = 0;
    for (const Row& r : rows) {
        top_bytes += ChainBytes(r.f.width, r.f.height, r.f.mips, r.f.view_format);
        top_double += ChainBytes(r.f.width * 2, r.f.height * 2, r.f.mips + 1, r.f.view_format);
    }
    na::eventlog::Emit("native11_texture_census",
                       "candidates_live=%llu candidates_bytes=%llu used_in_scene=%zu listed=%zu listed_bytes=%llu "
                       "listed_at_double_size_bytes=%llu counted_every=%llu "
                       "note=\"binds are counted on one frame in %llu, before the picture is handed to DLSS; they rank use, they are "
                       "not pixel coverage; set= names textures bound in one call, a candidate association, not a proven material\"",
                       static_cast<unsigned long long>(candidates), static_cast<unsigned long long>(bytes), used, rows.size(),
                       static_cast<unsigned long long>(top_bytes), static_cast<unsigned long long>(top_double),
                       static_cast<unsigned long long>(kCensusEvery), static_cast<unsigned long long>(kCensusEvery));
    int rank = 0;
    for (const Row& r : rows)
        na::eventlog::Emit("native11_texture",
                           "rank=%d w=%u h=%u mips=%u format=%u view_format=%u class=%s slot=%u scene_binds=%llu binds=%llu "
                           "set=%08x set_size=%u bytes=%llu replaced=%d",
                           rank++, r.f.width, r.f.height, r.f.mips, r.f.format, r.f.view_format, Name(ClassOf(r.f.view_format)),
                           r.slot, static_cast<unsigned long long>(r.scene_binds), static_cast<unsigned long long>(r.binds),
                           r.set_key, r.set_size,
                           static_cast<unsigned long long>(ChainBytes(r.f.width, r.f.height, r.f.mips, r.f.view_format)),
                           r.replaced ? 1 : 0);
}

} // namespace

bool InstallTextures(void* device)
{
    std::lock_guard<std::mutex> g(g_install_mu);
    auto* dev = static_cast<ID3D11Device*>(device);
    if (!dev) return false;
    g_device.store(dev, std::memory_order_release);
    if (g_installed.load()) return true;
    if (!na::hooking::Initialize()) return false;
    ComPtr<ID3D11DeviceContext> imm, def;
    dev->GetImmediateContext(&imm);
    dev->CreateDeferredContext(0, &def);
    bool ok = imm && na::hooking::BeginInstallWait("native11_textures");
    ok = ok && na::hooking::PrepareVTableSlot(dev, kCreateShaderResourceView, reinterpret_cast<void*>(&CreateShaderResourceView_d),
                                              reinterpret_cast<void**>(&g_create));
    ok = ok && na::hooking::PrepareVTableSlot(imm.Get(), kPSGetShaderResources, reinterpret_cast<void*>(&PSGetShaderResources_d),
                                              reinterpret_cast<void**>(&g_get[0]));
    if (ok && def) {
        void** vt_imm = *reinterpret_cast<void***>(imm.Get());
        void** vt_def = *reinterpret_cast<void***>(def.Get());
        if (vt_def[kPSGetShaderResources] != vt_imm[kPSGetShaderResources])
            ok = na::hooking::PrepareVTableSlot(def.Get(), kPSGetShaderResources,
                                                reinterpret_cast<void*>(&PSGetShaderResourcesDeferred_d),
                                                reinterpret_cast<void**>(&g_get[1]));
    }
    if (ok) ok = na::hooking::CommitInstall();
    else na::hooking::AbandonInstall();
    if (!g_get[1]) g_get[1] = g_get[0];
    g_installed.store(ok);
    na::eventlog::Emit("native11_textures", "install ok=%d deferred=%d", ok ? 1 : 0, def ? 1 : 0);
    return ok;
}

ID3D11ShaderResourceView* const* SwapTextures(ID3D11DeviceContext* c, PsShadow& shadow, unsigned start, unsigned count,
                                              ID3D11ShaderResourceView* const* views, ID3D11ShaderResourceView** scratch)
{
    if (!views || !count || start >= kSlots || count > kSlots - start || !g_installed.load(std::memory_order_relaxed))
        return views;
    const bool census = g_census_frame.load(std::memory_order_relaxed);
    const bool replace = g_enabled.load(std::memory_order_relaxed) && g_replaced.load(std::memory_order_relaxed) != 0;
    if (!census && !replace) {
        // Nothing to bind in place of anything: the game's views go in as they are, so no slot of
        // this call holds a replacement any more.
        if (shadow.held)
            for (unsigned i = 0; i < count; ++i) shadow.Set(start + i, nullptr);
        return views;
    }
    uint32_t set_key = 0;
    if (census && count > 1) {
        uint64_t h = 1469598103934665603ull; // FNV-1a over the bound views of this call
        for (unsigned i = 0; i < count; ++i) h = (h ^ reinterpret_cast<uintptr_t>(views[i])) * 1099511628211ull;
        set_key = static_cast<uint32_t>(h ^ (h >> 32));
    }
    const bool in_scene = census && !SceneHanded();
    uint64_t changed = 0;
    State& s = S();
    {
        std::shared_lock<std::shared_mutex> l(s.mu);
        for (unsigned i = 0; i < count; ++i) {
            scratch[i] = views[i];
            if (!views[i]) continue;
            const auto it = s.entries.find(views[i]);
            if (it == s.entries.end()) continue;
            Entry& e = *it->second;
            if (census) {
                e.binds.fetch_add(1, std::memory_order_relaxed);
                if (in_scene) e.scene_binds.fetch_add(1, std::memory_order_relaxed);
                e.slot.store(start + i, std::memory_order_relaxed);
                if (set_key) {
                    e.set_key.store(set_key, std::memory_order_relaxed);
                    e.set_size.store(count, std::memory_order_relaxed);
                }
            }
            if (replace && e.replacement) {
                scratch[i] = e.replacement.Get();
                scratch[i]->AddRef(); // held until EndTextureSwap, after the real bind has taken its own
                ++changed;
            }
        }
    }
    // The game's own view is kept for exactly the slots that now hold a replacement.
    if (changed || shadow.held)
        for (unsigned i = 0; i < count; ++i) shadow.Set(start + i, scratch[i] != views[i] ? views[i] : nullptr);
    if (!changed) return views;
    if (c->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE) g_swapped.fetch_add(changed, std::memory_order_relaxed);
    else g_swapped_deferred.fetch_add(changed, std::memory_order_relaxed);
    return scratch;
}

void EndTextureSwap(unsigned count, ID3D11ShaderResourceView* const* views, ID3D11ShaderResourceView* const* bound)
{
    if (!views || !bound || bound == views) return;
    for (unsigned i = 0; i < count; ++i)
        if (bound[i] && bound[i] != views[i]) bound[i]->Release();
}

uint64_t TexturePolicyKey() { return g_installed.load(std::memory_order_relaxed) ? g_generation.load(std::memory_order_relaxed) : 0; }

void RefreshTextures(ID3D11DeviceContext* c, PsShadow& shadow, TextureSetFn original_set)
{
    if (!c || !original_set || !g_installed.load()) return;
    // Nothing is bound in place of anything on this context, and nothing could be: leave it alone.
    if (!shadow.held && (!g_enabled.load(std::memory_order_relaxed) || g_replaced.load(std::memory_order_relaxed) == 0)) return;
    const int deferred = c->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED ? 1 : 0;
    ID3D11ShaderResourceView* bound[kSlots]{}; // referenced until the end of this call
    ID3D11ShaderResourceView* game[kSlots]{};
    ID3D11ShaderResourceView* want[kSlots]{};
    bool leased[kSlots]{}; // want[i] is a replacement referenced here until the bind below has taken its own
    g_get[deferred](c, 0, kSlots, bound); // what is really bound: replacements included
    const bool replace = g_enabled.load(std::memory_order_relaxed);
    bool differs = false;
    State& s = S();
    {
        std::shared_lock<std::shared_mutex> l(s.mu);
        for (unsigned i = 0; i < kSlots; ++i) {
            if (!bound[i]) continue; // an empty slot has no game view behind it (the shadow is dropped below)
            // The game's view for this slot: the shadow's where a replacement is bound, else what is bound.
            game[i] = shadow.game[i] ? shadow.game[i] : bound[i];
            want[i] = game[i];
            const auto it = s.entries.find(game[i]);
            if (replace && it != s.entries.end() && it->second->replacement) {
                want[i] = it->second->replacement.Get();
                want[i]->AddRef();
                leased[i] = true;
            }
            if (want[i] != bound[i]) differs = true;
        }
    }
    // Every view in want[] is referenced here: game views by `bound` or the shadow, replacements by
    // the lease just taken. Released only after the bind, and outside the table's lock.
    if (differs) original_set(c, 0, kSlots, want);
    for (unsigned i = 0; i < kSlots; ++i) shadow.Set(i, want[i] != game[i] ? game[i] : nullptr);
    for (unsigned i = 0; i < kSlots; ++i) {
        if (leased[i]) want[i]->Release();
        if (bound[i]) bound[i]->Release();
    }
}

void TexturesOnPresent()
{
    if (!g_installed.load(std::memory_order_relaxed)) return;
    const uint64_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    g_census_frame.store(frame % kCensusEvery == 0, std::memory_order_relaxed);
    std::vector<ComPtr<ID3D11ShaderResourceView>> release; // dropped after the lock
    State& s = S();
    {
        std::unique_lock<std::shared_mutex> l(s.mu);
        bool any = false;
        for (ID3D11ShaderResourceView* key : s.pending) {
            const auto it = s.entries.find(key);
            if (it == s.entries.end()) continue;
            Entry& e = *it->second;
            if (!e.pending_set) continue;
            if (e.replacement) {
                g_replaced.fetch_sub(1, std::memory_order_relaxed);
                g_replaced_bytes.fetch_sub(e.bytes, std::memory_order_relaxed);
                s.grave.emplace_back(frame, std::move(e.replacement));
            }
            e.replacement = std::move(e.pending);
            e.bytes = e.pending_bytes;
            e.pending_set = false;
            if (e.replacement) {
                g_replaced.fetch_add(1, std::memory_order_relaxed);
                g_replaced_bytes.fetch_add(e.bytes, std::memory_order_relaxed);
            }
            any = true;
        }
        g_pending.store(0, std::memory_order_relaxed);
        s.pending.clear();
        if (any) g_generation.fetch_add(1, std::memory_order_relaxed);
        size_t kept = 0;
        for (auto& dead : s.grave) {
            if (dead.first + kGraveFrames <= frame) release.push_back(std::move(dead.second));
            else s.grave[kept++] = std::move(dead);
        }
        s.grave.resize(kept);
    }
    release.clear();
    const uint64_t now = GetTickCount64();
    if (!g_census_next_ms) g_census_next_ms = now + 45000;
    if (now >= g_census_next_ms) {
        g_census_next_ms = now + 120000;
        EmitCensus();
    }
}

bool PublishTexture(ID3D11ShaderResourceView* original, ID3D11ShaderResourceView* replacement, uint64_t bytes)
{
    if (!original || !replacement || original == replacement) return false;
    State& s = S();
    ComPtr<ID3D11ShaderResourceView> displaced;
    {
        std::unique_lock<std::shared_mutex> l(s.mu);
        const auto it = s.entries.find(original);
        if (it == s.entries.end()) return false;
        // A replacement is never itself a game view to judge or replace.
        const auto own = s.entries.find(replacement);
        if (own != s.entries.end()) {
            if (own->second->replacement || own->second->pending_set) return false;
            s.entries.erase(own);
        }
        Entry& e = *it->second;
        if (e.replacement.Get() == replacement && !e.pending_set) return true; // already visible as asked
        displaced = std::move(e.pending);
        e.pending = replacement;
        e.pending_bytes = bytes;
        if (!e.pending_set) {
            e.pending_set = true;
            s.pending.push_back(original);
            g_pending.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return true;
}

bool RemoveTexture(ID3D11ShaderResourceView* original)
{
    State& s = S();
    ComPtr<ID3D11ShaderResourceView> displaced;
    std::unique_lock<std::shared_mutex> l(s.mu);
    const auto it = s.entries.find(original);
    if (it == s.entries.end()) return false;
    Entry& e = *it->second;
    if (!e.replacement && !e.pending_set) return false;
    displaced = std::move(e.pending);
    e.pending_bytes = 0;
    if (!e.pending_set) {
        e.pending_set = true;
        s.pending.push_back(original);
        g_pending.fetch_add(1, std::memory_order_relaxed);
    }
    l.unlock();
    return true;
}

void SetTexturesEnabled(bool on)
{
    if (g_enabled.exchange(on) != on) g_generation.fetch_add(1, std::memory_order_relaxed);
}

TextureStats GetTextureStats()
{
    TextureStats t;
    t.views = g_views.load();
    t.candidates = g_candidates.load();
    t.retired = g_retired.load();
    t.replaced = g_replaced.load();
    t.pending = g_pending.load();
    t.swapped = g_swapped.load();
    t.swapped_deferred = g_swapped_deferred.load();
    t.returned_original = g_returned.load();
    t.replaced_bytes = g_replaced_bytes.load();
    t.generation = g_generation.load();
    t.installed = g_installed.load();
    t.enabled = g_enabled.load();
    {
        State& s = S();
        std::shared_lock<std::shared_mutex> l(s.mu);
        t.live = s.entries.size();
    }
    return t;
}

int FormatTextureStatus(char* out, unsigned long n)
{
    const TextureStats t = GetTextureStats();
    const auto v = [](TextureVerdict x) { return static_cast<unsigned long long>(g_verdicts[static_cast<size_t>(x)].load()); };
    return std::snprintf(out, n,
                         "native11_textures{installed=%d enabled=%d views=%llu candidates=%llu not{rendered_to=%llu rewritten=%llu "
                         "data_format=%llu too_small=%llu no_mips=%llu partial_view=%llu cube=%llu array=%llu multisampled=%llu "
                         "not_2d=%llu} untracked=%llu live=%llu retired=%llu replaced=%llu replaced_bytes=%llu pending=%llu "
                         "swapped=%llu swapped_deferred=%llu returned_original=%llu generation=%llu}",
                         t.installed ? 1 : 0, t.enabled ? 1 : 0, static_cast<unsigned long long>(t.views),
                         static_cast<unsigned long long>(t.candidates), v(TextureVerdict::RenderedTo), v(TextureVerdict::Rewritten),
                         v(TextureVerdict::DataFormat), v(TextureVerdict::TooSmall), v(TextureVerdict::NoMips),
                         v(TextureVerdict::PartialView), v(TextureVerdict::Cube), v(TextureVerdict::Array),
                         v(TextureVerdict::Multisampled), v(TextureVerdict::NotTexture2D),
                         static_cast<unsigned long long>(g_overflow.load()), static_cast<unsigned long long>(t.live),
                         static_cast<unsigned long long>(t.retired), static_cast<unsigned long long>(t.replaced),
                         static_cast<unsigned long long>(t.replaced_bytes), static_cast<unsigned long long>(t.pending),
                         static_cast<unsigned long long>(t.swapped), static_cast<unsigned long long>(t.swapped_deferred),
                         static_cast<unsigned long long>(t.returned_original), static_cast<unsigned long long>(t.generation));
}

} // namespace na::native11
