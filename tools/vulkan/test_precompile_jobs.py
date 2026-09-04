#!/usr/bin/env python3
"""Test production precompile batching against a deterministic fake driver."""

import os
from pathlib import Path
import re
import subprocess
import tempfile


def definition(source, name, kind="function"):
    if kind == "function":
        pattern = (r"(?:static\s+)?[\w\s*]+\b" + re.escape(name) +
                   r"\s*\([^;{}]*\)\s*\{")
    else:
        pattern = r"\b" + kind + r"\s+" + re.escape(name) + r"\s*\{"
    match = re.search(pattern, source)
    if not match:
        raise RuntimeError("Production definition not found: " + name)
    opening = source.index("{", match.start())
    tokens = re.compile(
        r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',
        re.S)
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
                return source[match.start():end].strip() + "\n"
    raise RuntimeError("Unclosed production definition: " + name)


def main():
    root = Path(__file__).resolve().parents[2]
    source = (root / "rhi/rhi_lib_vulkan.c").read_text(encoding="utf-8")
    type_names = (
        "VulkanPipelineRecipe", "VulkanPipelineRecipeVec",
        "VulkanGraphicsPrecompileJob", "VulkanGraphicsPrecompileJobVec",
        "VulkanComputePrecompileJob", "VulkanComputePrecompileJobVec",
        "VulkanPrecompileContext", "VulkanPrecompileWorker",
        "VulkanPrecompileQueue",
    )
    type_text = definition(source, "ShaderStage", "enum")
    type_text += "".join(definition(source, name, "struct")
                        for name in type_names)
    enum_start = source.rfind("   enum\n", 0,
                              source.index("VulkanPrecompileBatchSize ="))
    enum_end = source.index(";", source.index(
        "VulkanPrecompileMaxWorkers =", enum_start)) + 1
    type_text += source[enum_start:enum_end] + "\n"
    worker_text = definition(source, "vulkan_precompile_worker")
    worker_text += definition(source, "vulkan_precompile_pending_jobs")

    with tempfile.TemporaryDirectory(prefix="beetle-vulkan-jobs-") as tmp:
        work = Path(tmp)
        (work / "precompile_types_under_test.h").write_text(
            type_text, encoding="utf-8")
        (work / "precompile_worker_under_test.h").write_text(
            worker_text, encoding="utf-8")
        executable = work / (
            "precompile_jobs_test.exe" if os.name == "nt" else
            "precompile_jobs_test")
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c99", "-O2", "-Wall",
            "-Wextra", "-Werror", "-Wno-unused-function",
            "-I", str(work),
            "-I", str(root / "parallel-psx/khronos/include"),
            str(root / "tools/vulkan/precompile_jobs_test.c"),
            "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
