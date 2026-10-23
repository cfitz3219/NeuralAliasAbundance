# Universal DX12 architecture: Argon audit response

2026-10-23. Cort directs us to pursue universal DX12 injection as feasible and
remove framing that dismisses that goal. The attached Argon audit is technical
review input; its individual implementation directives are proposals to evaluate,
not independent authorization to change frozen interfaces or reorder the project.
The goal is automatic acquisition and synthesis across games. A current coverage
gap is an engineering task, not a conclusion about the project's feasibility.

## What changes now

The acquisition roadmap now specifies candidate enumeration, convention search,
GPU warp verification, camera estimation, sky motion, flow fusion and automatic
profiles. Missing inputs must trigger continued acquisition and experiments.
A submission validator is the last check on a particular frame, not the discovery
algorithm. Keep acquisition active when a candidate is rejected. Track reasons,
next experiments and progress rather than ending discovery at the first missing
signal. The R-101 concurrency regression and R-104 identity clarification are
already pushed in 7ad38c7 and 15f75ba.

## C-001 proposal: DX12 upload slicing (C9/A6)

The existing UploadCache limits entries to 64 KiB and rejects reads while mapped.
Those semantics do not implement persistent DX12 upload-ring acquisition. Add a
separate DX12 slicer so the original cache's snapshot semantics remain explicit.
Track session/resource identity, GPU virtual-address interval, resource size,
mapping range/lifetime, binding offsets and command-list recording generation.
Resolve root CBVs and descriptor CBVs to bounded slices, including persistent
multi-megabyte allocations. Copy only requested ranges at an observed submission
boundary into a bounded transport ring; never copy the whole allocation by default.

A submission callback does not itself synchronize application CPU writers.
Snapshots must retain `app-sync-unknown` unless there is additional evidence;
validate stable candidate content across observations. Handle overlapping/aliased
addresses, release/unmap, offset overflow, descriptor versions and lost events.
Fixture acceptance: persistent 4–64 MiB rings, two CBV slices, changing offsets,
concurrent writes, bounds failures, resource reuse and multiple submissions.
The audit's 99% rejection estimate and Anno-specific layout are not measured
facts; instrument actual binding coverage to quantify the gap.

## M-002 proposal: motion synthesis (C10/A5/A6)

Separate camera-relative residual flow from raw de-jittered flow in the input
contract. Propose raw-flow provenance as a new value after ABI/interface review;
never feed residuals into a path expecting complete motion. Add background-motion
inputs sufficient to express rotation-only reprojection. Clear depth alone must
not label all clouds, transparent effects and unrendered pixels as static sky.

Construct disagreement and visibility masks from depth and exposure-aware luma
warps; stabilize analytical motion on verified static geometry and qualify flow
for independent motion. Test camera-relative origin deltas and robust camera
fitting from depth/flow, including degeneracy and moving-scene contamination.
Acceptance: camera translation/rotation, sky rotation, moving clouds/objects,
repeated textures, exposure changes, disocclusion, camera acquisition/reacquisition,
and independently measured per-region motion error. Do not describe noisy depth
or flow estimates as exact, or low photometric error as proof of a unique solution.

## Frame contract proposal (A3/C6; FG scheduling coordinated separately)

Replace the fixed 0.5% aspect-ratio assumption with backend-qualified per-axis
extent/subrect and motion-scale rules. Test 8-aligned inputs, horizontal-only DRS,
resize and real backend limits. A blanket 0.5–2.0 aspect window would still encode
an arbitrary gate; establish supported ranges from the actual runtime contract.

Recovered host frame-generation code exists, while the core contract still rejects
FG unconditionally. Describe this as an integration gap. Propose explicit backend
capability and required-input validation, including scheduling, presentation,
resource lifetime and reset behavior. Merely accepting color/depth/motion does
not connect the backend. MASTER-PLAN currently schedules FG after J2; discuss its
integration through COMMS before changing that sequencing.

## Coordination and validation

Request Claude's response to C-001/M-002 and the frame-contract proposal in COMMS.
Implement agreed changes in their owning task branches with meaningful regressions.
No production code, frozen ABI or existing tests are changed by this documentation
revision. This records an active engineering strategy, not a runtime completion
claim. Original recovery uploads retain their provenance and hashes; current
instructions and roadmap supersede their historical planning language.
