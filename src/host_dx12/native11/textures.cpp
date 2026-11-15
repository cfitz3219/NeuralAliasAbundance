// Native DX11 route: material texture census and replacement (see textures.hpp).
#include "host_dx12/native11/textures.hpp"

#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/native11/input_provider.hpp"
#include "host_dx12/native11/samplers.hpp"
#include "host_dx12/native11/texture_loader.hpp"
#include "host_dx12/native11/texture_policy.hpp"
#include "materials/texture_subresources_v1.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace na::native11 {
namespace {

using Microsoft::WRL::ComPtr;

enum : uint32_t { kCreateTexture2D = 5, kCreateShaderResourceView = 7 }; // ID3D11Device
enum : uint32_t { kPSGetShaderResources = 73 };    // ID3D11DeviceContext
constexpr unsigned kSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
constexpr uint64_t kCensusEvery = 16;   // frames; binds are counted on one frame in this many
constexpr uint64_t kGraveFrames = 8;    // tidy-up delay only: a retired replacement is released this many Presents later.
                                        // Safety does not rest on it: every use between lookup and the real bind holds a reference.
constexpr size_t kMaxEntries = 262144;  // views tracked at once; beyond it new views are not judged
constexpr size_t kCensusRows = 48;
constexpr uint64_t kDumpCollectFrames = 32;        // Presents during which bound candidates are noted (two census frames)
constexpr size_t kDumpCollectMax = 256;            // noted at most; the most used are then kept
constexpr size_t kDumpTextures = 48;               // written per request
constexpr uint64_t kDumpBytes = 512ull << 20;      // and at most this much texture data per request
constexpr uint64_t kDumpReadFrames = 600;          // a queued copy not readable after this many Presents is given up

using PFN_CreateSrv = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                                  ID3D11ShaderResourceView**);
using PFN_GetSrvs = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView**);

using PFN_CreateTex2D = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*,
                                                    ID3D11Texture2D**);
PFN_CreateSrv g_create = nullptr;
PFN_CreateTex2D g_create_texture = nullptr;
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
    std::atomic<bool> dump_taken{false}; // noted for the running source dump
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

// Source dump (RequestTextureDump). A view is noted, with a reference, only inside the game's own
// bind call, where the game's view is certainly alive. Its copy is queued and later read on the Present thread
// without waiting for the GPU; hashing and file writes happen on the dump's own worker thread.
struct DumpItem {
    ComPtr<ID3D11ShaderResourceView> view;
    std::shared_ptr<Entry> entry;
};
// One texture's bytes on their way to disk: filled on the Present thread (memory copies only),
// hashed and written by the dump's worker thread.
struct DumpJob {
    MaterialTextureFacts facts;
    uint32_t slot = 0, set_key = 0, set_size = 0;
    uint64_t scene_binds = 0, binds = 0;
    std::vector<uint8_t> body;  // the levels, tightly packed, in order
    const char* why = nullptr;  // set when the texture could not be read: logged, nothing written
    bool last = false;          // no texture: the request is finished
};
// The one staging copy in flight. The copy is queued on the GPU and read back on a later Present,
// never waited for.
struct DumpRead {
    DumpItem item;
    ComPtr<ID3D11Texture2D> staging;
    uint64_t since_frame = 0;
};
struct Dump {
    std::mutex mu;
    std::vector<DumpItem> queue; // under mu while collecting; Present thread only while reading
    std::wstring dir;            // set under mu at the request; read by the worker after it
    uint64_t until_frame = 0;
    uint64_t noted = 0, over_budget = 0;
    std::unique_ptr<DumpRead> read; // Present thread only
    bool finishing = false;         // Present thread only: the last job has been handed over
    std::mutex work_mu;
    std::condition_variable work_cv;
    std::deque<DumpJob> work;
};
Dump& D()
{
    static Dump* d = new Dump;
    return *d;
}
std::atomic<int> g_dump_phase{0}; // 0 idle, 1 noting binds, 2 reading and writing
std::atomic<uint64_t> g_dump_written{0}, g_dump_failed{0};

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
    if (e->verdict == TextureVerdict::Candidate) MaterialsOnViewCreated(r, view, e->serial, e->facts);
    return hr;
}

// The game's creation data is the one cheap moment a texture's bytes are in hand (texture_loader.hpp).
HRESULT STDMETHODCALLTYPE CreateTexture2D_d(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* initial,
                                            ID3D11Texture2D** out)
{
    const HRESULT hr = g_create_texture(dev, desc, initial, out);
    if (FAILED(hr) || !desc || !out || !*out || dev != g_device.load(std::memory_order_acquire) || IsOwnWork()) return hr;
    MaterialsOnTextureCreated(dev, *desc, initial, *out);
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

bool WriteAll(const std::wstring& path, const void* head, size_t head_n, const std::vector<uint8_t>& body)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return false;
    const bool ok = std::fwrite(head, 1, head_n, f) == head_n && (body.empty() || std::fwrite(body.data(), 1, body.size(), f) == body.size());
    return std::fclose(f) == 0 && ok;
}

// Worker thread: names one texture's bytes by "texture-subresources-v1", writes <digest>.dds (DX10
// header, the view's typed format, bytes as stored) and then its index line. Returns the reason it
// could not, or null. `duplicate`: another view had the same content, so only the index line is new.
const char* WriteDumped(const DumpJob& job, const std::wstring& dir, bool& duplicate, uint64_t& file_bytes)
{
    const MaterialTextureFacts& f = job.facts;
    uint32_t block = 0, texel = 0;
    if (!na::materials::TexSubLayout(f.view_format, block, texel)) return "format has no v1 identity";
    na::materials::TexSubHeader h;
    h.resource_format = f.format;
    h.view_format = f.view_format;
    h.width = f.width;
    h.height = f.height;
    h.first_mip = 0;
    h.mip_count = f.mips;
    na::materials::TextureSubresourcesV1 digest(h);
    size_t at = 0;
    uint32_t first_level_bytes = 0;
    for (uint32_t mip = 0; mip < f.mips; ++mip) {
        const uint32_t w = (std::max)(1u, f.width >> mip), ht = (std::max)(1u, f.height >> mip);
        const uint32_t row = block ? ((w + 3) / 4) * block : w * texel, rows = block ? (ht + 3) / 4 : ht;
        const size_t size = size_t(row) * rows;
        if (at + size > job.body.size() || !digest.AddLevel(0, mip, job.body.data() + at, row)) return "levels incomplete";
        if (!mip) first_level_bytes = row * rows;
        at += size;
    }
    const std::string name = digest.Finish();
    if (name.empty() || at != job.body.size()) return "levels incomplete";
    const std::wstring path = dir + L"\\" + std::wstring(name.begin(), name.end()) + L".dds";
    duplicate = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!duplicate) {
        uint32_t head[37] = {};
        head[0] = 0x20534444; // "DDS "
        head[1] = 124;
        head[2] = 0x1 | 0x2 | 0x4 | 0x1000 | (block ? 0x80000u : 0x8u) | (f.mips > 1 ? 0x20000u : 0u);
        head[3] = f.height;
        head[4] = f.width;
        head[5] = block ? first_level_bytes : f.width * texel;
        head[7] = f.mips;
        head[19] = 32;
        head[20] = 0x4;
        head[21] = 0x30315844; // "DX10"
        head[27] = 0x1000 | (f.mips > 1 ? 0x8u | 0x400000u : 0u);
        head[32] = f.view_format;
        head[33] = 3; // 2D
        head[35] = 1;
        // Written under a temporary name: a file with the digest's name is always complete.
        const std::wstring partial = path + L".part";
        if (!WriteAll(partial, head, sizeof(head), job.body) || !MoveFileExW(partial.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(partial.c_str());
            return "the file could not be written";
        }
        file_bytes += job.body.size();
    }
    char line[1024];
    const int n = std::snprintf(line, sizeof(line),
                                "{\"content_sha256\":\"%s\",\"identity_domain\":\"texture-subresources-v1\",\"file\":\"%s.dds\","
                                "\"resource_format\":%u,\"view_format\":%u,\"width\":%u,\"height\":%u,\"first_mip\":0,\"mip_count\":%u,"
                                "\"first_array_slice\":0,\"array_size\":1,\"complete_observed_range\":true,\"class\":\"%s\",\"slot\":%u,"
                                "\"scene_binds\":%llu,\"binds\":%llu,\"set\":\"%08x\",\"set_size\":%u,\"bytes\":%zu,\"duplicate\":%s}\n",
                                name.c_str(), name.c_str(), f.format, f.view_format, f.width, f.height, f.mips, Name(ClassOf(f.view_format)),
                                job.slot, static_cast<unsigned long long>(job.scene_binds), static_cast<unsigned long long>(job.binds),
                                job.set_key, job.set_size, job.body.size(), duplicate ? "true" : "false");
    FILE* index = nullptr; // the index line follows the finished file, never precedes it
    if (n <= 0 || n >= static_cast<int>(sizeof(line)) || _wfopen_s(&index, (dir + L"\\index.jsonl").c_str(), L"ab") || !index)
        return "the index could not be written";
    const bool ok = std::fwrite(line, 1, static_cast<size_t>(n), index) == static_cast<size_t>(n);
    return std::fclose(index) == 0 && ok ? nullptr : "the index could not be written";
}

// One per request; ends with it. Hashing and file writes happen here, never in a game callback.
void DumpWorker(std::wstring dir, uint64_t noted, uint64_t over_budget)
{
    Dump& d = D();
    CreateDirectoryW(dir.c_str(), nullptr);
    uint64_t written = 0, duplicates = 0, failed = 0, file_bytes = 0;
    for (;;) {
        DumpJob job;
        {
            std::unique_lock<std::mutex> l(d.work_mu);
            d.work_cv.wait(l, [&] { return !d.work.empty(); });
            job = std::move(d.work.front());
            d.work.pop_front();
        }
        if (job.last) break;
        bool duplicate = false;
        const char* why = job.why ? job.why : WriteDumped(job, dir, duplicate, file_bytes);
        if (why) {
            ++failed;
            g_dump_failed.fetch_add(1, std::memory_order_relaxed);
            const MaterialTextureFacts& f = job.facts;
            na::eventlog::Emit("native11_texture_dump", "skipped w=%u h=%u mips=%u format=%u view_format=%u why=\"%s\"", f.width,
                               f.height, f.mips, f.format, f.view_format, why);
        } else {
            ++(duplicate ? duplicates : written);
            g_dump_written.fetch_add(1, std::memory_order_relaxed);
        }
    }
    na::eventlog::Emit("native11_texture_dump",
                       "done noted=%llu written=%llu same_content=%llu skipped=%llu not_kept=%llu file_bytes=%llu "
                       "note=\"candidates bound to pixel shaders before the picture was handed to DLSS over %llu frames, most used first; "
                       "at most %zu textures and %llu MiB per request; names are texture-subresources-v1 digests; index.jsonl lists them\"",
                       static_cast<unsigned long long>(noted), static_cast<unsigned long long>(written),
                       static_cast<unsigned long long>(duplicates), static_cast<unsigned long long>(failed),
                       static_cast<unsigned long long>(over_budget), static_cast<unsigned long long>(file_bytes),
                       static_cast<unsigned long long>(kDumpCollectFrames), kDumpTextures, static_cast<unsigned long long>(kDumpBytes >> 20));
    g_dump_phase.store(0, std::memory_order_release);
}

void HandToWorker(Dump& d, DumpJob&& job)
{
    {
        std::lock_guard<std::mutex> l(d.work_mu);
        d.work.push_back(std::move(job));
    }
    d.work_cv.notify_one();
}

DumpJob JobOf(const DumpItem& item)
{
    const Entry& e = *item.entry;
    DumpJob job;
    job.facts = e.facts;
    job.slot = e.slot.load(std::memory_order_relaxed);
    job.set_key = e.set_key.load(std::memory_order_relaxed);
    job.set_size = e.set_size.load(std::memory_order_relaxed);
    job.scene_binds = e.scene_binds.load(std::memory_order_relaxed);
    job.binds = e.binds.load(std::memory_order_relaxed);
    return job;
}

// Present thread: queues the GPU copy of the next texture and returns the reason it could not, or null.
const char* StartRead(DumpRead& read, uint64_t frame)
{
    uint32_t block = 0, texel = 0;
    if (!na::materials::TexSubLayout(read.item.entry->facts.view_format, block, texel)) return "format has no v1 identity";
    ComPtr<ID3D11Resource> resource;
    read.item.view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture))) return "not a 2D texture any more";
    ComPtr<ID3D11Device> device;
    read.item.view->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC td{};
    texture->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    OwnWorkScope own; // our copy is not the game's work
    if (FAILED(device->CreateTexture2D(&td, nullptr, &read.staging)) || !read.staging) return "no staging copy (memory or format)";
    ctx->CopyResource(read.staging.Get(), texture.Get());
    read.since_frame = frame;
    return nullptr;
}

// Present thread: reads the finished copy without waiting. 0 = not ready yet (same copy, ask again
// next Present), 1 = `job.body` holds every level, -1 = failed (`job.why`).
int FinishRead(DumpRead& read, DumpJob& job)
{
    const MaterialTextureFacts& f = job.facts;
    uint32_t block = 0, texel = 0;
    na::materials::TexSubLayout(f.view_format, block, texel);
    ComPtr<ID3D11Device> device;
    read.staging->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    OwnWorkScope own;
    job.body.clear();
    for (uint32_t mip = 0; mip < f.mips; ++mip) {
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = ctx->Map(read.staging.Get(), mip, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return 0; // nothing kept: levels already copied are read again when ready
        if (FAILED(hr) || !m.pData) {
            job.why = "a level could not be read back";
            return -1;
        }
        const uint32_t w = (std::max)(1u, f.width >> mip), ht = (std::max)(1u, f.height >> mip);
        const uint32_t row = block ? ((w + 3) / 4) * block : w * texel, rows = block ? (ht + 3) / 4 : ht;
        const bool fits = m.RowPitch >= row;
        if (fits) {
            if (!mip) job.body.reserve(static_cast<size_t>(ChainBytes(f.width, f.height, f.mips, f.view_format)));
            for (uint32_t r = 0; r < rows; ++r) {
                const auto* p = static_cast<const uint8_t*>(m.pData) + size_t(r) * m.RowPitch;
                job.body.insert(job.body.end(), p, p + row);
            }
        }
        ctx->Unmap(read.staging.Get(), mip);
        if (!fits) {
            job.why = "a level's row layout was not the expected one";
            return -1;
        }
    }
    return 1;
}

// Present thread. Phase 1 ends after its frames (or when enough is noted): the most used within the
// budget are kept. Phase 2 has one staging copy in flight: it is queued on one Present and read on a
// later one with DO_NOT_WAIT, so the GPU is never waited for here; the bytes (memory copies only)
// then go to the worker thread, which hashes and writes them.
void DumpOnPresent(uint64_t frame)
{
    const int phase = g_dump_phase.load(std::memory_order_acquire);
    if (!phase) return;
    Dump& d = D();
    if (phase == 1) {
        std::vector<DumpItem> dropped; // released after the lock
        {
            std::lock_guard<std::mutex> l(d.mu);
            if (frame < d.until_frame && d.queue.size() < kDumpCollectMax) return;
            d.noted = d.queue.size();
            // Least used first, so the next texture to read is taken from the back.
            std::sort(d.queue.begin(), d.queue.end(), [](const DumpItem& a, const DumpItem& b) {
                return a.entry->scene_binds.load(std::memory_order_relaxed) < b.entry->scene_binds.load(std::memory_order_relaxed);
            });
            std::vector<DumpItem> keep;
            uint64_t bytes = 0;
            d.over_budget = 0;
            for (auto it = d.queue.rbegin(); it != d.queue.rend(); ++it) {
                const MaterialTextureFacts& f = it->entry->facts;
                const uint64_t size = ChainBytes(f.width, f.height, f.mips, f.view_format);
                if (keep.size() < kDumpTextures && bytes + size <= kDumpBytes) {
                    bytes += size;
                    keep.push_back(std::move(*it));
                } else {
                    ++d.over_budget;
                    dropped.push_back(std::move(*it));
                }
            }
            std::reverse(keep.begin(), keep.end());
            d.queue = std::move(keep);
            d.finishing = false;
            g_dump_phase.store(2, std::memory_order_release);
        }
        dropped.clear();
        std::thread(&DumpWorker, d.dir, d.noted, d.over_budget).detach();
        return;
    }
    if (d.finishing) return; // the worker ends the request
    if (d.read) {
        DumpJob job = JobOf(d.read->item);
        int state = FinishRead(*d.read, job);
        if (!state && frame - d.read->since_frame > kDumpReadFrames) {
            job.why = "the copy was not ready within the allowed frames";
            state = -1;
        }
        if (!state) return;
        if (state < 0) job.body.clear();
        d.read.reset(); // lets go of the game's view and the staging copy
        HandToWorker(d, std::move(job));
        return;
    }
    if (!d.queue.empty()) {
        auto read = std::make_unique<DumpRead>();
        read->item = std::move(d.queue.back());
        d.queue.pop_back();
        if (const char* why = StartRead(*read, frame)) {
            DumpJob job = JobOf(read->item);
            job.why = why;
            HandToWorker(d, std::move(job));
        } else {
            d.read = std::move(read);
        }
        return;
    }
    DumpJob last;
    last.last = true;
    d.finishing = true;
    HandToWorker(d, std::move(last));
}

} // namespace

bool RequestTextureDump(const wchar_t* dir)
{
    if (!dir || !*dir || !g_installed.load(std::memory_order_relaxed)) return false;
    if (g_dump_phase.load(std::memory_order_relaxed) != 0) return false; // one at a time
    {
        // Lock order everywhere: the entry table, then the dump. Never both here.
        State& s = S();
        std::shared_lock<std::shared_mutex> sl(s.mu);
        for (const auto& kv : s.entries) kv.second->dump_taken.store(false, std::memory_order_relaxed);
    }
    Dump& d = D();
    {
        std::lock_guard<std::mutex> l(d.mu);
        if (g_dump_phase.load(std::memory_order_relaxed) != 0) return false;
        d.dir = dir;
        d.until_frame = g_frame.load(std::memory_order_relaxed) + kDumpCollectFrames;
        d.noted = d.over_budget = 0;
        g_dump_phase.store(1, std::memory_order_relaxed);
    }
    na::eventlog::Emit("native11_texture_dump", "requested frames=%llu", static_cast<unsigned long long>(kDumpCollectFrames));
    return true;
}

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
    ok = ok && na::hooking::PrepareVTableSlot(dev, kCreateTexture2D, reinterpret_cast<void*>(&CreateTexture2D_d),
                                              reinterpret_cast<void**>(&g_create_texture));
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
    if (ok) LoadMaterialCache(nullptr); // manifests only; an absent folder leaves the loader idle
    return ok;
}

ID3D11ShaderResourceView* const* SwapTextures(ID3D11DeviceContext* c, PsShadow& shadow, unsigned start, unsigned count,
                                              ID3D11ShaderResourceView* const* views, ID3D11ShaderResourceView** scratch)
{
    if (!views || !count || start >= kSlots || count > kSlots - start || !g_installed.load(std::memory_order_relaxed))
        return views;
    const bool census = g_census_frame.load(std::memory_order_relaxed);
    const bool replace = g_enabled.load(std::memory_order_relaxed) && g_replaced.load(std::memory_order_relaxed) != 0;
    const bool dump = g_dump_phase.load(std::memory_order_relaxed) == 1 && !SceneHanded();
    if (!census && !replace && !dump) {
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
            if (dump && e.verdict == TextureVerdict::Candidate && !e.dump_taken.exchange(true, std::memory_order_relaxed)) {
                Dump& d = D();
                std::lock_guard<std::mutex> dl(d.mu);
                if (g_dump_phase.load(std::memory_order_relaxed) == 1 && d.queue.size() < kDumpCollectMax)
                    d.queue.push_back({ComPtr<ID3D11ShaderResourceView>(views[i]), it->second});
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
    DumpOnPresent(frame);
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

size_t PublishTextures(const TexturePublication* items, size_t count)
{
    if (!items || !count) return 0;
    State& s = S();
    std::vector<ComPtr<ID3D11ShaderResourceView>> displaced; // released after the lock
    size_t taken = 0;
    {
        std::unique_lock<std::shared_mutex> l(s.mu);
        for (size_t i = 0; i < count; ++i) {
            const TexturePublication& p = items[i];
            if (!p.original || !p.replacement || p.original == p.replacement.Get()) continue;
            const auto it = s.entries.find(p.original);
            if (it == s.entries.end() || it->second->serial != p.serial) continue; // that view is gone
            Entry& e = *it->second;
            ++taken;
            if (e.replacement == p.replacement && !e.pending_set) continue; // already visible as asked
            displaced.push_back(std::move(e.pending));
            e.pending = p.replacement;
            e.pending_bytes = p.bytes;
            if (!e.pending_set) {
                e.pending_set = true;
                s.pending.push_back(p.original);
                g_pending.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    return taken;
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
    t.dump_written = g_dump_written.load();
    t.dump_failed = g_dump_failed.load();
    t.dump_active = g_dump_phase.load() != 0;
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
                         "swapped=%llu swapped_deferred=%llu returned_original=%llu generation=%llu dump{active=%d written=%llu skipped=%llu}}",
                         t.installed ? 1 : 0, t.enabled ? 1 : 0, static_cast<unsigned long long>(t.views),
                         static_cast<unsigned long long>(t.candidates), v(TextureVerdict::RenderedTo), v(TextureVerdict::Rewritten),
                         v(TextureVerdict::DataFormat), v(TextureVerdict::TooSmall), v(TextureVerdict::NoMips),
                         v(TextureVerdict::PartialView), v(TextureVerdict::Cube), v(TextureVerdict::Array),
                         v(TextureVerdict::Multisampled), v(TextureVerdict::NotTexture2D),
                         static_cast<unsigned long long>(g_overflow.load()), static_cast<unsigned long long>(t.live),
                         static_cast<unsigned long long>(t.retired), static_cast<unsigned long long>(t.replaced),
                         static_cast<unsigned long long>(t.replaced_bytes), static_cast<unsigned long long>(t.pending),
                         static_cast<unsigned long long>(t.swapped), static_cast<unsigned long long>(t.swapped_deferred),
                         static_cast<unsigned long long>(t.returned_original), static_cast<unsigned long long>(t.generation),
                         t.dump_active ? 1 : 0, static_cast<unsigned long long>(t.dump_written),
                         static_cast<unsigned long long>(t.dump_failed));
}

} // namespace na::native11
