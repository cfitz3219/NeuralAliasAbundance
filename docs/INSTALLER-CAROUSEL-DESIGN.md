# Installer game library and feature selection

Recorded by Astra, 2026-10-29, from Cort's design request and the following discussion.

**Status: design recorded only. Implementation is deferred at Cort's explicit request.**
This document does not authorize a UI build, installer changes, game deployment, or a new review cycle. No implementation duration is committed; documenting this requires no user attendance.

## Cort's requirements

### Upright artwork carousel

- Display the game library as artwork cards standing upright.
- The selected game's card faces the viewer directly.
- Cards to its left have their faces angled toward the right. Cards to its right have their faces angled toward the left.
- The arrangement exposes the artwork of games behind the foreground cards, down to roughly three or four cards back on each side.
- Cards farther back become more faded/darker, emphasizing depth and the selected game.
- Place two navigation arrows beneath the artwork: one left and one right.
- Pressing an arrow moves the artwork to the next game. Cards should smoothly shuffle across into their new positions, like swiping on a phone, rather than jump abruptly.

### Feature selection

Provide an easy-to-see place to check which components the user wants installed. There is one checkbox for each:

1. DLSS
2. DLSS 5 Neural Rendering
3. Frame Gen

The user can select the desired combination. Do not install unselected optional features.

### Large installation action bar

When a game is selected, show a large bar-shaped button whose colour communicates whether it works with NeuralAlias:

| Compatibility | Colour | Button text | Pointer-hover behaviour |
|---|---|---|---|
| Works | Green | **Go** | Normal installation action when at least one feature is checked. |
| Cannot work | Red | **Unable to Install*** | Display why NeuralAlias cannot work with that game. |
| Works with caveats | Yellow | **Go*** | Display the applicable caveats. |

If no feature checkbox is selected, the green Go bar turns gray when the pointer moves over it, and a popup says **Please make a selection**. Preserve this requested hover behaviour; do not silently replace it with a different design.

### In-game overlay

- The overlay remains largely the same as the existing overlay.
- Options for components not selected for installation remain visible but grayed out.
- Those unselected components are not installed.

## Astra's suggestions, recorded separately

These were proposed in response to Cort's design. They are documented for the future build, not represented as requirements originally specified by Cort.

1. **Animate both movement and rotation.** Side cards slide and rotate toward the front-facing position while the previous selection moves aside. This implements the intended phone-swipe feel.
2. **Additional navigation:** support dragging/swiping and keyboard left/right arrows using the same animation as the buttons.
3. **Selection-aware compatibility:** compute the action-bar state for the selected features, not just a single permanent game-wide label. For example, DLSS and Neural Rendering might work while Frame Gen lacks a working route. Selecting Frame Gen should explain that particular limitation rather than implying the whole game is incompatible.
4. **No-selection action:** clicking Go with nothing selected does nothing and explains **Please make a selection**. Apply this protection to a yellow Go as well; the user's explicit green hover appearance remains as described above.
5. **Persistent explanations:** make the asterisk clickable so caveats and refusal reasons can be read without holding the pointer still and can be accessed on a touchscreen.
6. **Checking state:** while compatibility is being determined, show **Checking compatibility…**, rather than prematurely declaring the game unsupported.
7. **Overlay labels:** label grayed-out features **Not installed**, distinguishing that condition from an installed feature that is temporarily unavailable.
8. **Shared dependencies:** install the selected features and the shared components they require; this does not mean installing every optional effect.
9. **Supported-state explanation:** a green Go may explain that it is ready to install the selected features.

The intended interaction is: choose a game, check desired features, then Go.

## Related installer suggestions from the preceding research

Retain Cort's visual concept. The following are reuse candidates, not approved dependencies or implementation work:

- Study/reuse the game-library discovery in [DLSS5oneclick library.rs](https://github.com/faisalkindi/DLSS5oneclick/blob/main/src/library.rs), including launcher libraries, artwork and executable hints.
- Study its [game-folder inspection](https://github.com/faisalkindi/DLSS5oneclick/blob/main/src/game.rs) for executable architecture and installed-component detection.
- Adapt its [plain-language log diagnostics approach](https://github.com/faisalkindi/DLSS5oneclick/blob/main/src/diagnose.rs) to NeuralAlias's own runtime evidence.
- Use consistent typography, spacing, panel colours and status colours; its [theme implementation](https://github.com/faisalkindi/DLSS5oneclick/blob/main/src/theme.rs) is a reference, not a replacement for Cort's artwork-carousel concept.
- Keep routine controls simple, with advanced controls tucked away. Suggested main actions were **Install, Configure, Diagnose and Restore**; the install action can use Cort's **Go** label.
- Show actual runtime status alongside the installed version, so files being installed is not presented as proof an effect is running.
- The installer repository's own code is [MIT-licensed](https://github.com/faisalkindi/DLSS5oneclick/blob/main/LICENSE); preserve required notices if reused. Downloaded components retain their separate licenses. Its Rust/egui desktop UI is distinct from the ReShade/RenoDX or OptiScaler in-game overlays it installs.

## Details deliberately left for the future implementation

No decision has been made about checkbox defaults, carousel wraparound at the first/last game, exact artwork dimensions, animation timing, or whether to use Rust/egui. The precise evidence used to determine feature compatibility also remains an implementation decision. These are not reasons to expand this design into another rendering project.

No code, mockup, installer, or game change is part of this documentation task.
