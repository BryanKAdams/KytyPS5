// Include at global scope after the GPU test helpers; call
// CheckAttachmentFeedbackPipelineKeys().
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include <unordered_map>

namespace Libs::Graphics {

struct AttachmentFeedbackTestAccess {
  using Key = PipelineCache::GraphicsPipelineKey;
  using Hash = PipelineCache::GraphicsPipelineKeyHash;
};

namespace {

void CheckAttachmentFeedbackPipelineKeys() {
  constexpr const char *name = "AttachmentFeedbackPipelineKeys";
  using Key = AttachmentFeedbackTestAccess::Key;
  using Hash = AttachmentFeedbackTestAccess::Hash;
  const vk::ImageAspectFlags depth = vk::ImageAspectFlagBits::eDepth;
  const vk::ImageAspectFlags stencil = vk::ImageAspectFlagBits::eStencil;
  const vk::ImageAspectFlags color = vk::ImageAspectFlagBits::eColor;
  const vk::PipelineCreateFlags depth_flag =
      vk::PipelineCreateFlagBits::eDepthStencilAttachmentFeedbackLoopEXT;
  const vk::PipelineCreateFlags color_flag =
      vk::PipelineCreateFlagBits::eColorAttachmentFeedbackLoopEXT;
  const auto key_for = [&](vk::ImageAspectFlags aspects, bool dynamic_enabled) {
    Key key{};
    key.vertex_shader_ids[0] = 17;
    key.ps_shader_id = 18;
    key.rendering.depth_format = vk::Format::eD32SfloatS8Uint;
    key.rendering.stencil_format = vk::Format::eD32SfloatS8Uint;
    key.static_params.attachment_feedback_loop_flags =
        AttachmentFeedbackPipelineFlags(aspects, dynamic_enabled);
    return key;
  };

  const auto none = key_for({}, false);
  const auto depth_key = key_for(depth, false);
  const auto stencil_key = key_for(stencil, false);
  const auto depth_stencil_key = key_for(depth | stencil, false);
  const auto color_key = key_for(color, false);
  const auto combined_key = key_for(color | depth | stencil, false);
  Require(name, "static flags",
          !none.static_params.attachment_feedback_loop_flags &&
              depth_key.static_params.attachment_feedback_loop_flags ==
                  depth_flag &&
              stencil_key.static_params.attachment_feedback_loop_flags ==
                  depth_flag &&
              color_key.static_params.attachment_feedback_loop_flags ==
                  color_flag &&
              combined_key.static_params.attachment_feedback_loop_flags ==
                  (color_flag | depth_flag),
          "static attachment feedback flags do not match the required aspects");

  // Exercise the production equality and hash, including the appended packed
  // field.
  Require(name, "static cache identity",
          !(none == depth_key) && !(none == color_key) &&
              !(depth_key == combined_key) && depth_key == stencil_key &&
              depth_key == depth_stencil_key &&
              Hash{}(none) != Hash{}(depth_key) &&
              Hash{}(depth_key) == Hash{}(stencil_key),
          "feedback variants collide with the ordinary key or unnecessarily "
          "split depth/stencil");
  std::unordered_map<Key, uint32_t, Hash> pipelines;
  pipelines.emplace(none, 1);
  pipelines.emplace(depth_key, 2);
  pipelines.emplace(stencil_key, 3);
  pipelines.emplace(depth_stencil_key, 4);
  pipelines.emplace(color_key, 5);
  pipelines.emplace(combined_key, 6);
  Require(name, "static cache reuse",
          pipelines.size() == 4 && pipelines.at(none) == 1 &&
              pipelines.at(stencil_key) == 2 && pipelines.at(combined_key) == 6,
          "static feedback toggles reused an incompatible pipeline");

  // A feedback-capable layout alone must not enable static flags: the sampled
  // subresource and an attachment write must actually overlap.
  ImageViewInfo target{};
  target.aspect = depth | stencil;
  target.base_level = 1;
  target.level_count = 1;
  target.base_layer = 2;
  target.layer_count = 1;
  auto sampled = target;
  sampled.aspect = depth;
  Require(name, "overlapping depth write",
          key_for(DepthFeedbackAspects(depth, target, sampled), false) ==
              depth_key,
          "overlapping sampled depth writes lost their static flag");
  Require(name, "read-only attachment",
          key_for(DepthFeedbackAspects({}, target, sampled), false) == none,
          "read-only depth sampling unnecessarily enabled static feedback");
  Require(
      name, "separate aspects",
      key_for(DepthFeedbackAspects(stencil, target, sampled), false) == none,
      "stencil writes unnecessarily enabled feedback for depth-only sampling");
  sampled.base_layer = 3;
  Require(name, "disjoint layers",
          key_for(DepthFeedbackAspects(depth, target, sampled), false) == none,
          "disjoint sampled layers unnecessarily enabled static feedback");
  sampled.base_layer = target.base_layer;
  sampled.base_level = 2;
  Require(name, "disjoint mips",
          key_for(DepthFeedbackAspects(depth, target, sampled), false) == none,
          "disjoint sampled mips unnecessarily enabled static feedback");

  // Dynamic state supersedes both static flags. Aspect changes must reuse one
  // pipeline key even when switching between color, depth and stencil feedback.
  pipelines.clear();
  pipelines.emplace(key_for({}, true), 7);
  for (const auto aspects :
       {depth, stencil, color, depth | stencil, color | depth | stencil}) {
    const auto dynamic_key = key_for(aspects, true);
    Require(name, "dynamic normalization",
            dynamic_key == none && Hash{}(dynamic_key) == Hash{}(none) &&
                !pipelines.emplace(dynamic_key, 8).second &&
                pipelines.at(dynamic_key) == 7,
            "dynamic feedback created a redundant static pipeline variant");
  }
  Require(name, "dynamic cache size", pipelines.size() == 1,
          "dynamic feedback did not share one pipeline");
  std::printf("[host]    %-32s ok\n", name);
}

} // namespace
} // namespace Libs::Graphics
