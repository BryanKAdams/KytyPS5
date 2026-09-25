#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_DCCCLEARRESOLVER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_DCCCLEARRESOLVER_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <cstdint>
#include <memory>

namespace Libs::Graphics {

struct GraphicContext;
class CommandScheduler;

// Decides on the GPU whether GPU-written DCC metadata slices hold a fast clear. The check writes
// one predicate per (slice, clear code); callers wrap each candidate clear in conditional
// rendering, so the CPU never waits for the metadata bytes.
class DccClearResolver {
public:
	static constexpr uint32_t CodeCount = 5;
	static constexpr uint32_t MaxSlices = 64;

	DccClearResolver(GraphicContext& graphics, CommandScheduler& scheduler);
	~DccClearResolver();
	KYTY_CLASS_NO_COPY(DccClearResolver);

	[[nodiscard]] bool Available() const noexcept { return m_pipeline != nullptr; }

	// Code index used by the predicate layout, or CodeCount for a code that never clears.
	[[nodiscard]] static uint32_t CodeIndex(uint8_t code) noexcept;
	[[nodiscard]] static uint8_t  Code(uint32_t index) noexcept;

	// Records the check of `slices` consecutive slices of `slice_size` bytes starting at
	// `metadata_offset` in `metadata`. `code_mask` selects the decodable codes; `consume`
	// rewrites cleared slices to 0xff. Must be recorded outside a rendering instance.
	void Record(vk::CommandBuffer command, const Buffer& metadata, uint64_t metadata_offset,
	            uint64_t slice_size, uint32_t slices, uint32_t code_mask, bool consume);

	[[nodiscard]] vk::Buffer PredicateBuffer() const noexcept;
	[[nodiscard]] static uint64_t PredicateOffset(uint32_t slice, uint32_t code_index) noexcept {
		return (static_cast<uint64_t>(slice) * CodeCount + code_index) * sizeof(uint32_t);
	}

private:
	GraphicContext&         m_graphics;
	vk::DescriptorSetLayout m_descriptor_layout = nullptr;
	vk::PipelineLayout      m_pipeline_layout   = nullptr;
	vk::Pipeline            m_pipeline          = nullptr;
	std::unique_ptr<Buffer> m_predicates;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_DCCCLEARRESOLVER_H_
