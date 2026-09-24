#ifndef KYTY_GRAPHICS_SHADER_PRECOMPILE_H_
#define KYTY_GRAPHICS_SHADER_PRECOMPILE_H_

#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/shaderCompiler.h"

#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace Libs::Graphics::ShaderPrecompile {

// Recompile from code and module-affecting metadata, without reading guest memory. User SGPR
// contents and resource addresses are runtime inputs; the compiler needs only the SGPR count.
struct PermutationRecord {
	ShaderType                                   stage                 = ShaderType::Unknown;
	uint64_t                                     hash                  = 0;
	uint32_t                                     push_data_start_dword = 0;
	uint32_t                                     user_data_base        = 0;
	uint32_t                                     user_data_count       = 0;
	uint32_t                                     wave_size             = 64;
	std::vector<uint32_t>                        code;
	std::vector<uint32_t>                        back_code;
	ShaderRecompiler::IR::ResourceSpecialization specialization;
	std::variant<ShaderVertexInputInfo, ShaderPixelInputInfo, ShaderComputeInputInfo> info;
};

template <typename Info>
PermutationRecord Capture(const ShaderParams&                                 params,
                          const ShaderRecompiler::CompileOptions&             options,
                          const ShaderRecompiler::IR::ResourceSpecialization& specialization,
                          uint32_t push_data_start_dword, const Info& info) {
	PermutationRecord record;
	record.stage                 = options.stage;
	record.hash                  = options.shader_hash;
	record.push_data_start_dword = push_data_start_dword;
	record.user_data_base        = options.user_data_base;
	record.user_data_count       = static_cast<uint32_t>(options.user_data.size());
	record.wave_size             = options.wave_size;
	record.code.assign(params.code.begin(), params.code.end());
	record.back_code.assign(params.back_code.begin(), params.back_code.end());
	record.specialization = specialization;
	record.info           = info;
	std::visit([](auto& stage) { stage.stage = {}; }, record.info);
	return record;
}

// The codec writes individual scalars in little-endian order. It never copies a C++ object's
// representation, padding, runtime pointers, or vertex descriptor/buffer addresses.
bool Encode(const PermutationRecord& record, std::vector<uint8_t>& bytes);
bool Decode(std::span<const uint8_t> bytes, PermutationRecord& record);

// A journal belongs to one PipelineCache; its caller serializes Append/Close with that cache.
// Open validates the header/checksums and repairs a torn tail before permitting appends. A
// failed repair leaves recording disabled, while previously validated records can still replay.
class Journal {
public:
	Journal() = default;
	~Journal() { Close(); }
	Journal(const Journal&)            = delete;
	Journal& operator=(const Journal&) = delete;

	std::vector<PermutationRecord> Open(const std::filesystem::path& path, std::string_view key);
	bool                           Append(const PermutationRecord& record);
	void                           Close();
	[[nodiscard]] bool             IsOpen() const { return m_out.is_open(); }
	[[nodiscard]] uint64_t         RecordedCount() const { return m_count; }

private:
	std::ofstream m_out;
	uint64_t      m_count = 0;
	uint64_t      m_bytes = 0;
};

} // namespace Libs::Graphics::ShaderPrecompile

#endif
