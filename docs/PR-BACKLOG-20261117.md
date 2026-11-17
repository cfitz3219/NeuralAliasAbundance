# Pull request consolidation, November 17, 2026

Cort asked to merge the open GitHub backlog where possible. The initial inventory contained 39 pull requests. The Route B consolidation branch now includes the native DX11/DX12 source through b36d261 and the newer GameCheck rules through e1b4723. The proposed main merge takes that branch at cc5079c. The only content conflict was the append in WORKLOG.md. Both journals are retained. All 38 GameCheck tests passed on the combined tree, and the newer GameCheck files match e1b4723.

| Disposition | Pull requests | Retained source or reason |
| --- | --- | --- |
| Merged into Route B | #133, #132, #134, #135, #144 | 85ff895, 3fe865d, 0d48716, bee514f, cc5079c. Includes camera recovery, GameCheck, native DX11 and Rome II work, and camera-independent regressions. |
| Older PR, exact head already in consolidated history | #84, #86, #87, #96, #97, #100, #123, #127, #130, #138 | Their heads are ancestors of cc5079c. The existing Route B policy holds injected upscaling and the translation bridge disabled by default. |
| Older PR, carried through other commits | #95, #98, #99, #101, #102, #104, #114, #119, #120, #128 | Compared patches, exact files, or explicit cherry-pick trailers. The remaining separate changes in #98, #99, #101 and #128 are historical notes. #102 is ebe081f. #114's report and two tools match. #119's two original patches are equivalent. #120 is 1dd8040. #128's runtime commits are ancestors of the consolidated line, and its final commit is a local preflight entry. |
| Rejected in live use | #79 | The user directed a return to 6f6b806. The abandoned stack and later alternatives are documented in docs/VARIANTS.md. |
| Preserve reports independently | #103, #139, #143 | The Intel/NVIDIA survey, original material proposal, and adaptive runtime audit are copied verbatim into this merge. The material proposal remains a proposal. The adaptive audit describes its stated baseline. |
| Retained separately | #83, #88, #89, #90, #91, #92, #93, #94 | Unmerged Anno experiments, render-small/display-mode work, independent jitter/HUD experiments or legacy API work. These PRs have unique changes outside the current consolidation. |
| Active or partially integrated | #142, #145 | #142 still has distinct material preparation and runtime changes beyond the earlier partial merges. #145 is Fable's active iteration for Resistance. |

The branches and commit history remain available. A PR merge records source integration. Game behavior continues to be attributed to the exact build that was run.
