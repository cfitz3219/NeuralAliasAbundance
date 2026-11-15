// Native DX11 route: material textures. Two jobs on the game's own D3D11 device:
//
//  1. Census. Every shader resource view the game creates is judged once (texture_policy.hpp) and
//     its pixel-shader binds are counted on one frame in sixteen, so a log can say which textures a
//     game draws its surfaces with, in what formats and sizes, and what larger versions would cost.
//  2. Swap. A prepared replacement view, published for one of the game's views, is bound in its
//     place at PSSetShaderResources. The game keeps seeing its own view: PSGetShaderResources hands
//     the original back. The game's files and its own resources are never changed.
//
// When a replacement becomes visible: Publish/Remove/SetTexturesEnabled take effect at the next
// Present (a CPU policy boundary), for every map published before it together. Views already bound
// are re-bound at the first draw after it, with no new bind by the game. A deferred command list
// recorded before that Present keeps what it recorded, for as long as the game replays it; the
// status line counts deferred swaps separately for that reason.
//
// Lifetime: the table of judged views holds no reference on a game object; a view's destruction is
// seen through its private data and retires its entry and replacement. A replacement chosen for a
// bind is referenced from the lookup until the real bind has taken D3D's own reference
// (SwapTextures .. EndTextureSwap, and inside RefreshTextures), so a thread recording a deferred
// list can be held up for any length of time without binding a freed view.
// The one reference taken on a game view is the bind reference D3D itself would have held: while a
// replacement sits in a slot in the game's view's place, that context's PsShadow keeps the game's
// view alive, taken on the game's own bind call. It is what the slot is restored from and what
// PSGetShaderResources answers with, so neither ever works from a pointer of unknown lifetime.
#pragma once

#include <d3d11.h>

#include <cstdint>

namespace na::native11 {

// Per context: for each pixel-shader slot where a replacement is bound, the game's own view.
// Owned by the provider's per-context state; touched only on that context's thread.
struct PsShadow {
    ID3D11ShaderResourceView* game[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    unsigned held = 0;
    PsShadow() = default;
    PsShadow(const PsShadow&) = delete;
    PsShadow& operator=(const PsShadow&) = delete;
    ~PsShadow() { Clear(); }
    void Set(unsigned slot, ID3D11ShaderResourceView* view) // null clears the slot
    {
        if (game[slot] == view) return;
        if (view) {
            view->AddRef();
            ++held;
        }
        if (ID3D11ShaderResourceView* old = game[slot]) {
            game[slot] = view;
            --held;
            old->Release();
        } else {
            game[slot] = view;
        }
    }
    void Clear() // the context's state was reset (ClearState, a list finished or executed without restore)
    {
        if (!held) return;
        for (auto*& kept : game)
            if (kept) {
                ID3D11ShaderResourceView* old = kept;
                kept = nullptr;
                old->Release();
            }
        held = 0;
    }
};

// Hooks the game's device (ID3D11Device*) CreateShaderResourceView and its contexts'
// PSGetShaderResources. Idempotent. The bind itself is hooked by the provider, which calls
// SwapTextures. Install before the game creates its textures.
bool InstallTextures(void* device);

// From the provider's PSSetShaderResources detour, before forwarding. Returns `views`, or `scratch`
// (at least `count` entries) holding the views to bind instead. `shadow` is that context's.
ID3D11ShaderResourceView* const* SwapTextures(ID3D11DeviceContext* c, PsShadow& shadow, unsigned start, unsigned count,
                                              ID3D11ShaderResourceView* const* views, ID3D11ShaderResourceView** scratch);
// After the real bind of what SwapTextures returned: lets go of the replacements it referenced.
void EndTextureSwap(unsigned count, ID3D11ShaderResourceView* const* views, ID3D11ShaderResourceView* const* bound);
// The provider's per-context shadow for a context of the game's device (null otherwise). Implemented
// by the provider; used by the PSGetShaderResources hook.
PsShadow* TextureShadowOf(ID3D11DeviceContext* c);

// Changes whenever what should be bound may have changed; the provider compares it per context at
// each draw and calls RefreshTextures with that context's original PSSetShaderResources.
uint64_t TexturePolicyKey();
using TextureSetFn = void (__stdcall*)(ID3D11DeviceContext*, unsigned, unsigned, ID3D11ShaderResourceView* const*);
void RefreshTextures(ID3D11DeviceContext* c, PsShadow& shadow, TextureSetFn original_set);

// From the provider's Present handler.
void TexturesOnPresent();

// Publication (the runtime's cache loader, and the fixture). `original` must be a view the game
// created after InstallTextures; `replacement` is a view on the same device. False when the
// original is not known. Visible from the next Present. Several originals may share one replacement
// view (several views of one texture); `bytes` is then counted once per original.
bool PublishTexture(ID3D11ShaderResourceView* original, ID3D11ShaderResourceView* replacement, uint64_t bytes);
bool RemoveTexture(ID3D11ShaderResourceView* original);
void SetTexturesEnabled(bool on); // one switch for every replacement; the census is not affected

struct TextureStats {
    uint64_t views = 0;            // game views judged
    uint64_t candidates = 0;       // of those: may take a replacement
    uint64_t live = 0;             // entries now (views not yet destroyed)
    uint64_t retired = 0;          // entries dropped because the game destroyed the view
    uint64_t replaced = 0;         // views with a visible replacement
    uint64_t pending = 0;          // published, waiting for the next Present
    uint64_t swapped = 0;          // binds forwarded with a replacement (immediate context)
    uint64_t swapped_deferred = 0; // the same on deferred contexts (recorded; see the header note)
    uint64_t returned_original = 0;// PSGetShaderResources answers turned back to the game's view
    uint64_t replaced_bytes = 0;   // memory of visible replacements, as their publisher declared it
    uint64_t generation = 0;
    bool installed = false, enabled = true;
};
TextureStats GetTextureStats();
int FormatTextureStatus(char* out, unsigned long n); // "native11_textures{...}"

} // namespace na::native11
