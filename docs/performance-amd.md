# AMD performance work

This branch starts from `83628a436e58a52814a5322dd5ba91d4d86479d2` and adapts the remaining
performance proposals to that revision's renderer. The development machine is a Windows PC with
a Ryzen 7 7800X3D and Radeon RX 9070 XT. Astro Bot gameplay and a 60 fps result have not been
validated by this patch set.

## Implemented changes

- **Predicate synchronization (PR #702):** bool predicates skip submission when their bytes have
  no GPU producer. Buffer producers download the required range; image producers and deferred
  image writebacks retain conservative GPU and priority-operation completion. Overlapping pending
  image writebacks are tracked independently until their backing-memory writes finish.
- **BDA discovery (PR #562):** CPU-dirty region hints narrow the search for cached buffers requiring
  uploads. The per-page dirty state remains authoritative. Region creation, CPU writes, mapping,
  and buffer registration publish hints; concurrent publications are preserved across consumption.
  Inconsistent ownership falls back to the legacy walk. A 64-word summary (one bit per hint word)
  lets a pass skip clean hint words, so an idle pass reads 64 words instead of 4,096.
- **Windows address waits (PR #618):** native `WaitOnAddress` parks stack-allocated waiter records.
  Sharded locks replace the shared allocating registry. Explicit wakes remain visible during signal
  callbacks, and counted wakes are bounded by registered waiters rather than the requested count.
- **Shader replay (PR #718):** a versioned, checksummed journal records shader code and compile
  metadata, without runtime pointers or buffer addresses. Warm startup replays the journal before
  the first shader lookup. Runtime resources are rematerialized from the current guest state.
- **Readback and pressure handling (PR #638):** downloads exceeding the 64 MiB staging ring use
  dedicated staging allocations retained through publication. Pressure collection retries image
  lookup after reclaiming eligible textures. Dirty victims remain protected until their writebacks
  complete. Overlap replacement also keeps the old image registered and write-protected until
  pending publication completes; the wait releases the texture lock and does not run unrelated
  deferred resource-destruction callbacks. Safely downloadable depth planes retain their eviction
  path when their stencil companions are known clean; dirty or unknown stencil ownership, metadata,
  and conflicting alias cases remain resident.
- **Mesh dispatch limits (PR #793):** oversized mesh draws are sliced to the host's per-axis and
  total limits. Primitive and instance offsets survive slicing. The normal draw path visits slices
  without allocating a vector.
- **Frame reporting:** the window reports fresh game frames separately from presentation rate,
  excludes reused frames from game FPS, and updates the title at most once per second.
- **AMD attachment feedback:** layout support is independent from dynamic feedback-state support.
  Drivers exposing the layout extension alone use static pipeline flags keyed by actual overlapping
  attachment reads/writes. Drivers supporting dynamic state continue to reuse one pipeline variant.
  The tested RX 9070 XT driver exposes the layout extension but not the dynamic-state extension.
- **Scalar mask preservation:** 64-bit SAVEEXEC operations retain raw EXEC words independently of
  host-active lanes, while preserving the saved per-invocation predicate for later restores. The
  complete Radeon shader suite exposed loss of inactive bits through the previous ballot path;
  existing expected register values remain unchanged. Scalar VCC/EXEC branches use the complete
  guest-wave zero flags, with the upper half included only in wave64; per-lane VALU masking is
  unchanged.
- **FP16 fractional boundary:** finite `V_FRACT_F16` results stay below one before destination
  modifiers are applied. The test's previous rounded-one expectations contradicted the DX
  fractional range referenced by AMD's RDNA2 ISA, section 12.8. Edge tests now require the largest
  half below one, while separate modifier checks allow multiplication and saturation to reach one.
  References: [AMD RDNA2 ISA](https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture),
  [Microsoft frac](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-frac).
- **FP16 inline constants:** use the fixed architectural half value for `1/(2*pi)` (`0x3118`,
  RDNA2 ISA section 6.2/Table 20). It must not depend on the host GPU's conversion of the distinct
  single-precision inline constant. Packed selectors and source modifiers retain their behavior.
- **Guest-wave buffer indexing:** ADD_TID offsets wrap at the guest wave width, including wave32
  shaders executed in a host subgroup of 64. Specialized and dynamically selected buffers share
  the corrected lane calculation.
- **Windows regression repairs:** the filesystem test has a valid SDL entry-point signature and
  now exercises Winsock receive-flag translation. Blocking stream `PEEK | WAITALL` is emulated
  without passing that invalid combination to Winsock; partial arrivals, deadlines, EOF and
  cancellation retain the bytes already received. Socket mode and timeout state are shared by
  transport. Per-call `DONTWAIT` on an otherwise blocking Windows transport remains unsupported
  and returns `EOPNOTSUPP`; an explicitly nonblocking transport supports it.

The dense evaluator, retained resource snapshots, and replacement of the old SRT readability path
were already present at the base revision. Their historical PR improvements are not additional gains
from this branch.

## Ported upstream pull requests

These open upstream PRs were brought in as separate commits that keep their original authors.

- **#483, skip the GPU drain on non-GPU unmaps:** `RenderContext::UnmapMemory` returns early
  when a range never intersected the GPU-mapped set. Unmapping 64 KiB of CPU-only memory from a
  guest thread took 79 us with an idle GPU and 130 us with 64 MiB of fill work queued; it now takes
  0.10 us.
- **#749, bounded event waits:** the main loop wakes at least every half vblank, so queued
  main-thread callbacks such as the title update cannot stall presentation. Ported to SDL3. In a
  1,200-update SDL3 harness, the unbounded wait's worst case reached 1.53 s (1.96 s in an earlier
  run); the bounded wait's worst case was 8.0 ms.
- **#820, zero reused direct memory:** new allocations and pool expansions no longer expose a
  previous owner's bytes. A follow-up commit clears only ranges that were returned to the free list
  before, so fresh backing is not touched (and committed) at allocation.
- **#761, fragment helpers in ballots:** helper invocations no longer count in guest ballots or
  in the lane-activity ballot for swizzle/bpermute. The PR's MoltenVK test-harness modes were not
  carried over; its tests run on the full harness.
- **#795, GPU tiler constants:** the tiler shaders avoid `uvec4` specialization-constant selects
  that RADV miscompiled on the RX 9070 XT. The Windows driver already passed the tiler tests.

Reviewed but not ported: #506 (its texture-residency change would raise memory to the pressure
threshold, where eviction drains the GPU; read-only compute barriers almost never apply; block
descriptor reads are already in `main`), #767 (duplicates #702 and the dense memo), #628, #484,
#473 and #420 (superseded in `main`), #373 (covered by #638), and #637/#735 (MoltenVK-specific).

## Runtime comparison controls

```text
--bda-sync Selective          Default: use dirty-region discovery, with conservative fallback.
--bda-sync Legacy             Use the full mapped-buffer walk for controlled comparisons.
--bda-sync SelectiveChecked   Use selective discovery and check remaining dirty-page coverage.
--shader-precompile true      Default: record and replay compatible shader permutations.
--shader-precompile false     Disable journal recording/replay for comparison or recovery.
```

The driver cache and shader journal require a Release build. They are keyed on a SHA-256 of the
recompiler and pipeline sources (`src/graphics/shader/**` and
`src/graphics/host_gpu/renderer/pipeline/**`, including uncommitted edits) and on the GPU/driver
identity; shader records also include the game version. Commits that do not touch those sources keep
a title's caches valid, so A/B builds can be measured warm; any change to them invalidates both files.
The journal detects checksum damage and interrupted writes, but is not an authenticated format for
importing arbitrary third-party shader records. Deleting a title's `.shaders` file or disabling replay
is the recovery path for a problematic record set.

The existing window dimensions do not establish the game's internal render resolution. The legacy
shader optimization selector has not been activated by this work. `--amd-cpu` remains an instruction
compatibility option rather than an X3D tuning switch.

## Validation commands

Validation on September 24, 2026: the combined Release/IPO `kyty_emulator` and `kyty_tests`
build succeeded, and all 49 registered CTest targets passed (51.03 seconds, serial execution).
GPU tests explicitly selected `AMD Radeon RX 9070 XT`, driver identifier `8389003`, including
the complete shader suite, static attachment feedback, pending-writeback races, shader replay
equivalence, and tiling/texture-cache tests. The focused wave-mask, FP16, and indirect-buffer
groups also passed during diagnosis. Logs are retained in `_Build/windows/ctest-final.log` and
`_Build/windows/Testing/Temporary/LastTest.log`.

This validation covers the changed mechanisms and the repository's regression coverage. It does
not establish an Astro Bot frame rate, full-game compatibility, or end-to-end warm startup for an
actual game's recorded shader set. The formatted-FP16 precision allowance is documented below.

Build with the repository's normal Windows prerequisites and Clang:

```powershell
cmake --build _Build/windows --target kyty_tests --parallel 8
$env:KYTY_TEST_GPU = 'AMD Radeon RX 9070 XT'
ctest --test-dir _Build/windows --output-on-failure
```

`KYTY_TEST_GPU` is an optional substring selector in the GPU test harness; it does not change the
emulator's `--gpu` selection. The harness prints the chosen adapter and mirrors the renderer's
optional attachment-feedback and provoking-vertex capabilities. Generic depth-backend tests choose
a supported four-byte depth/stencil backing rather than assuming D24S8 support. The tested Radeon
driver exposes D32S8 but does not expose D24S8.

Focused cases are `memory_tracker`, `sync_on_address`, `frame_statistics`, `mesh_dispatch`,
`shader_cfg`, `shader_precompile_record`, `performance_memory`, and `shader_precompile_gpu`.
The GPU tests exercise real buffer/image readbacks, pending-writeback ordering, dirty-region
equivalence, safe eviction, and live versus recorded shader execution. Windows fault tests issue
actual CPU stores while publication is held by a gate, covering ordinary collection, pressure
collection, overlap-style retirement, and live-image readbacks (including writes outside the image
on the same tracked page).
Depth-only eviction fixtures perform a real depth-only GPU clear; separately dirty stencil planes
must remain resident.

```powershell
.\_Build\windows\shader_recompiler_compute_tests.exe --bda-benchmark
```

This synthetic benchmark compares the original full mapped-buffer scan with selective discovery
over the same warmed, unchanged cached-buffer workload. The reference directly calls the original
range walk; it excludes hint maintenance added to the runtime `Legacy` comparison mode. There are
32 alternating batches of 64 passes per mode. Reported medians are medians of batch-average CPU
time per pass, not individual latency medians or game FPS. Do not run it while builds or other heavy
CPU tasks are active.

On September 24, 2026, this workload on the Ryzen 7 7800X3D / RX 9070 XT measured:

| Discovery path | Mean CPU microseconds/pass | Median batch-average microseconds/pass |
| --- | ---: | ---: |
| Original full scan | 16.443 | 16.315 |
| Selective hints with summary words | 0.110 | 0.111 |

There were 512 unchanged cached buffers and 2,048 passes per mode. Before the summary words, the
selective pass scanned all 4,096 hint words and measured 7.068 µs (median 6.986 µs) on the same
workload. That fixed cost made it slower than the full scan below roughly 250 cached buffers
(0.96 µs versus 6.8 µs at 32 buffers). With the summary words it is faster at every tested size
(32 to 8,192 buffers). The benchmark does not measure dirty uploads, shader cost, GPU execution,
frame time, or Astro Bot FPS. The GPU identity logged by the harness was `AMD Radeon RX 9070 XT`,
Vulkan driver value `8389003`.

## Remaining rendering work

Formatted FP16 buffer stores still use `PackHalf2x16`, whose rounding mode is implementation-defined
in the [Vulkan SPIR-V environment](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html).
The component-conversion regression accepts only two adjacent finite encodings for its one
non-representable input; all other components remain exact. This verifies numeric conversion instead
of raw low-bit copying, not bit-exact PS5 formatted-store rounding. A native guest-format oracle is
still needed before changing this production rounding behavior.

Real occlusion queries are not implemented here. They require decoding the currently ignored
`DB_COUNT_CONTROL`, accumulating counters across rendering and submission boundaries, and publishing
coherent, asynchronously completed guest results. Replacing the synthetic visibility counters without
those semantics could incorrectly hide geometry.

Full BVH/ray-tracing emulation is also unchanged. The base revision skips compute dispatches containing
unsupported BVH intersections. A higher frame rate with missing effects is not evidence of accurate
Astro Bot rendering at 60 fps.

For a gameplay comparison, keep the game version, patch set, save, route, and rendering configuration
fixed. Warm each build's cache separately, compare fresh-frame times and GPU/CPU traces, and check
image correctness as well as loading and save behavior. The 60 fps target is 16.67 ms per fresh frame.
