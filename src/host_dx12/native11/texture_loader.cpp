// Native DX11 route: material cache loader (see texture_loader.hpp).
#include "host_dx12/native11/texture_loader.hpp"

#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/native11/input_provider.hpp"
#include "host_dx12/native11/texture_policy.hpp"
#include "host_dx12/native11/textures.hpp"
#include "materials/cache_index.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace na::native11 {
namespace {

using Microsoft::WRL::ComPtr;
using na::materials::CacheIndex;
using na::materials::TextureShape;

constexpr uint64_t kBudgetBytes = 1536ull << 20;  // prepared maps held at once (agreed with the preparer; headroom-limited later)
constexpr uint64_t kHashBacklogBytes = 256ull << 20; // creation data copied and not yet digested
constexpr size_t kManifestMax = 4u << 20;
constexpr size_t kEntriesMax = 4096;

// {8c1e5a02-6d37-4f9b-a1e4-27b03c9d5e61}: a game texture's digest note, by the lifetime of its private data.
const GUID kNoteGuid = {0x8c1e5a02, 0x6d37, 0x4f9b, {0xa1, 0xe4, 0x27, 0xb0, 0x3c, 0x9d, 0x5e, 0x61}};

struct WaitingView {
    ID3D11ShaderResourceView* view; // an address only: published by serial, never dereferenced here
    uint64_t serial;
    uint32_t view_format;
};

// Lives in a game texture's private data (and in its digest job): what the loader knows of it.
struct Note final : IUnknown {
    std::atomic<ULONG> refs{1};
    std::mutex mu;
    TextureShape shape;
    uint64_t index_generation = 0;
    bool done = false;                                       // digests are final
    std::vector<std::pair<uint32_t, std::string>> digests;   // view format -> digest
    std::vector<WaitingView> waiting;                        // views created before the digest was known
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
        if (!left) delete this;
        return left;
    }
};

struct MapState {
    ComPtr<ID3D11ShaderResourceView> replacement; // created by the loader's thread
    uint64_t bytes = 0;
    bool requested = false, failed = false;
    std::vector<WaitingView> views; // matched game views not yet published
};
struct EntryState {
    std::vector<MapState> maps; // by the record's map index
    size_t needed = 0;          // qualified maps: all must be matched and created before any is shown
    bool shown = false, counted_waiting = false, refused = false;
};

struct Job {
    enum class Kind { Digest, Load } kind = Kind::Digest;
    uint64_t index_generation = 0;
    // Digest
    ComPtr<Note> note;
    uint32_t resource_format = 0, width = 0, height = 0, mips = 0;
    std::vector<uint32_t> view_formats;
    std::vector<uint8_t> body;      // creation data, tightly packed in the wanted layout
    uint32_t block = 0, texel = 0;  // that layout
    // Load
    ComPtr<ID3D11Device> device;
    size_t record = 0, map = 0;
};

// Never destroyed: the worker and game threads may outlive this module's statics.
struct Loader {
    std::mutex mu; // index, entries, generation
    std::shared_ptr<const CacheIndex> index;
    std::vector<std::wstring> folders; // by record
    std::vector<EntryState> entries;   // by record
    uint64_t generation = 0;
    uint64_t loaded_bytes = 0;
    std::mutex work_mu;
    std::condition_variable work_cv;
    std::deque<Job> work;
    bool worker_started = false;
};
Loader& Ldr()
{
    static Loader* l = new Loader;
    return *l;
}

std::atomic<bool> g_active{false}; // an index with findable maps is loaded
std::atomic<uint64_t> g_entries{0}, g_findable{0}, g_refused{0}, g_unreadable{0};
std::atomic<uint64_t> g_hashed{0}, g_hash_bytes{0}, g_no_data{0}, g_backlog_skipped{0}, g_backlog_bytes{0};
std::atomic<uint64_t> g_matched{0}, g_loaded{0}, g_failed{0}, g_shown{0}, g_waiting{0}, g_over_budget{0}, g_loaded_bytes{0};

bool ReadWhole(const std::wstring& path, size_t max, std::vector<uint8_t>& out)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
    bool ok = _fseeki64(f, 0, SEEK_END) == 0;
    const long long size = ok ? _ftelli64(f) : -1;
    ok = ok && size >= 0 && static_cast<unsigned long long>(size) <= max && _fseeki64(f, 0, SEEK_SET) == 0;
    if (ok) {
        out.resize(static_cast<size_t>(size));
        ok = out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
    }
    std::fclose(f);
    return ok;
}

std::string Narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

void Post(Job&& job)
{
    Loader& l = Ldr();
    {
        std::lock_guard<std::mutex> g(l.work_mu);
        l.work.push_back(std::move(job));
    }
    l.work_cv.notify_one();
}

// With Loader::mu held. A game view whose texture is the source of (record, map).
// Fills `load` when the map's file has to be read, and `publish` with what may be shown now.
void Matched(Loader& l, ID3D11Device* device, const CacheIndex::Hit& hit, const WaitingView& view, std::vector<Job>& load,
             std::vector<TexturePublication>& publish)
{
    EntryState& e = l.entries[hit.record];
    MapState& m = e.maps[hit.map];
    g_matched.fetch_add(1, std::memory_order_relaxed);
    if (e.refused || m.failed) return;
    if (e.shown) { // the unit is already visible: this view joins it
        publish.push_back({view.view, view.serial, m.replacement.Get(), m.bytes});
        return;
    }
    m.views.push_back(view);
    if (!e.counted_waiting) {
        e.counted_waiting = true;
        g_waiting.fetch_add(1, std::memory_order_relaxed);
    }
    if (m.requested) return;
    // The entry's memory is claimed on its first match, for all its qualified maps at once.
    bool first = true;
    for (const MapState& other : e.maps) first = first && !other.requested;
    if (first) {
        uint64_t bytes = 0;
        const na::materials::Record& r = l.index->records()[hit.record];
        for (size_t i = 0; i < r.maps.size(); ++i)
            if (na::materials::Qualified(r.maps[i]) == nullptr) bytes += r.maps[i].mips.back().offset + r.maps[i].mips.back().byte_size;
        if (l.loaded_bytes + bytes > kBudgetBytes) {
            e.refused = true;
            g_over_budget.fetch_add(1, std::memory_order_relaxed);
            g_waiting.fetch_sub(1, std::memory_order_relaxed);
            na::eventlog::Emit("native11_materials", "entry=%s not loaded why=\"memory budget reached\" held=%llu entry_bytes=%llu budget=%llu",
                               r.entry_key.c_str(), static_cast<unsigned long long>(l.loaded_bytes),
                               static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(kBudgetBytes));
            return;
        }
        l.loaded_bytes += bytes;
        g_loaded_bytes.store(l.loaded_bytes, std::memory_order_relaxed);
    }
    m.requested = true;
    Job job;
    job.kind = Job::Kind::Load;
    job.index_generation = l.generation;
    job.device = device;
    job.record = hit.record;
    job.map = hit.map;
    load.push_back(std::move(job));
}

// With Loader::mu held: shows the entry when every qualified map has a view and a created texture.
void ShowIfComplete(Loader& l, size_t record, std::vector<TexturePublication>& publish)
{
    EntryState& e = l.entries[record];
    if (e.shown || e.refused) return;
    size_t ready = 0;
    for (const MapState& m : e.maps) ready += (m.replacement && !m.views.empty()) ? 1u : 0u;
    if (ready < e.needed) return;
    for (MapState& m : e.maps) {
        for (const WaitingView& v : m.views) publish.push_back({v.view, v.serial, m.replacement.Get(), m.bytes});
        m.views.clear();
    }
    e.shown = true;
    g_shown.fetch_add(1, std::memory_order_relaxed);
    if (e.counted_waiting) g_waiting.fetch_sub(1, std::memory_order_relaxed);
}

void Digest(Job& job)
{
    Loader& l = Ldr();
    g_backlog_bytes.fetch_sub(job.body.size(), std::memory_order_relaxed);
    std::vector<std::pair<uint32_t, std::string>> digests;
    for (uint32_t view_format : job.view_formats) {
        uint32_t block = 0, texel = 0;
        // The copy was laid out for one block/texel size; a view format of another layout has no match.
        if (!na::materials::TexSubLayout(view_format, block, texel) || block != job.block || texel != job.texel) continue;
        na::materials::TexSubHeader h;
        h.resource_format = job.resource_format;
        h.view_format = view_format;
        h.width = job.width;
        h.height = job.height;
        h.mip_count = job.mips;
        na::materials::TextureSubresourcesV1 d(h);
        size_t at = 0;
        for (uint32_t mip = 0; mip < job.mips && d.ok(); ++mip) {
            const uint32_t w = (std::max)(1u, job.width >> mip), ht = (std::max)(1u, job.height >> mip);
            const uint32_t row = block ? ((w + 3) / 4) * block : w * texel, rows = block ? (ht + 3) / 4 : ht;
            if (at + size_t(row) * rows > job.body.size()) break;
            d.AddLevel(0, mip, job.body.data() + at, row);
            at += size_t(row) * rows;
        }
        std::string name = d.Finish();
        if (!name.empty()) digests.emplace_back(view_format, std::move(name));
    }
    g_hashed.fetch_add(1, std::memory_order_relaxed);
    g_hash_bytes.fetch_add(job.body.size(), std::memory_order_relaxed);
    std::vector<WaitingView> waiting;
    {
        std::lock_guard<std::mutex> g(job.note->mu);
        job.note->digests = digests;
        job.note->done = true;
        waiting.swap(job.note->waiting);
    }
    if (waiting.empty()) return;
    std::vector<Job> load;
    std::vector<TexturePublication> publish;
    {
        std::lock_guard<std::mutex> g(l.mu);
        if (l.generation != job.index_generation || !l.index) return;
        for (const WaitingView& v : waiting)
            for (const auto& d : digests) {
                CacheIndex::Hit hit;
                if (d.first == v.view_format && l.index->Find(d.second, job.note->shape, v.view_format, hit))
                    Matched(l, job.device.Get(), hit, v, load, publish);
            }
    }
    if (!publish.empty()) PublishTextures(publish.data(), publish.size());
    for (Job& j : load) Post(std::move(j));
}

void Load(Job& job)
{
    Loader& l = Ldr();
    na::materials::Map map;
    std::wstring folder;
    std::string entry_key;
    {
        std::lock_guard<std::mutex> g(l.mu);
        if (l.generation != job.index_generation || !l.index) return;
        const na::materials::Record& r = l.index->records()[job.record];
        map = r.maps[job.map];
        entry_key = r.entry_key;
        folder = l.folders[job.record];
    }
    const uint64_t file_bytes = map.mips.back().offset + map.mips.back().byte_size;
    std::vector<uint8_t> file;
    std::string why;
    ComPtr<ID3D11ShaderResourceView> view;
    if (!ReadWhole(folder + L"\\" + std::wstring(map.file.begin(), map.file.end()), static_cast<size_t>(file_bytes), file)) {
        why = "file missing, unreadable or larger than the manifest says";
    } else if (na::materials::CheckDds(file.data(), file.size(), map, why)) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = map.width;
        td.Height = map.height;
        td.MipLevels = static_cast<UINT>(map.mips.size());
        td.ArraySize = 1;
        td.Format = static_cast<DXGI_FORMAT>(map.dxgi_format);
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<D3D11_SUBRESOURCE_DATA> levels(map.mips.size());
        for (size_t i = 0; i < map.mips.size(); ++i) {
            levels[i].pSysMem = file.data() + map.mips[i].offset;
            levels[i].SysMemPitch = static_cast<UINT>(map.mips[i].row_pitch);
        }
        OwnWorkScope own; // these objects are ours: never judged, digested or replaced
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(job.device->CreateTexture2D(&td, levels.data(), &texture)) || !texture ||
            FAILED(job.device->CreateShaderResourceView(texture.Get(), nullptr, &view)) || !view)
            why = "the graphics device refused the texture (memory or format support)";
    }
    std::vector<TexturePublication> publish;
    {
        std::lock_guard<std::mutex> g(l.mu);
        if (l.generation != job.index_generation) return;
        EntryState& e = l.entries[job.record];
        MapState& m = e.maps[job.map];
        if (view) {
            m.replacement = view;
            m.bytes = file_bytes;
            g_loaded.fetch_add(1, std::memory_order_relaxed);
            ShowIfComplete(l, job.record, publish);
        } else {
            // One map of a unit cannot be made: the unit is never shown, and says why.
            m.failed = true;
            g_failed.fetch_add(1, std::memory_order_relaxed);
            if (!e.refused && !e.shown && e.counted_waiting) g_waiting.fetch_sub(1, std::memory_order_relaxed);
            e.refused = true;
        }
    }
    if (!view)
        na::eventlog::Emit("native11_materials", "entry=%s map=%s not loaded why=\"%s\"", entry_key.c_str(), map.id.c_str(), why.c_str());
    if (!publish.empty()) {
        const size_t taken = PublishTextures(publish.data(), publish.size());
        na::eventlog::Emit("native11_materials", "entry=%s shown views=%zu of %zu matched (the rest were released by the game meanwhile)",
                           entry_key.c_str(), taken, publish.size());
    }
}

void Worker()
{
    Loader& l = Ldr();
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> g(l.work_mu);
            l.work_cv.wait(g, [&] { return !l.work.empty(); });
            job = std::move(l.work.front());
            l.work.pop_front();
        }
        if (job.kind == Job::Kind::Digest) Digest(job);
        else Load(job);
    }
}

} // namespace

size_t LoadMaterialCache(const wchar_t* dir)
{
    std::wstring root;
    if (dir && *dir) {
        root = dir;
    } else {
        wchar_t local[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        if (!n || n >= MAX_PATH) return 0;
        root = std::wstring(local) + L"\\NeuralAlias\\materials";
    }
    const uint64_t began = GetTickCount64();
    auto index = std::make_shared<CacheIndex>();
    std::vector<std::wstring> folders;
    uint64_t unreadable = 0;
    WIN32_FIND_DATAW found{};
    const HANDLE find = FindFirstFileW((root + L"\\*").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE) {
        // Entries are added in name order, so which of two entries for one game texture is kept
        // does not depend on the file system's listing order.
        std::vector<std::wstring> names;
        do {
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcslen(found.cFileName) == 64) names.emplace_back(found.cFileName);
        } while (names.size() < kEntriesMax && FindNextFileW(find, &found));
        FindClose(find);
        std::sort(names.begin(), names.end());
        for (const std::wstring& name : names) {
            const std::wstring folder = root + L"\\" + name;
            std::vector<uint8_t> text;
            na::materials::Record record;
            std::string why;
            if (!ReadWhole(folder + L"\\manifest.json", kManifestMax, text)) why = "manifest.json missing or unreadable";
            else if (na::materials::ReadManifest(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()), record, why) &&
                     record.entry_key != Narrow(name))
                why = "the folder is not named by the manifest's entry_key";
            if (!why.empty()) {
                ++unreadable;
                na::eventlog::Emit("native11_materials", "entry=%s not read why=\"%s\"", Narrow(name).c_str(), why.c_str());
                continue;
            }
            if (index->Add(std::move(record))) folders.push_back(folder);
        }
    }
    for (const CacheIndex::Refusal& r : index->refusals())
        na::eventlog::Emit("native11_materials", "entry=%s map=%s not used why=\"%s\"", r.entry_key.c_str(), r.map_id.c_str(), r.why.c_str());
    Loader& l = Ldr();
    const size_t entries = index->records().size();
    {
        std::lock_guard<std::mutex> g(l.mu);
        ++l.generation; // work in flight for the previous index is dropped when it lands
        l.entries.clear();
        l.entries.resize(entries);
        for (size_t i = 0; i < entries; ++i) {
            l.entries[i].maps.resize(index->records()[i].maps.size());
            l.entries[i].needed = index->QualifiedMaps(i);
        }
        l.folders = std::move(folders);
        l.loaded_bytes = 0;
        g_entries.store(entries);
        g_findable.store(index->findable());
        g_refused.store(index->refusals().size());
        g_unreadable.store(unreadable);
        g_waiting.store(0);
        g_loaded_bytes.store(0);
        g_active.store(index->findable() != 0, std::memory_order_release);
        l.index = std::move(index);
        if (!l.worker_started && g_active.load()) {
            l.worker_started = true;
            std::thread(&Worker).detach();
        }
    }
    na::eventlog::Emit("native11_materials", "cache read folder=\"%s\" entries=%zu findable_maps=%llu refused_maps=%llu unreadable=%llu ms=%llu",
                       Narrow(root).c_str(), entries, static_cast<unsigned long long>(g_findable.load()),
                       static_cast<unsigned long long>(g_refused.load()), static_cast<unsigned long long>(unreadable),
                       static_cast<unsigned long long>(GetTickCount64() - began));
    return entries;
}

void MaterialsOnTextureCreated(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& desc, const D3D11_SUBRESOURCE_DATA* initial,
                               ID3D11Texture2D* texture)
{
    if (!g_active.load(std::memory_order_acquire) || !texture || desc.ArraySize != 1 || desc.SampleDesc.Count != 1) return;
    Loader& l = Ldr();
    const TextureShape shape{static_cast<uint32_t>(desc.Format), desc.Width, desc.Height, desc.MipLevels};
    std::vector<uint32_t> wanted;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> g(l.mu);
        if (!l.index) return;
        wanted = l.index->WantedViews(shape);
        generation = l.generation;
    }
    if (wanted.empty()) return; // nearly every texture ends here
    if (!initial) {
        g_no_data.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint32_t block = 0, texel = 0;
    if (!na::materials::TexSubLayout(wanted[0], block, texel)) return;
    const uint64_t bytes = ChainBytes(desc.Width, desc.Height, desc.MipLevels, wanted[0]);
    if (g_backlog_bytes.load(std::memory_order_relaxed) + bytes > kHashBacklogBytes) {
        g_backlog_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Job job;
    job.kind = Job::Kind::Digest;
    job.index_generation = generation;
    job.resource_format = shape.resource_format;
    job.width = desc.Width;
    job.height = desc.Height;
    job.mips = desc.MipLevels;
    job.view_formats = std::move(wanted);
    job.block = block;
    job.texel = texel;
    job.device = device;
    job.body.reserve(static_cast<size_t>(bytes));
    for (uint32_t mip = 0; mip < desc.MipLevels; ++mip) {
        const uint32_t w = (std::max)(1u, desc.Width >> mip), ht = (std::max)(1u, desc.Height >> mip);
        const uint32_t row = block ? ((w + 3) / 4) * block : w * texel, rows = block ? (ht + 3) / 4 : ht;
        const D3D11_SUBRESOURCE_DATA& level = initial[mip];
        if (!level.pSysMem || level.SysMemPitch < row) { // rows the game did not lay out as expected: no identity
            g_no_data.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        for (uint32_t r = 0; r < rows; ++r) {
            const auto* p = static_cast<const uint8_t*>(level.pSysMem) + size_t(r) * level.SysMemPitch;
            job.body.insert(job.body.end(), p, p + row);
        }
    }
    Note* note = new Note;
    note->shape = shape;
    note->index_generation = generation;
    texture->SetPrivateDataInterface(kNoteGuid, note); // the texture and the job each hold it
    job.note.Attach(note);
    g_backlog_bytes.fetch_add(job.body.size(), std::memory_order_relaxed);
    Post(std::move(job));
}

void MaterialsOnViewCreated(ID3D11Resource* resource, ID3D11ShaderResourceView* view, uint64_t serial, const MaterialTextureFacts& facts)
{
    if (!g_active.load(std::memory_order_acquire) || !resource || !view) return;
    IUnknown* raw = nullptr;
    UINT size = sizeof(raw);
    if (FAILED(resource->GetPrivateData(kNoteGuid, &size, &raw)) || !raw) return; // not a texture of a prepared shape
    ComPtr<Note> note;
    note.Attach(static_cast<Note*>(raw));
    const WaitingView waiting{view, serial, facts.view_format};
    std::vector<std::pair<uint32_t, std::string>> digests;
    {
        std::lock_guard<std::mutex> g(note->mu);
        if (!note->done) {
            note->waiting.push_back(waiting); // matched by the loader's thread when the digest lands
            return;
        }
        digests = note->digests;
    }
    Loader& l = Ldr();
    ComPtr<ID3D11Device> device;
    view->GetDevice(&device);
    std::vector<Job> load;
    std::vector<TexturePublication> publish;
    {
        std::lock_guard<std::mutex> g(l.mu);
        if (l.generation != note->index_generation || !l.index) return;
        for (const auto& d : digests) {
            CacheIndex::Hit hit;
            if (d.first == facts.view_format && l.index->Find(d.second, note->shape, facts.view_format, hit))
                Matched(l, device.Get(), hit, waiting, load, publish);
        }
    }
    if (!publish.empty()) PublishTextures(publish.data(), publish.size());
    for (Job& j : load) Post(std::move(j));
}

MaterialStats GetMaterialStats()
{
    MaterialStats s;
    s.entries = g_entries.load();
    s.findable_maps = g_findable.load();
    s.refused_maps = g_refused.load();
    s.unreadable_entries = g_unreadable.load();
    s.textures_hashed = g_hashed.load();
    s.hash_bytes = g_hash_bytes.load();
    s.no_creation_data = g_no_data.load();
    s.hash_backlog_skipped = g_backlog_skipped.load();
    s.views_matched = g_matched.load();
    s.maps_loaded = g_loaded.load();
    s.maps_failed = g_failed.load();
    s.entries_shown = g_shown.load();
    s.entries_waiting = g_waiting.load();
    s.over_budget = g_over_budget.load();
    s.loaded_bytes = g_loaded_bytes.load();
    return s;
}

int FormatMaterialStatus(char* out, unsigned long n)
{
    const MaterialStats s = GetMaterialStats();
    const auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    return std::snprintf(out, n,
                         "native11_materials{entries=%llu findable_maps=%llu refused_maps=%llu unreadable=%llu textures_hashed=%llu "
                         "hash_bytes=%llu no_creation_data=%llu hash_backlog_skipped=%llu views_matched=%llu maps_loaded=%llu "
                         "maps_failed=%llu entries_shown=%llu entries_waiting=%llu over_budget=%llu loaded_bytes=%llu}",
                         u(s.entries), u(s.findable_maps), u(s.refused_maps), u(s.unreadable_entries), u(s.textures_hashed), u(s.hash_bytes),
                         u(s.no_creation_data), u(s.hash_backlog_skipped), u(s.views_matched), u(s.maps_loaded), u(s.maps_failed),
                         u(s.entries_shown), u(s.entries_waiting), u(s.over_budget), u(s.loaded_bytes));
}

} // namespace na::native11
