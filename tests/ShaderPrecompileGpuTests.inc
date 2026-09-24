// Include at global scope after ShaderRecompilerComputeTests.cpp's helpers,
// before main.
#include "graphics/host_gpu/renderer/pipeline/shaderPrecompile.h"

namespace Libs::Graphics {
namespace {

void CheckShaderPrecompileGpu(VulkanHarness &vulkan) {
  constexpr const char *name = "ShaderPrecompileGpu";
  using namespace ShaderRecompiler;

  // Store a LIVE SGPR value using a descriptor in s[0:3]. In contrast to a
  // constant-fill shader, this fails if replay ever freezes recorded SGPR
  // values or loses their count.
  std::vector<u32> code{
      EncodeVop1(0x01, 0, 4),            // v_mov_b32 v0, s4
      EncodeVop1(0x01, 1, InlineU32(0)), // v_mov_b32 v1, 0 (byte offset)
      EncodeMubuf0(0x1cu),               // buffer_store_dword v0, v1, s[0:3]
      EncodeMubuf1(0, 0, 1),
  };
  AppendEnd(&code);
  ShaderParams params;
  params.code = code;
  params.hash = 0x123456789abcdef0ull;
  params.user_data_count =
      6; // The backing array holds 40; only six are guest-visible.
  params.user_data[2] = 16;
  params.user_data[3] = DstSel(4, 5, 6, 7) | (1u << 24u);
  params.user_data[4] = 0x12345678u;

  ShaderComputeInputInfo input;
  input.threads_num[0] = 1;
  input.threads_num[1] = 1;
  input.threads_num[2] = 1;
  input.dispatch_threads_num[0] =
      37; // Must never become a baked shader constant.
  input.host_subgroup_size = vulkan.SubgroupSize();
  input.thread_ids_num = 1;
  input.workgroup_register = 6;

  for (const auto push_start : {7u, IR::PushData::NoStart}) {
    CompileOptions options;
    options.stage = ShaderType::Compute;
    options.shader_hash = params.hash;
    options.user_data =
        std::span(params.user_data).first(params.user_data_count);
    options.input_info.compute = &input;
    options.dump_ir = false;
    auto translated = TranslateProgram(params.code, options);
    auto live_plan = IR::ExtractResourcePlan(translated.program);
    IR::ResourceSnapshot live_resources;
    IR::ResourceSpecialization specialization;
    IR::SrtRuntime runtime{.user_data = options.user_data};
    Require(name, "live materialization",
            IR::MaterializeResources(live_plan, runtime, live_resources,
                                     specialization),
            "live shader descriptor could not be materialized");
    auto live_result = CompileProgram(std::move(translated), options,
                                      specialization, push_start);

    const auto captured = ShaderPrecompile::Capture(
        params, options, specialization, push_start, input);
    std::vector<uint8_t> bytes;
    ShaderPrecompile::PermutationRecord recorded;
    Require(name, "record round trip",
            ShaderPrecompile::Encode(captured, bytes) &&
                ShaderPrecompile::Decode(bytes, recorded),
            "valid shader record did not round trip");

    // This is the startup replay condition: there is no old guest memory and
    // all user SGPR contents are zero. Only count and compile metadata are
    // available until a live lookup.
    std::array<u32, 40> zeros{};
    CompileOptions replay_options;
    replay_options.stage = recorded.stage;
    replay_options.shader_hash = recorded.hash;
    replay_options.wave_size = recorded.wave_size;
    replay_options.user_data_base = recorded.user_data_base;
    replay_options.user_data = std::span(zeros).first(recorded.user_data_count);
    replay_options.back_code = recorded.back_code;
    replay_options.input_info.compute =
        &std::get<ShaderComputeInputInfo>(recorded.info);
    replay_options.dump_ir = false;
    auto replay_translated = TranslateProgram(recorded.code, replay_options);
    auto replay_plan = IR::ExtractResourcePlan(replay_translated.program);
    auto replay_result =
        CompileProgram(std::move(replay_translated), replay_options,
                       recorded.specialization, recorded.push_data_start_dword);
    ValidateSpirv(name, live_result.spirv);
    ValidateSpirv(name, replay_result.spirv);
    Require(name, "compile equivalence",
            live_result.spirv == replay_result.spirv &&
                live_result.program.info == replay_result.program.info &&
                live_result.program.bindings ==
                    replay_result.program.bindings &&
                replay_result.program.user_data_count == params.user_data_count,
            "recorded zero-SGPR replay changed SPIR-V, reflection, layout or "
            "user-data count");
    Require(name, "push placement",
            replay_result.program.bindings.push_data_start_dword == push_start,
            "replay did not preserve push placement or the storage fallback");

    CompiledShader live{
        std::move(live_result.spirv), std::move(live_result.program), {}, {}};
    CompiledShader replay{std::move(replay_result.spirv),
                          std::move(replay_result.program),
                          {},
                          {}};
    const auto bind_runtime = [](CompiledShader &shader,
                                 const IR::ResourceSnapshot &resources) {
      shader.resources = resources;
      shader.packed_user_data.clear();
      for (const auto reg : shader.program.bindings.user_data_registers) {
        shader.packed_user_data.push_back(
            resources.user_data.at(reg - shader.program.user_data_base));
      }
      // This test binds its native buffer at offset zero, including packed
      // memory offsets.
      shader.packed_user_data.resize(shader.program.bindings.ShaderDataDwords(),
                                     0u);
    };
    for (const auto expected : {0x12345678u, 0x87654321u}) {
      params.user_data[4] = expected;
      IR::ResourceSnapshot current;
      IR::ResourceSpecialization current_specialization;
      Require(name, "live refresh after replay",
              IR::MaterializeResources(replay_plan, runtime, current,
                                       current_specialization) &&
                  current_specialization == recorded.specialization,
              "runtime SGPR refresh changed module identity or failed after "
              "recorded replay");
      bind_runtime(live, current);
      bind_runtime(replay, current);
      TestCase test;
      test.name = name;
      test.initial = {0, 0, 0, 0};
      test.expected = {expected, 0, 0, 0};
      test.has_compute_info = true;
      test.compute_info = input;
      std::vector<u32> outputs[2];
      for (unsigned mode = 0; mode != 2; ++mode) {
        auto buffer =
            vulkan.CreateStorageBuffer(name, test.initial, test.initial.size());
        vulkan.Dispatch(test, mode == 0 ? live : replay, buffer);
        outputs[mode] = vulkan.ReadBuffer(name, buffer, test.expected.size());
        vulkan.DestroyBuffer(&buffer);
        CompareWords(test, mode == 0 ? "live readback" : "replay readback",
                     test.expected, outputs[mode]);
      }
      Require(name, "execution equivalence", outputs[0] == outputs[1],
              "live and replayed shader executions disagree");
    }
  }
  std::printf("[compute] %-32s ok\n", name);
}

} // namespace
} // namespace Libs::Graphics
