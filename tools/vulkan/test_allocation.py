#!/usr/bin/env python3
"""Fault-test the production Vulkan containers and pipeline publication helper.

Requires Python 3 and a GCC/Clang-compatible C compiler (CC, or cc) with address
and undefined-behavior sanitizers. Both release and diagnostic configurations
are tested. No Vulkan SDK, GPU, core, frontend, or game data is used.
"""

import os
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[2]
    source = (root / "rhi/rhi_lib_vulkan.c").read_text(encoding="utf-8")
    container_start = source.index("   struct IntrusiveListNode\n")
    container_end = source.index(
        "   /* Concrete temporary-hashmap node base", container_start)
    publication_start = source.index("   static VkPipeline program_add_pipeline(")
    publication_end = source.index("   static void program_fini(", publication_start)

    with tempfile.TemporaryDirectory(prefix="beetle-vulkan-allocation-") as tmp:
        work = Path(tmp)
        (work / "allocation_under_test.h").write_text(
            source[container_start:container_end], encoding="utf-8")
        (work / "publication_under_test.h").write_text(
            source[publication_start:publication_end], encoding="utf-8")
        for diagnostics in (False, True):
            mode = "diagnostic" if diagnostics else "release"
            executable = work / (mode + (".exe" if os.name == "nt" else ""))
            command = [
                os.environ.get("CC", "cc"), "-std=c89", "-g", "-Wall", "-Wextra",
                "-Werror", "-Werror=declaration-after-statement",
                "-Wno-unused-function", "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer", "-I", str(work),
                str(root / "tools/vulkan/allocation_test.c"),
                "-o", str(executable),
            ]
            if diagnostics:
                command.append("-DBEETLE_VULKAN_PIPELINE_DIAGNOSTICS")
            subprocess.run(command, check=True)
            print("Testing " + mode + " allocator/publication paths", flush=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
