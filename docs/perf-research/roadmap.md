# Performance roadmap (draft)

Status on September 25, 2026: research notes. No emulator behavior was changed by this work.
Full agent notes are in [raw-findings.md](raw-findings.md). A snapshot of upstream's open pull
requests (fetched as `refs/pull/<N>/head`) is in [upstream-open-prs.txt](upstream-open-prs.txt).

## What the profile says

Astro Bot on a Ryzen 7 7800X3D and RX 9070 XT ran at about 27 fps with the GPU about 40% busy.
The guest GPU thread (`Thread_Gpu`, `GuestGpu::ThreadRun`) was the bottleneck:

- At least 38% of its time was spent in full GPU drains for CPU readbacks.
- About 12% went to the SRT walker (`SrtWalker::EvaluateWide`, `Value::Resolve`). This was first
  read as shader translation, but the run compiled no shaders after the startup replay. It is the
  per-draw `MaterializeResources` call on every shader cache hit (see the second pass in the raw
  notes).

The CPU and GPU take turns instead of overlapping.

## Can we have multi-core support?

Mostly, it already exists. Guest x86-64 code runs natively, and every guest pthread is its own
host thread, so the game's own CPU work already spreads across cores. The limit is that everything
the GPU needs goes through one thread. That thread parses PM4, looks up caches, translates shaders,
records Vulkan commands, submits them, and waits on readbacks.

Ways to move work off that thread, cheapest first:

1. **Stop draining on behalf of other threads.** When a game thread faults on GPU-written memory,
   the GPU thread records the download and then blocks until the GPU is idle. Instead, it can
   record, submit, and return the tick; the faulting thread then waits for that tick itself.
   `MasterSemaphore::Wait` and `WaitPriorityOperations` are already thread-safe. This needs a
   per-page "readback armed" token so that a GPU write recorded meanwhile is not lost (see
   Buffer readback in the raw notes).
2. **Compile shaders in parallel.** Journal replay (`PipelineCache::ReplayPrecompiled`) is
   single-threaded and holds `m_mutex` through translation and `vkCreateShaderModule`. It can use
   N workers and take the lock only for dedupe and insert. Pipelines can also be recorded and
   pre-created at startup. Opt-in async compilation with skip-draw is a larger follow-up.
3. **Submit from a separate thread.** A queue-owner thread takes over `vkQueueSubmit` and
   `vkQueuePresentKHR`; `queue_mutex` is shared with present, so a slow present blocks the GPU
   thread's submits today.
4. **Let the guest build the next frame while the GPU thread works** (bounded frames in flight at
   `GuestGpu::Done`, which currently waits for the GPU thread to go idle).
5. **Large:** Yuzu-style deferred command recording on a worker thread; a second Vulkan queue for
   guest async compute. Splitting PM4 parsing from Vulkan recording was evaluated and is not
   recommended yet.

## Ranked next steps

### Quick wins (days each, measure every one against upstream on the same save and route)

- **Tag pending image downloads with their tick.** `TextureCache::InvalidateMemory`, `FreeImage`
  and `SynchronizePredicate` would wait for that tick only, off the GPU thread when it is already
  submitted, instead of `Wait(CurrentTick())`.
- **Adaptive readback window.** Download the whole dirty extent of small buffers, and grow the
  window for sequential faulters, so one wait covers a streaming read instead of one drain per
  512 KiB.
- **Coalesce fault readbacks.** Readbacks queued in one `ProcessCommands` batch share one submit.
- **Fix millisecond-granularity waits on Windows.** `Common::CondVar::WaitFor` rounds sub-ms waits
  up to 1 ms. The blocked-queue poll in `ThreadRun` sleeps 100 ms.
- **Parallel journal replay.** Uses all cores at startup.
- **Make the shader journal and driver cache usable in development builds.** They are disabled for
  uncommitted builds, so tests of performance patches pay full compile cost.
- **Instrument the drains.** Count full drains per frame, split by caller (fault from a game thread,
  GPU-thread self-fault, DCC clear, predicate, GDS, GC). The profile could not tell which inner
  call caused the drains attributed to DMA.

### Medium (1-3 weeks)

- **Asynchronous fault readback** with per-page arm tokens, finalized in the priority callback
  (item 1 above). This is the biggest expected win for the 38%.
- **Cheaper per-draw resource materialization** (the 12%): memoize the snapshot per source entry
  when the plan is a pure function of user data and flat SRT slots, or compile the plan into a
  flat evaluation program. Also avoid the texture-cache lock on clean SRT reads.
- **DCC fast-clear materialization without readback.** Remember known fill values;
  `MaterializeDccClear` currently drains whenever the metadata range is GPU-dirty.
- **Queue-owner submit thread.**
- **Bounded guest frames in flight.**
- **Fewer shader permutations** (push-data placement, clip-space constants, depth bounds).

### Large

- Opt-in async shader compilation with skip-draw.
- GPU-side indirect draws (no CPU dereference of args).
- Deferred command recording.
- Second queue for async compute.

## How to measure

Keep the game version, save, route and settings fixed, and warm each build's shader cache
separately. Compare fresh-frame times and a CPU profile of `Thread_Gpu`, and check image
correctness. For scheduler and cache changes, the GPU test groups can now run without a GPU:

```bash
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json KYTY_TEST_SOFTWARE_GPU=1 \
  ctest --test-dir _Build/linux --output-on-failure
```

On Mesa 25.2 lavapipe, 43 of 47 targets pass. `shader_recompiler_compute`,
`compute_meta_clear_classification`, `shader_precompile_gpu` and `kernel_file_system` need
features, formats, a clean build, or a video device that lavapipe in a container does not provide.

## Research status

All thirteen areas are now covered. The last eight (texture-cache lookups, DMA, fault handling,
command-processor waits, per-draw CPU cost, host sync, frame pacing, and upstream PR triage) are in
the "Second pass" section of the raw notes. Main results of that pass:

- **DMA drains:** `DmaData` has no wait of its own. The drains the profile attributed to it can
  only come from staging-ring wraps (uploads or BDA page-table writes) or from faults in its CPU
  fast paths.
- **Texture lookups:** `FindImage` → `MaterializeDccClear` is a real full-drain path for DCC
  targets whose metadata is GPU-dirty.
- **Frame pacing:** vblank and present share one thread with a 1 ms `Flip(0)` sleep and take
  `RenderContext::m_mutex`, so drains inside draws delay presents.
- **Upstream PRs:** none of the open PRs addresses the Thread_Gpu bottleneck.
