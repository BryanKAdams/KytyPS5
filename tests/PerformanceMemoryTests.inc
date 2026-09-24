// Included at global scope after the existing GPU test helpers and before main.
#include <thread>

namespace Libs::Graphics {

struct PerformanceMemoryTestAccess {
  static MemoryTracker &Tracker(BufferCache &cache) {
    return cache.m_memory_tracker;
  }

  static void SetPageOwner(BufferCache &cache, uint64_t address,
                           BufferId owner) {
    cache.m_page_table[address >> BufferCache::CACHING_PAGEBITS] = owner;
  }

  static bool Download(TextureCache &cache, ImageId id) {
    std::lock_guard lock(cache.m_lock);
    return cache.DownloadImageMemory(id);
  }

  static void EnableLinearImageReadback(TextureCache &cache) {
    std::lock_guard lock(cache.m_lock);
    cache.m_readback_linear_images = true;
  }

  static void Retire(TextureCache &cache, ImageId id) {
    std::lock_guard lock(cache.m_lock);
    cache.FreeImage(id);
  }

  static void ClearImageGpuModified(TextureCache &cache, ImageId id) {
    std::lock_guard lock(cache.m_lock);
    cache.m_slot_images[id].ClearGpuModified();
  }

  static void SetCriticalPressure(TextureCache &cache) {
    cache.m_critical_gc_memory = 0;
  }

  static bool CollectOnMiss(TextureCache &cache) {
    return cache.CollectGarbage(true);
  }
  static uint64_t Age(const TextureCache &cache) { return cache.m_gc_tick; }
  static bool ImageAllocated(const TextureCache &cache, ImageId id) {
    return cache.m_slot_images.is_allocated(id);
  }
};

namespace {

class PerformanceMemoryFixture {
public:
  PerformanceMemoryFixture(VulkanHarness &vulkan, const char *name,
                           uint64_t base, uint64_t size)
      : name(name), base(base), size(size), context(vulkan.RuntimeContext()),
        processor(context, 0) {
    context.InitializeGpu(nullptr);
    Require(name, "allocate guest backing",
            LibKernel::Memory::KernelAllocateDirectMemory(
                0, LibKernel::Memory::KernelGetDirectMemorySize(), size,
                0x10000, 0, &direct_offset) == 0,
            "performance-memory allocation failed");
    void *mapped = reinterpret_cast<void *>(base);
    Require(name, "map guest backing",
            LibKernel::Memory::KernelMapDirectMemory(
                &mapped, size, 0x3, 0x10, direct_offset, 0x10000) == 0 &&
                mapped == reinterpret_cast<void *>(base),
            "performance-memory fixed mapping failed");
    std::memset(mapped, 0x5a, static_cast<size_t>(size));
    context.GetGpu().SendCommandSync([&] { processor.BufferInit(); });
  }

  ~PerformanceMemoryFixture() {
    context.GetGpu().SendCommandSync([&] { processor.BufferWait(); });
    context.UnmapMemory(base, size);
    context.ShutdownGpu();
    context.GetCommandScheduler().Shutdown();
    Require(name, "unmap guest backing",
            LibKernel::Memory::KernelMunmap(base, size) == 0,
            "performance-memory unmap failed");
    Require(name, "release guest backing",
            LibKernel::Memory::KernelReleaseDirectMemory(direct_offset, size) ==
                0,
            "performance-memory release failed");
  }

  void WriteCpu(uint64_t address, uint64_t bytes, uint8_t value) {
    auto &tracker =
        PerformanceMemoryTestAccess::Tracker(context.GetBufferCache());
    tracker.MarkRegionAsCpuModified(address, bytes);
    std::memset(reinterpret_cast<void *>(address), value,
                static_cast<size_t>(bytes));
  }

  // Read the actual cached GPU bytes without ObtainBuffer, which could hide a
  // missed BDA upload.
  std::vector<uint8_t> ReadNative(uint64_t address, uint64_t bytes) {
    auto &cache = context.GetBufferCache();
    auto &scheduler = context.GetCommandScheduler();
    auto &source = cache.GetBuffer(cache.FindBuffer(address, bytes));
    Libs::Graphics::Buffer readback(
        context.GetGraphics(), scheduler, MemoryUsage::Download, 0,
        vk::BufferUsageFlagBits::eTransferDst, bytes);
    scheduler.EndRendering();
    const auto command = scheduler.Current().Handle();
    vk::BufferMemoryBarrier before{};
    before.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
    before.dstAccessMask = vk::AccessFlagBits::eTransferRead;
    before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before.buffer = source.Handle();
    before.offset = source.Offset(address);
    before.size = bytes;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                            vk::PipelineStageFlagBits::eTransfer, {}, 0,
                            nullptr, 1, &before, 0, nullptr);
    const vk::BufferCopy copy{source.Offset(address), 0, bytes};
    command.copyBuffer(source.Handle(), readback.Handle(), 1, &copy);
    auto after = before;
    after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    after.dstAccessMask = vk::AccessFlagBits::eHostRead;
    after.buffer = readback.Handle();
    after.offset = 0;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1,
                            &after, 0, nullptr);
    scheduler.Wait(scheduler.CurrentTick());
    readback.Invalidate(0, bytes);
    return {readback.Mapped().begin(), readback.Mapped().end()};
  }

  const char *name;
  uint64_t base;
  uint64_t size;
  RenderContext context;
  CommandProcessor processor;

private:
  int64_t direct_offset = -1;
};

TextureCache::ImageDesc PerformanceLinearImage(uint64_t address, uint32_t width,
                                               uint32_t height) {
  TextureCache::ImageDesc desc{};
  desc.type = TextureCache::BindingType::Texture;
  desc.info.data = {address, uint64_t{width} * height * sizeof(uint32_t)};
  desc.info.pixel_format = vk::Format::eR8G8B8A8Uint;
  desc.info.guest_format = Prospero::BufferFormat::k8_8_8_8UInt;
  desc.info.type = Prospero::ImageType::kColor2D;
  desc.info.extent = {width, height, 1};
  desc.info.resources = {1, 1};
  desc.info.pitch = width;
  desc.info.bytes_per_block = 4;
  desc.info.samples = 1;
  desc.info.tile_mode = Prospero::TileMode::kLinear;
  desc.info.mip_layout[0] = {0, desc.info.data.size, width, height};
  desc.view_info.format = desc.info.pixel_format;
  desc.view_info.type = vk::ImageViewType::e2D;
  desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
  desc.view_info.usage = vk::ImageUsageFlagBits::eSampled;
  return desc;
}

void CheckBdaSelectiveMemory(VulkanHarness &vulkan) {
  constexpr const char *name = "BdaSelectiveMemory";
  constexpr uint64_t base = 0x0000000280000000ull;
  constexpr auto page = BufferCache::CACHING_PAGESIZE;
  constexpr auto region = TRACKER_REGION_SIZE;
  const auto scenario = [&](bool selective) {
    PerformanceMemoryFixture fixture(vulkan, name, base, region * 2);
    std::vector<uint8_t> observed;
    auto &context = fixture.context;
    auto &cache = context.GetBufferCache();
    auto &tracker = PerformanceMemoryTestAccess::Tracker(cache);
    RangeSet mapped;
    const auto map = [&](uint64_t address, uint64_t bytes) {
      mapped.Add(address, bytes);
      context.MapMemory(address, bytes);
    };
    const auto sync = [&] {
      if (selective) {
        Require(name, "selective discovery",
                cache.SynchronizeBdaSelective(mapped),
                "selective discovery rejected a valid owner index");
      } else {
        cache.SynchronizeBdaLegacy(mapped);
      }
      Require(name, "dirty coverage", cache.CheckBdaHintInvariant(mapped),
              "completed pass lost a mapped registered dirty page");
    };
    context.GetGpu().SendCommandSync([&] {
      map(base, page);
      map(base + 2 * page, page);
      sync(); // Mapping hints can be consumed before any cache owner/manager
              // exists.
      const auto first_owner = cache.FindBuffer(base, 3 * page);
      Require(name, "registration republishes",
              tracker.IsBdaHintPending(base / region),
              "registration did not publish previously unreachable pages");
      sync();
      for (const auto address : {base, base + 2 * page}) {
        const auto bytes = fixture.ReadNative(address, page);
        Require(name, "partial mapping bytes",
                std::ranges::all_of(
                    bytes, [](uint8_t value) { return value == 0x5a; }),
                "mapped part of a shared owner was not uploaded");
      }
      Require(name, "unmapped page stays dirty",
              tracker.IsRegionCpuModified(base + page, page),
              "partial synchronization consumed an unmapped page");
      fixture.WriteCpu(base + page, page, 0x72);
      // Consume its write hint while still unmapped, then require MapMemory to
      // publish again.
      sync();
      map(base + page, page);
      Require(name, "mapping republishes",
              tracker.IsBdaHintPending(base / region),
              "mapping did not republish an already registered dirty page");
      sync();
      auto bytes = fixture.ReadNative(base + page, page);
      Require(name, "remapped bytes",
              std::ranges::all_of(bytes,
                                  [](uint8_t value) { return value == 0x72; }),
              "new mapped interval reused stale GPU bytes");

      fixture.WriteCpu(base, page, 0x19);
      fixture.WriteCpu(base + 3 * page, page, 0x43);
      map(base + 3 * page, page);
      const auto replacement = cache.FindBuffer(base, 4 * page);
      Require(name, "owner replacement", replacement != first_owner,
              "test did not expand and replace the registered owner");
      sync();

      const auto cross_region = base + region - page;
      fixture.WriteCpu(cross_region, page, 0x27);
      fixture.WriteCpu(cross_region + page, page, 0x38);
      map(cross_region, 2 * page);
      (void)cache.FindBuffer(cross_region, 2 * page);
      sync();

      if (selective) {
        fixture.WriteCpu(base, page, 0x61);
        PerformanceMemoryTestAccess::SetPageOwner(cache, base, first_owner);
        Require(name, "stale owner fails closed",
                !cache.SynchronizeBdaSelective(mapped) &&
                    tracker.IsBdaHintPending(base / region),
                "failed selective pass lost the unprocessed region");
        PerformanceMemoryTestAccess::SetPageOwner(cache, base, replacement);
        cache.SynchronizeBdaLegacy(mapped);
      } else {
        fixture.WriteCpu(base, page, 0x61);
        sync();
      }
      const auto first = fixture.ReadNative(base, 4 * page);
      const auto crossing = fixture.ReadNative(cross_region, 2 * page);
      for (size_t index = 0; index < first.size(); ++index) {
        const std::array<uint8_t, 4> expected{0x61, 0x72, 0x5a, 0x43};
        Require(name, "expanded owner contents",
                first[index] == expected[index / page],
                "replacement lost uploaded bytes or retained a stale page");
      }
      for (size_t index = 0; index < crossing.size(); ++index) {
        Require(name, "cross-region contents",
                crossing[index] == (index < page ? 0x27 : 0x38),
                "cross-region synchronization missed an owner fragment");
      }
      observed.insert(observed.end(), first.begin(), first.end());
      observed.insert(observed.end(), crossing.begin(), crossing.end());
      Require(name, "final coverage", cache.CheckBdaHintInvariant(mapped),
              "fallback or replacement broke the dirty-hint invariant");
    });
    return observed;
  };
  Require(name, "legacy/selective differential",
          scenario(false) == scenario(true),
          "selective discovery changed GPU-visible bytes");
  std::printf("[host]    %-32s ok\n", name);
}

void CheckOversizedBufferReadback(VulkanHarness &vulkan) {
  constexpr const char *name = "OversizedBufferReadback";
  constexpr uint64_t mib = 1024 * 1024;
  constexpr uint64_t base = 0x0000000290000000ull;
  constexpr uint64_t first_size = 33 * mib;
  constexpr uint64_t second_offset = 34 * mib;
  constexpr uint64_t range_size = 67 * mib;
  PerformanceMemoryFixture fixture(vulkan, name, base, 68 * mib);
  auto &context = fixture.context;
  context.MapMemory(base, fixture.size);
  context.GetGpu().SendCommandSync([&] {
    auto &cache = context.GetBufferCache();
    (void)cache.FindBuffer(base, range_size);
    cache.SynchronizeBuffersInRange(base, range_size);
    (void)cache.ObtainBuffer(base, first_size, true, false);
    cache.FillBuffer(base, first_size, 0x11223344u, false);
    (void)cache.ObtainBuffer(base + second_offset, first_size, true, false);
    cache.FillBuffer(base + second_offset, first_size, 0xaabbccddu, false);
    const auto ring = cache.GetUtilityBuffer(MemoryUsage::Download).Handle();
    cache.ReadMemory(base, range_size, false);
    Require(
        name, "ownership publication",
        !cache.IsRegionGpuModified(base, range_size) &&
            ring == cache.GetUtilityBuffer(MemoryUsage::Download).Handle(),
        "oversized readback failed to publish or replaced the reusable ring");
    std::vector<uint32_t> data(range_size / sizeof(uint32_t));
    Require(name, "read guest backing",
            LibKernel::Memory::TryReadBacking(base, data.data(), range_size),
            "oversized backing read failed");
    const auto first_end = data.begin() + first_size / sizeof(uint32_t);
    const auto second_begin = data.begin() + second_offset / sizeof(uint32_t);
    Require(
        name, "packed copies and untouched gap",
        std::all_of(data.begin(), first_end,
                    [](uint32_t value) { return value == 0x11223344u; }) &&
            std::all_of(first_end, second_begin,
                        [](uint32_t value) { return value == 0x5a5a5a5au; }) &&
            std::all_of(second_begin, data.end(),
                        [](uint32_t value) { return value == 0xaabbccddu; }),
        "oversized packed copies lost an interval or overwrote their clean "
        "gap");
  });
  std::printf("[host]    %-32s ok\n", name);
}

void CheckPredicateMemoryProducers(VulkanHarness &vulkan) {
  constexpr const char *name = "PredicateMemoryProducers";
  constexpr uint64_t base = 0x00000002a0000000ull;
  PerformanceMemoryFixture fixture(vulkan, name, base, 0x10000);
  auto &context = fixture.context;
  context.MapMemory(base, fixture.size);
  context.GetGpu().SendCommandSync([&] {
    auto &cache = context.GetBufferCache();
    auto &scheduler = context.GetCommandScheduler();
    auto &processor = fixture.processor;
    const auto tick = scheduler.CurrentTick();
    processor.SetPredication(1, 3, 1, reinterpret_cast<void *>(base), 0);
    Require(name, "CPU producer",
            !processor.ShouldSkipPredicatedPackets() &&
                scheduler.CurrentTick() == tick,
            "CPU-only predicate unnecessarily submitted queued GPU work");
    fixture.WriteCpu(base + 0x4000, sizeof(uint64_t), 0);
    (void)cache.ObtainBuffer(base + 0x4000, sizeof(uint64_t), true, false);
    cache.FillBuffer(base + 0x4000, sizeof(uint64_t), 1, false);
    processor.SetPredication(1, 3, 1, reinterpret_cast<void *>(base + 0x4000),
                             0);
    uint64_t value = 0;
    Require(name, "GPU producer",
            !processor.ShouldSkipPredicatedPackets() &&
                !cache.IsRegionGpuModified(base + 0x4000, sizeof(uint64_t)) &&
                LibKernel::Memory::TryReadBacking(base + 0x4000, &value,
                                                  sizeof(value)) &&
                value == 0x0000000100000001ull,
            "predicate did not observe the queued buffer producer");
    const auto after = scheduler.CurrentTick();
    processor.SetPredication(1, 3, 0, reinterpret_cast<void *>(base), 0);
    Require(name, "no wait", scheduler.CurrentTick() == after,
            "no-wait predicate changed its submission behavior");
  });
  std::printf("[host]    %-32s ok\n", name);
}

void CheckPendingImagePredicates(VulkanHarness &vulkan) {
  constexpr const char *name = "PendingImagePredicates";
  constexpr uint64_t base = 0x00000002b0000000ull;
  PerformanceMemoryFixture fixture(vulkan, name, base, 0x10000);
  auto &context = fixture.context;
  auto &textures = context.GetTextureCache();
  context.MapMemory(base, fixture.size);
  std::binary_semaphore first_started{0};
  std::binary_semaphore first_gate{0};
  std::binary_semaphore middle_started{0};
  std::binary_semaphore middle_gate{0};
  std::binary_semaphore predicate_started{0};
  std::binary_semaphore predicate_done{0};
  uint64_t sampled = 0;
  bool skipped = true;
  context.GetGpu().SendCommandSync([&] {
    fixture.WriteCpu(base, sizeof(uint64_t), 0);
    auto desc = PerformanceLinearImage(base, 2, 1);
    const auto id = textures.FindImage(desc);
    vk::ClearValue clear{};
    clear.color.uint32 = std::array{1u, 0u, 0u, 0u};
    auto &scheduler = context.GetCommandScheduler();
    TextureCacheTestAccess::ClearImage(
        textures, scheduler.Current(), id,
        {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}, clear);
    textures.MarkGpuWritten(id);
    scheduler.DeferPriorityOperation([&] {
      first_started.release();
      first_gate.acquire();
    });
    Require(name, "first image download",
            PerformanceMemoryTestAccess::Download(textures, id),
            "first image readback was not recorded");
    scheduler.DeferPriorityOperation([&] {
      middle_started.release();
      middle_gate.acquire();
    });
    Require(name, "overlapping image download",
            PerformanceMemoryTestAccess::Download(textures, id),
            "second image readback was not recorded");
    // Exercise pending-only predicate ownership without dropping the page's
    // write watcher. Actual retirement must wait for these gated publications.
    PerformanceMemoryTestAccess::ClearImageGpuModified(textures, id);
    Require(
        name, "publication-only ownership remains pending",
        !textures.IsRegionGpuModified(base, sizeof(uint64_t)) &&
            textures.HasPendingDownload(base, sizeof(uint64_t)) &&
            TextureCacheTestAccess::Contains(textures, id) &&
            textures.GetImage(id).IsTracked() &&
            !textures.HasPendingDownload(base + sizeof(uint64_t), 1),
        "publication lost protected ownership or used closed interval overlap");
    scheduler.Flush();
  });

  // The gates order callback execution, independently of GPU or thread
  // scheduling speed.
  Require(name, "first callback started",
          first_started.try_acquire_for(std::chrono::seconds(5)),
          "priority runner did not reach the first gate");
  first_gate.release();
  Require(name, "middle callback started",
          middle_started.try_acquire_for(std::chrono::seconds(5)),
          "priority runner did not retire the first image download");
  Require(
      name, "overlapping writeback still pending",
      textures.HasPendingDownload(base, 8),
      "completing one download erased another overlapping pending writeback");

  context.GetGpu().SendCommand([&] {
    predicate_started.release();
    fixture.processor.SetPredication(1, 3, 1, reinterpret_cast<void *>(base),
                                     0);
    skipped = fixture.processor.ShouldSkipPredicatedPackets();
    Require(name, "sample backing",
            LibKernel::Memory::TryReadBacking(base, &sampled, sizeof(sampled)),
            "predicate backing became unavailable");
    predicate_done.release();
  });
  Require(name, "predicate started",
          predicate_started.try_acquire_for(std::chrono::seconds(5)),
          "GPU command thread did not reach the predicate");
  const bool completed_before_publication =
      predicate_done.try_acquire_for(std::chrono::milliseconds(100));
  middle_gate.release();
  if (!completed_before_publication) {
    Require(name, "predicate completed",
            predicate_done.try_acquire_for(std::chrono::seconds(5)),
            "predicate did not resume after the pending writeback finished");
  }
  Require(name, "priority lifecycle",
          !completed_before_publication && !skipped &&
              sampled == 0x0000000100000001ull &&
              !textures.HasPendingDownload(base, sizeof(uint64_t)),
          "predicate escaped its pending image producer or sampled the wrong "
          "bytes");
  std::printf("[host]    %-32s ok\n", name);
}

void CheckOversizedImageReadback(VulkanHarness &vulkan) {
  constexpr const char *name = "OversizedImageReadback";
  constexpr uint64_t base = 0x00000002c0000000ull;
  constexpr uint32_t width = 4096;
  constexpr uint32_t height = 4097;
  constexpr uint64_t bytes = uint64_t{width} * height * sizeof(uint32_t);
  static_assert(bytes > 64ull * 1024 * 1024);
  PerformanceMemoryFixture fixture(vulkan, name, base, 68ull * 1024 * 1024);
  auto &context = fixture.context;
  context.MapMemory(base, fixture.size);
  context.GetGpu().SendCommandSync([&] {
    auto &textures = context.GetTextureCache();
    auto &scheduler = context.GetCommandScheduler();
    auto desc = PerformanceLinearImage(base, width, height);
    const auto id = textures.FindImage(desc);
    vk::ClearValue clear{};
    clear.color.uint32 = std::array{0x12u, 0x34u, 0x56u, 0x78u};
    TextureCacheTestAccess::ClearImage(
        textures, scheduler.Current(), id,
        {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}, clear);
    textures.MarkGpuWritten(id);
    const auto ring = context.GetBufferCache()
                          .GetUtilityBuffer(MemoryUsage::Download)
                          .Handle();
    const auto tick = scheduler.CurrentTick();
    Require(name, "oversized download recorded",
            PerformanceMemoryTestAccess::Download(textures, id),
            "image readback exceeding the ring was rejected");
    uint32_t stale = 0;
    Require(
        name, "deferred bytes and lifetime",
        scheduler.CurrentTick() == tick &&
            textures.HasPendingDownload(base, bytes) &&
            LibKernel::Memory::TryReadBacking(base, &stale, sizeof(stale)) &&
            stale == 0x5a5a5a5au,
        "oversized image staging submitted early or failed to retain pending "
        "ownership");
    PerformanceMemoryTestAccess::Retire(textures, id);
    Require(name, "retirement completed publication",
            scheduler.CurrentTick() == tick + 1 &&
                !textures.HasPendingDownload(base, bytes) &&
                !TextureCacheTestAccess::Contains(textures, id),
            "oversized retirement released ownership before publication");
    scheduler.PopPendingOperations();
    std::vector<uint32_t> data(bytes / sizeof(uint32_t));
    Require(
        name, "completed image bytes",
        LibKernel::Memory::TryReadBacking(base, data.data(), bytes) &&
            std::ranges::all_of(
                data, [](uint32_t value) { return value == 0x78563412u; }) &&
            !textures.HasPendingDownload(base, bytes) &&
            ring == context.GetBufferCache()
                        .GetUtilityBuffer(MemoryUsage::Download)
                        .Handle(),
        "dedicated image staging lost contents, leaked pending ownership, or "
        "replaced the ring");
  });
  std::printf("[host]    %-32s ok\n", name);
}

void CheckConservativeImagePressure(VulkanHarness &vulkan) {
  constexpr const char *name = "ConservativeImagePressure";
  constexpr uint64_t base = 0x00000002d0000000ull;
  PerformanceMemoryFixture fixture(vulkan, name, base, 0x10000);
  auto &context = fixture.context;
  context.MapMemory(base, fixture.size);
  context.GetGpu().SendCommandSync([&] {
    auto &textures = context.GetTextureCache();
    auto &scheduler = context.GetCommandScheduler();
    const auto color = [&](uint64_t address) {
      auto desc = PerformanceLinearImage(address, 2, 1);
      const auto id = textures.FindImage(desc);
      vk::ClearValue clear{};
      clear.color.uint32 = std::array{1u, 2u, 3u, 4u};
      TextureCacheTestAccess::ClearImage(
          textures, scheduler.Current(), id,
          {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}, clear);
      textures.MarkGpuWritten(id);
      return id;
    };
    const auto safe = color(base);
    const auto conflict = color(base + 0x1000);
    textures.GetImage(conflict).MarkBufferModified();

    auto depth_desc = PerformanceLinearImage(base + 0x2000, 1, 1);
    depth_desc.type = TextureCache::BindingType::DepthTarget;
    depth_desc.info.pixel_format = vk::Format::eD32SfloatS8Uint;
    depth_desc.info.guest_format = Prospero::BufferFormat::k32Float;
    depth_desc.info.stencil = {base + 0x2800, sizeof(uint32_t)};
    depth_desc.view_info.format = depth_desc.info.pixel_format;
    depth_desc.view_info.aspect =
        vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
    depth_desc.view_info.usage =
        vk::ImageUsageFlagBits::eDepthStencilAttachment;
    const auto depth = textures.FindImage(depth_desc);
    TextureCacheTestAccess::AssociateStencil(textures, depth,
                                             depth_desc.info.stencil);
    vk::ClearValue depth_clear{};
    depth_clear.depthStencil = vk::ClearDepthStencilValue{0.5f, 42};
    TextureCacheTestAccess::ClearImage(
        textures, scheduler.Current(), depth,
        {depth_desc.view_info.aspect, 0, 1, 0, 1}, depth_clear);
    textures.MarkGpuWritten(depth);
    const auto stencil =
        textures.FindImageFromRange(base + 0x2800, sizeof(uint32_t), false);
    Require(name, "depth/stencil fixture", static_cast<bool>(stencil),
            "combined depth fixture did not create a stencil owner");

    const auto current = color(base + 0x3000);
    const std::array oldest{safe, conflict, depth, stencil, current};
    TextureCacheTestAccess::ConfigureGarbageCollection(textures, oldest, 0, 0);
    PerformanceMemoryTestAccess::SetCriticalPressure(textures);
    Require(name, "initial submission retention",
            !PerformanceMemoryTestAccess::CollectOnMiss(textures) &&
                TextureCacheTestAccess::Contains(textures, safe) &&
                PerformanceMemoryTestAccess::Age(textures) == 0,
            "image misses treated an initial submission as an old working set");
    TextureCacheTestAccess::ConfigureGarbageCollection(textures, oldest, 200,
                                                       0);
    PerformanceMemoryTestAccess::SetCriticalPressure(textures);
    textures.GetImage(current).tick_accessed_last = scheduler.CurrentTick();
    bool unrelated_callback_ran = false;
    scheduler.DeferOperation([&] { unrelated_callback_ran = true; });
    const auto age = PerformanceMemoryTestAccess::Age(textures);
    Require(name, "safe on-demand recovery",
            PerformanceMemoryTestAccess::CollectOnMiss(textures) &&
                PerformanceMemoryTestAccess::Age(textures) == age &&
                !TextureCacheTestAccess::Contains(textures, safe) &&
                !PerformanceMemoryTestAccess::ImageAllocated(textures, safe) &&
                TextureCacheTestAccess::Contains(textures, conflict) &&
                TextureCacheTestAccess::Contains(textures, depth) &&
                TextureCacheTestAccess::Contains(textures, stencil) &&
                TextureCacheTestAccess::Contains(textures, current) &&
                !unrelated_callback_ran,
            "pressure recovery aged the cache, retained all reclaimable work, "
            "or lost a dirty representation");
    scheduler.Wait(scheduler.CurrentTick());
    scheduler.WaitPriorityOperations(scheduler.CurrentTick() - 1);
    scheduler.PopPendingOperations();
    uint32_t published = 0;
    Require(name, "safe writeback published",
            LibKernel::Memory::TryReadBacking(base, &published,
                                              sizeof(published)) &&
                published == 0x04030201u,
            "eviction failed to publish the safely downloadable image");

    // A clean parent must also survive while its separately owned stencil plane
    // is dirty.
    textures.GetImage(depth).ClearGpuModified();
    const std::array still_owned{conflict, depth, stencil};
    TextureCacheTestAccess::ConfigureGarbageCollection(textures, still_owned,
                                                       400, 0);
    PerformanceMemoryTestAccess::SetCriticalPressure(textures);
    textures.RunGarbageCollector();
    Require(name, "stencil and conflicting ownership retention",
            TextureCacheTestAccess::Contains(textures, conflict) &&
                TextureCacheTestAccess::Contains(textures, depth) &&
                TextureCacheTestAccess::Contains(textures, stencil) &&
                textures.GetImage(stencil).IsGpuModified(),
            "GC deleted dirty stencil data through its clean parent or "
            "discarded conflicting aliases");
  });
  std::printf("[host]    %-32s ok\n", name);
}

void CheckTiledImagePressure(VulkanHarness &vulkan) {
  constexpr const char *name = "TiledImagePressure";
  constexpr uint64_t base = 0x00000002f0000000ull;
  constexpr uint32_t width = 7;
  constexpr uint32_t height = 5;
  PerformanceMemoryFixture fixture(vulkan, name, base, 0x10000);
  auto &context = fixture.context;
  context.MapMemory(base, fixture.size);
  context.GetGpu().SendCommandSync([&] {
    TileSurfaceLayout surface{};
    Require(name, "tiled layout",
            TileGetTiledTextureLayout(
                {.format = Prospero::BufferFormat::k8_8_8_8UInt,
                 .tile_mode = Prospero::TileMode::kStandard4KB,
                 .width = width,
                 .height = height,
                 .levels = 1},
                surface) &&
                surface.total_size == 4096,
            "small texture did not fit one known tile block");
    const auto &mip = surface.mips[0];
    auto desc = PerformanceLinearImage(base, width, height);
    desc.info.data.size = surface.total_size;
    desc.info.pitch = mip.padded_width;
    desc.info.tile_mode = Prospero::TileMode::kStandard4KB;
    desc.info.mip_layout[0] = {mip.offset, mip.size, mip.padded_width,
                               mip.padded_height};
    std::vector<uint32_t> expected(surface.total_size / sizeof(uint32_t),
                                   0x5a5a5a5au);
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        uint32_t offset = 0;
        Require(name, "CPU tiled address",
                TileGetBlockOffset(surface.texture.block, x + mip.tail_x,
                                   y + mip.tail_y, 0, offset) &&
                    mip.offset + offset < surface.total_size,
                "CPU reference could not address a tiled texel");
        expected[(mip.offset + offset) / sizeof(uint32_t)] = 0x44332211u;
      }
    }
    auto &textures = context.GetTextureCache();
    auto &scheduler = context.GetCommandScheduler();
    const auto id = textures.FindImage(desc);
    vk::ClearValue clear{};
    clear.color.uint32 = std::array{0x11u, 0x22u, 0x33u, 0x44u};
    TextureCacheTestAccess::ClearImage(
        textures, scheduler.Current(), id,
        {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}, clear);
    textures.MarkGpuWritten(id);
    TextureCacheTestAccess::ConfigureGarbageCollection(textures, std::array{id},
                                                       200, 0);
    textures.RunGarbageCollector();
    Require(name, "noncritical tiled retention",
            TextureCacheTestAccess::Contains(textures, id),
            "ordinary pressure evicted a tiled GPU owner");
    TextureCacheTestAccess::ConfigureGarbageCollection(textures, std::array{id},
                                                       200, 0);
    PerformanceMemoryTestAccess::SetCriticalPressure(textures);
    textures.RunGarbageCollector();
    Require(name, "critical tiled retirement",
            !TextureCacheTestAccess::Contains(textures, id) &&
                !textures.HasPendingDownload(base, surface.total_size),
            "critical pressure did not publish the tiled writeback before "
            "retirement");
    const auto tick = scheduler.CurrentTick();
    scheduler.Wait(tick);
    scheduler.WaitPriorityOperations(tick);
    scheduler.PopPendingOperations();
    std::vector<uint32_t> observed(expected.size());
    Require(name, "tiled bytes and preserved padding",
            LibKernel::Memory::TryReadBacking(base, observed.data(),
                                              surface.total_size) &&
                observed == expected,
            "tiled eviction lost texels or overwrote guest padding");
  });
  std::printf("[host]    %-32s ok\n", name);
}

void CheckImageWritebackFaultOrdering(VulkanHarness &vulkan) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  constexpr uint64_t base = 0x0000000300000000ull;
  constexpr uint32_t cpu_value = 0x76543210u;
  for (const int retirement : {0, 1, 2}) {
    const bool pressure_only = retirement == 1;
    const bool direct_retirement = retirement == 2;
    const char *name = direct_retirement ? "RetiredImageWritebackCpuFault"
                       : pressure_only ? "PressureWritebackCpuFault"
                                       : "BoundaryWritebackCpuFault";
    PerformanceMemoryFixture fixture(vulkan, name, base, 0x10000);
    auto &context = fixture.context;
    context.MapMemory(base, fixture.size);
    LibKernel::Memory::InstallGpuResources(&context);
    std::binary_semaphore callback_started{0};
    std::binary_semaphore callback_gate{0};
    std::binary_semaphore collector_done{0};
    std::binary_semaphore writer_started{0};
    std::binary_semaphore writer_done{0};
    ImageId image;
    context.GetGpu().SendCommandSync([&] {
      auto &textures = context.GetTextureCache();
      auto &scheduler = context.GetCommandScheduler();
      auto desc = PerformanceLinearImage(base, 2, 1);
      const auto id = textures.FindImage(desc);
      image = id;
      vk::ClearValue clear{};
      clear.color.uint32 = std::array{1u, 0u, 0u, 0u};
      TextureCacheTestAccess::ClearImage(
          textures, scheduler.Current(), id,
          {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}, clear);
      textures.MarkGpuWritten(id);
      TextureCacheTestAccess::ConfigureGarbageCollection(
          textures, std::array{id}, 200, 0);
      PerformanceMemoryTestAccess::SetCriticalPressure(textures);
      scheduler.DeferPriorityOperation([&] {
        if (direct_retirement) {
          // Non-GC retirement must not wait with the texture lock held.
          auto lock = TextureCacheTestAccess::Lock(textures);
          callback_started.release();
          callback_gate.acquire();
        } else {
          callback_started.release();
          callback_gate.acquire();
        }
      });
      if (direct_retirement) {
        Require(name, "non-GC image download recorded",
                PerformanceMemoryTestAccess::Download(textures, id),
                "non-GC retirement has no pending publication");
      }
    });
    context.GetGpu().SendCommand([&] {
      if (direct_retirement) {
        PerformanceMemoryTestAccess::Retire(context.GetTextureCache(), image);
      } else if (pressure_only) {
        (void)PerformanceMemoryTestAccess::CollectOnMiss(
            context.GetTextureCache());
      } else {
        context.GetTextureCache().RunGarbageCollector();
      }
      // Also submits on a regressed implementation that returned before its
      // queued download.
      context.GetCommandScheduler().Flush();
      collector_done.release();
    });
    Require(name, "writeback gate reached",
            callback_started.try_acquire_for(std::chrono::seconds(5)),
            "collector did not submit the pending download");
    MEMORY_BASIC_INFORMATION mapping{};
    const bool remained_protected =
        VirtualQuery(reinterpret_cast<void *>(base), &mapping,
                     sizeof(mapping)) == sizeof(mapping) &&
        (mapping.Protect & 0xffu) == PAGE_READONLY;
    std::jthread writer([&] {
      writer_started.release();
      // Exercise the production access-violation route, not an explicit
      // invalidation shortcut.
      *reinterpret_cast<volatile uint32_t *>(base) = cpu_value;
      writer_done.release();
    });
    writer_started.acquire();
    const bool wrote_before_publication =
        writer_done.try_acquire_for(std::chrono::milliseconds(100));
    callback_gate.release();
    Require(name, "collector completed",
            collector_done.try_acquire_for(std::chrono::seconds(5)),
            "collector did not complete after releasing its writeback gate");
    if (!wrote_before_publication) {
      Require(name, "faulting writer resumed",
              writer_done.try_acquire_for(std::chrono::seconds(5)),
              "CPU writer remained blocked after writeback publication");
    }
    writer.join();
    context.GetGpu().SendCommandSync([&] {
      fixture.processor.BufferWait();
      Require(name, "image retired after publication",
              !TextureCacheTestAccess::Contains(context.GetTextureCache(), image) &&
                  !context.GetTextureCache().HasPendingDownload(base, 8),
              "retirement retained its image or pending publication");
    });
    uint32_t observed = 0;
    const bool read =
        LibKernel::Memory::TryReadBacking(base, &observed, sizeof(observed));
    LibKernel::Memory::InstallGpuResources(nullptr);
    Require(name, "protected ownership through publication",
            remained_protected && !wrote_before_publication && read &&
                observed == cpu_value,
            "image eviction exposed writable guest pages early or clobbered "
            "the faulting CPU store");
    std::printf("[host]    %-32s ok\n", name);
  }
#else
  (void)vulkan;
#endif
}

void CheckLiveImageWritebackFaultOrdering(VulkanHarness &vulkan) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  constexpr uint64_t base = 0x0000000310000000ull;
  constexpr uint64_t image_address = base + 0x200;
  constexpr uint32_t cpu_value = 0x76543210u;
  for (const bool neighboring_fault : {false, true}) {
    const char *name = neighboring_fault ? "LiveImageNeighborCpuFault"
                                         : "LiveImageWritebackCpuFault";
    PerformanceMemoryFixture fixture(vulkan, name, base, 0x10000);
    auto &context = fixture.context;
    auto &textures = context.GetTextureCache();
    context.MapMemory(base, fixture.size);
    LibKernel::Memory::InstallGpuResources(&context);
    std::binary_semaphore callback_started{0};
    std::binary_semaphore callback_gate{0};
    std::binary_semaphore writer_started{0};
    std::binary_semaphore writer_done{0};
    ImageId image;
    context.GetGpu().SendCommandSync([&] {
      auto &scheduler = context.GetCommandScheduler();
      PerformanceMemoryTestAccess::EnableLinearImageReadback(textures);
      auto desc = PerformanceLinearImage(image_address, 2, 1);
      desc.type = TextureCache::BindingType::Storage;
      desc.view_info.usage = vk::ImageUsageFlagBits::eStorage;
      image = textures.FindImage(desc);
      (void)textures.FindTexture(image, desc);
      vk::ClearValue clear{};
      clear.color.uint32 = std::array{1u, 2u, 3u, 4u};
      TextureCacheTestAccess::ClearImage(
          textures, scheduler.Current(), image,
          {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}, clear);
      scheduler.DeferPriorityOperation([&] {
        // This lock acquisition also catches a CPU invalidation that waits for
        // the GPU without first releasing m_lock. The actual image callback is
        // queued after us.
        auto lock = TextureCacheTestAccess::Lock(textures);
        callback_started.release();
        callback_gate.acquire();
      });
      textures.ProcessDownloadImages();
      Require(name, "live image download pending",
              textures.HasPendingDownload(image_address, 8) &&
                  TextureCacheTestAccess::Contains(textures, image) &&
                  textures.GetImage(image).IsTracked(),
              "linear readback did not preserve its live image and pending "
              "publication");
      const auto tick = scheduler.CurrentTick();
      textures.InvalidateMemory(base + 0x4000, sizeof(uint32_t));
      Require(name, "unrelated invalidation stays local",
              scheduler.CurrentTick() == tick,
              "invalidation without an overlapping pending download submitted "
              "GPU work");
      // Leave the download unsubmitted: the CPU fault must request its
      // producer's completion.
    });

    std::jthread writer([&] {
      writer_started.release();
      if (neighboring_fault) {
        // This first store lies outside the image but releases a watcher on the
        // same page. The following image store must not race the previously
        // recorded writeback.
        *reinterpret_cast<volatile uint32_t *>(base) = 0xabcdef01u;
      }
      *reinterpret_cast<volatile uint32_t *>(image_address) = cpu_value;
      writer_done.release();
    });
    writer_started.acquire();
    Require(name, "fault submitted pending work",
            callback_started.try_acquire_for(std::chrono::seconds(5)),
            "CPU invalidation failed to submit the pending download or held "
            "the texture lock");
    MEMORY_BASIC_INFORMATION mapping{};
    const bool remained_protected =
        VirtualQuery(reinterpret_cast<void *>(image_address), &mapping,
                     sizeof(mapping)) == sizeof(mapping) &&
        (mapping.Protect & 0xffu) == PAGE_READONLY;
    const bool wrote_before_publication =
        writer_done.try_acquire_for(std::chrono::milliseconds(100));
    callback_gate.release();
    if (!wrote_before_publication) {
      Require(
          name, "live-image writer resumed",
          writer_done.try_acquire_for(std::chrono::seconds(5)),
          "CPU writer remained blocked after the pending image publication");
    }
    writer.join();

    context.GetGpu().SendCommandSync([&] {
      auto &scheduler = context.GetCommandScheduler();
      scheduler.Wait(scheduler.CurrentTick());
      scheduler.WaitPriorityOperations(scheduler.CurrentTick() - 1);
      std::array<uint32_t, 2> observed{};
      Require(
          name, "CPU store follows complete live-image publication",
          remained_protected && !wrote_before_publication &&
              LibKernel::Memory::TryReadBacking(image_address, observed.data(),
                                                sizeof(observed)) &&
              observed[0] == cpu_value && observed[1] == 0x04030201u &&
              !textures.HasPendingDownload(image_address, sizeof(observed)) &&
              TextureCacheTestAccess::Contains(textures, image),
          "live-image writeback clobbered the CPU store, lost GPU bytes, or "
          "retired the image");
      const auto tick = scheduler.CurrentTick();
      textures.InvalidateMemory(image_address, sizeof(uint32_t));
      Require(name, "completed invalidation stays local",
              scheduler.CurrentTick() == tick,
              "invalidation submitted GPU work after all overlapping downloads "
              "completed");
    });
    LibKernel::Memory::InstallGpuResources(nullptr);
    std::printf("[host]    %-32s ok\n", name);
  }
#else
  (void)vulkan;
#endif
}

void RunBdaBenchmark(VulkanHarness &vulkan) {
  constexpr const char *name = "BdaBenchmark";
  constexpr uint64_t base = 0x00000002e0000000ull;
  constexpr uint64_t buffer_count = 512;
  constexpr uint64_t buffer_size = BufferCache::CACHING_PAGESIZE;
  constexpr uint64_t stride = buffer_size * 2;
  constexpr size_t batches = 32;
  constexpr size_t passes_per_batch = 64;
  PerformanceMemoryFixture fixture(vulkan, name, base, buffer_count * stride);
  auto &context = fixture.context;
  context.MapMemory(base, fixture.size);
  context.GetGpu().SendCommandSync([&] {
    auto &cache = context.GetBufferCache();
    auto &scheduler = context.GetCommandScheduler();
    RangeSet mapped;
    mapped.Add(base, fixture.size);
    for (uint64_t index = 0; index < buffer_count; ++index) {
      (void)cache.FindBuffer(base + index * stride, buffer_size);
    }
    // Match the original discovery path exactly, without a dirty-hint scan.
    const auto reference_full_scan = [&] {
      mapped.ForEach([&](uint64_t begin, uint64_t end) {
        cache.SynchronizeBuffersInRange(begin, end - begin);
      });
    };
    reference_full_scan();
    scheduler.Wait(scheduler.CurrentTick());
    Require(name, "warm selective pass", cache.SynchronizeBdaSelective(mapped),
            "warm selective discovery failed");
    for (size_t warm = 0; warm < 16; ++warm) {
      reference_full_scan();
      Require(name, "warm discovery", cache.SynchronizeBdaSelective(mapped),
              "warm selective discovery failed");
    }
    std::array<double, batches> reference_us{};
    std::array<double, batches> selective_us{};
    const auto tick = scheduler.CurrentTick();
    const auto measure = [&](bool selective) {
      const auto begin = std::chrono::steady_clock::now();
      for (size_t pass = 0; pass < passes_per_batch; ++pass) {
        if (selective) {
          Require(name, "measured discovery",
                  cache.SynchronizeBdaSelective(mapped),
                  "selective discovery failed during measurement");
        } else {
          reference_full_scan();
        }
      }
      return std::chrono::duration<double, std::micro>(
                 std::chrono::steady_clock::now() - begin)
                 .count() /
             passes_per_batch;
    };
    for (size_t batch = 0; batch < batches; ++batch) {
      // Reverse the order every batch; both paths inspect the same unchanged
      // warmed owners.
      if ((batch & 1u) == 0) {
        reference_us[batch] = measure(false);
        selective_us[batch] = measure(true);
      } else {
        selective_us[batch] = measure(true);
        reference_us[batch] = measure(false);
      }
    }
    Require(name, "unchanged warmed workload",
            scheduler.CurrentTick() == tick &&
                cache.CheckBdaHintInvariant(mapped),
            "benchmark unexpectedly submitted GPU work or lost dirty coverage");
    const auto mean = [](const auto &samples) {
      return std::accumulate(samples.begin(), samples.end(), 0.0) /
             samples.size();
    };
    const auto reference_mean = mean(reference_us);
    const auto selective_mean = mean(selective_us);
    std::ranges::sort(reference_us);
    std::ranges::sort(selective_us);
    std::printf(
        "[bench] BDA buffers=%llu passes_per_mode=%zu "
        "batches=%zu passes_per_batch=%zu\n"
        "[bench] reference full scan: mean_us_per_pass=%.3f "
        "median_batch_average_us_per_pass=%.3f\n"
        "[bench] selective: mean_us_per_pass=%.3f "
        "median_batch_average_us_per_pass=%.3f\n",
        static_cast<unsigned long long>(buffer_count),
        batches * passes_per_batch, batches, passes_per_batch, reference_mean,
        (reference_us[batches / 2 - 1] + reference_us[batches / 2]) / 2,
        selective_mean,
        (selective_us[batches / 2 - 1] + selective_us[batches / 2]) / 2);
    const auto first = fixture.ReadNative(base, buffer_size);
    const auto last =
        fixture.ReadNative(base + (buffer_count - 1) * stride, buffer_size);
    Require(name, "native contents preserved",
            std::ranges::all_of(first,
                                [](uint8_t value) { return value == 0x5a; }) &&
                std::ranges::all_of(
                    last, [](uint8_t value) { return value == 0x5a; }),
            "warmed discovery changed native cached contents");
  });
}

void CheckPerformanceMemory(VulkanHarness &vulkan) {
  CheckBdaSelectiveMemory(vulkan);
  CheckOversizedBufferReadback(vulkan);
  CheckPredicateMemoryProducers(vulkan);
  CheckPendingImagePredicates(vulkan);
  CheckOversizedImageReadback(vulkan);
  CheckConservativeImagePressure(vulkan);
  CheckTiledImagePressure(vulkan);
  CheckImageWritebackFaultOrdering(vulkan);
  CheckLiveImageWritebackFaultOrdering(vulkan);
}

} // namespace
} // namespace Libs::Graphics
