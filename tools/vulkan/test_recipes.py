#!/usr/bin/env python3
"""Run CPU-only regression tests against extracted production Vulkan helpers.

Requires Python 3 and a C99 compiler (CC, or cc). No core, GPU, SDK, ROM, or
frontend configuration is built or used. Temporary generated files are removed.
"""

import os
from pathlib import Path
import re
import subprocess
import tempfile


def definition(source, name, kind="function"):
    if kind == "function":
        pattern = r"(?:static\s+)?(?:INLINE\s+|RHI_INLINE\s+)?[\w\s*]+\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{"
    else:
        pattern = r"\b" + kind + r"\s+" + re.escape(name) + r"\s*\{"
    match = re.search(pattern, source)
    if not match:
        raise RuntimeError("Production definition not found: " + name)
    start = match.start()
    opening = source.index("{", match.start())
    tokens = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    depth = 0
    for token in tokens.finditer(source, opening):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                end = token.end()
                if kind != "function":
                    end = source.index(";", end) + 1
                return source[start:end].strip() + "\n"
    raise RuntimeError("Unclosed production definition: " + name)


def macro(source, name):
    match = re.search(r"^#define " + re.escape(name) + r"\b[^\n]*(?:\\\n[^\n]*)*", source, re.M)
    if not match:
        raise RuntimeError("Production macro not found: " + name)
    # Read continued physical lines rather than letting a greedy regex swallow
    # the first backslash and truncate a multi-line definition.
    lines = source[match.start():].splitlines()
    result = []
    for line in lines:
        result.append(line)
        if not line.rstrip().endswith("\\"):
            break
    return "\n".join(result) + "\n"


def main():
    root = Path(__file__).resolve().parents[2]
    source = (root / "rhi/rhi_lib_vulkan.c").read_text(encoding="utf-8")
    types = [
        ("struct", "State"), ("union", "PipelineState"),
        ("enum", "ShaderStage"),
        ("struct", "VulkanPipelineRecipe"),
        ("struct", "VulkanPipelineRecipeVec"),
        ("struct", "VulkanGraphicsPrecompileJob"),
        ("struct", "VulkanGraphicsPrecompileJobVec"),
        ("struct", "VulkanComputePrecompileJob"),
        ("struct", "VulkanComputePrecompileJobVec"),
        ("struct", "VulkanPrecompileContext"),
        ("struct", "PotentialState"), ("struct", "VulkanPrecompileConfiguration"),
        ("struct", "TextureWindow"), ("enum", "SemiTransparentMode"),
        ("enum", "FilterMode"), ("enum", "TransMode"), ("enum", "BlendMode"),
        ("struct", "SemiTransparentState"), ("struct", "BufferVertex"),
        ("enum", "SpecConstIndex"),
        ("enum", "ImageDomain"), ("enum", "Layout"),
        ("struct", "ImageCreateInfo"), ("enum", "RenderPassOp"),
        ("enum", "RenderPassInfo_DepthStencil"),
        ("struct", "RenderPassInfo_Subpass"), ("struct", "RenderPassInfo"),
    ]
    # This enum is anonymous in production. Preserve its actual numeric aliases.
    spec_start = source.rfind("enum", 0, source.index("SpecConstIndex_TransMode ="))
    spec_end = source.index("};", spec_start) + 2
    type_text = "".join(definition(source, name, kind)
                        for kind, name in types if name != "SpecConstIndex")
    type_text += source[spec_start:spec_end] + "\n"
    type_text += macro(source, "RENDERER_PIPELINE_MANIFEST")
    type_text += definition(source, "RendererPipelineManifestId", "enum")
    hash_text = definition(source, "Hasher", "struct")
    for name in ("hasher_init", "hasher_u32", "hasher_u64", "hasher_data", "hasher_get"):
        hash_text += definition(source, name)
    hash_text += macro(source, "FOR_EACH_BIT")
    hash_text += definition(source, "commandbuffer_pipeline_recipe_hash")
    state_text = macro(source, "SET_STATIC_STATE") + macro(source, "SET_POTENTIALLY_STATIC_STATE")
    for name in (
        "commandbuffer_set_depth_test", "commandbuffer_set_depth_compare",
        "commandbuffer_set_blend_enable", "commandbuffer_set_blend_factors",
        "commandbuffer_set_blend_op", "commandbuffer_set_primitive_topology",
        "commandbuffer_set_multisample_state", "commandbuffer_set_cull_mode",
        "commandbuffer_set_blend_constants", "commandbuffer_set_specialization_constant_mask",
        "commandbuffer_set_specialization_constant", "commandbuffer_set_vertex_attrib",
        "commandbuffer_set_opaque_state", "commandbuffer_set_quad_state",
        "renderer_set_opaque_primitive_spec_constants", "renderer_set_primitive_vertex_layout",
        "renderer_set_opaque_primitive_pipeline_state", "renderer_set_semi_transparent_pipeline_state",
        "renderer_set_primitive_texture_pipeline", "renderer_semi_trans_needs_feedback",
        "renderer_semi_trans_batch_wants_sub_floor", "renderer_set_sub_floor_pipeline_state",
        "renderer_semi_transparent_set_pipeline_state", "renderer_set_resolve_pipeline_state",
    ):
        state_text += definition(source, name)
    capture_text = "".join(definition(source, name) for name in (
        "renderer_precompile_opaque_variant", "renderer_precompile_semi_transparent_variant",
        "renderer_precompile_texture_variants", "renderer_precompile_primitive_pipelines",
        "renderer_precompile_quad_pipeline", "renderer_precompile_compute_pipeline",
        "renderer_scanout_program", "renderer_vram_scanout_program",
        "renderer_precompile_manifest_program_is_active",
        "renderer_precompile_manifest_nonprimitive_pipelines",
        "renderer_precompile_if_configuration_changed",
    ))
    validation_text = "".join(definition(source, name) for name in (
        "vulkan_precompile_expect_recipe", "vulkan_precompile_validate_recipes",
        "device_precompile_note_runtime_escape"))
    renderpass_text = "".join(definition(source, name) for name in (
        "image_create_info_defaults", "image_create_info_render_target",
        "render_pass_info_defaults", "render_pass_info_subpass_defaults",
        "device_request_render_pass", "renderer_precompile_render_pass"))
    # Input updates must be adopted before the safe precompile boundary.
    run = definition((root / "libretro.c").read_text(encoding="utf-8"), "retro_run")
    refresh = run.index("check_variables(false)")
    if "prepare_after_variables" in run:
        assert "prepare_after_variables = rhi_intf_is_type() == RHI_VULKAN" in run
        assert run.index("if (!prepare_after_variables)") < run.index("rhi_intf_prepare_frame()") < refresh
        assert refresh < run.index("if (prepare_after_variables)") < run.rindex("rhi_intf_prepare_frame()")
    else:
        assert refresh < run.index("rhi_intf_prepare_frame()")

    # A defensive renderer-null return must not leave a frame active, and no
    # driver-cache serialization or file I/O belongs in the per-frame path.
    prepare = definition(source, "rhi_vulkan_prepare_frame")
    assert prepare.index("if (renderer == NULL)") < prepare.index("inside_frame = true")
    assert "device_pipeline_cache_save" not in prepare
    assert "device_pipeline_cache_checkpoint" not in prepare

    # An empty job queue is the expected warm-cache case. Capture and exact
    # recipe validation, not a nonzero job count, decide completeness.
    finish = definition(source, "vulkan_precompile_jobs_finish")
    completion = finish[:finish.index("for (i = 0; i < jobs->count; i++)")]
    assert "total_count != 0" not in completion

    # Every attempt owns its capture vectors. The Device pointer is only a
    # scoped bridge for the existing command-buffer path, never global state.
    precompile = definition(
        source, "renderer_precompile_current_configuration_pipelines")
    assert "static bool vulkan_pipeline_precompiling" not in source
    assert "self->device->precompile_context = &context" in precompile
    assert "self->device->precompile_context = NULL" in precompile
    # Partial success is valuable cache data. The dirty-count gate inside the
    # save helper suppresses empty/unchanged attempts; plan completeness must
    # not suppress persistence of pipelines that did publish successfully.
    assert "device_pipeline_cache_save(self->device)" in precompile
    assert "if (complete)\n      device_pipeline_cache_save" not in precompile

    # The frontend-facing application info still requests Vulkan 1.0. Cache
    # control must therefore use the enabled KHR feature-query path, not infer
    # permission to call a promoted core command from the physical GPU version.
    create_device = definition(source, "context_create_device")
    assert '"vkGetPhysicalDeviceFeatures2KHR"' in create_device
    assert '"vkGetPhysicalDeviceFeatures2"' not in create_device

    option_text = (root / "libretro_core_options.h").read_text(encoding="utf-8")
    option_start = option_text.index("BEETLE_OPT(vulkan_shader_precompilation)")
    option_end = option_text.index("\n   },", option_start)
    assert "Restart required" not in option_text[option_start:option_end]
    with tempfile.TemporaryDirectory(prefix="beetle-vulkan-recipes-") as tmp:
        work = Path(tmp)
        for name, text in (("types", type_text), ("hash", hash_text),
                           ("state", state_text), ("capture", capture_text),
                           ("validation", validation_text), ("renderpass", renderpass_text)):
            (work / (name + "_under_test.h")).write_text(text, encoding="utf-8")
        executable = work / ("recipes_test.exe" if os.name == "nt" else "recipes_test")
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c99", "-O2", "-Wall", "-Wextra",
            "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
            "-I", str(work), "-I", str(root / "parallel-psx/khronos/include"),
            str(root / "tools/vulkan/recipes_test.c"), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
