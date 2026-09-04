#!/usr/bin/env python3
"""Exercise the real persistence helper block with deterministic driver/VFS faults.

Run on a POSIX host (including WSL): python3 tools/vulkan_cache/test_persistence.py
Use --windows under WSL to cross-compile with MinGW and execute natively on Windows.
No Vulkan SDK, GPU, core build, frontend configuration, or game data is needed.
"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def function_definition(source, signature):
    """Return one complete C function definition containing signature."""
    start = source.index(signature)
    start = source.rfind("\n", 0, start) + 1
    brace = source.index("{", start)
    depth = 0
    state = "code"
    index = brace
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            elif char == "/" and next_char == "*":
                state = "comment"
                index += 1
            elif char == "/" and next_char == "/":
                state = "line_comment"
                index += 1
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return source[start:index + 1] + "\n"
        elif state in ("string", "character"):
            if char == "\\":
                index += 1
            elif (state == "string" and char == '"') or (
                    state == "character" and char == "'"):
                state = "code"
        elif state == "comment" and char == "*" and next_char == "/":
            state = "code"
            index += 1
        elif state == "line_comment" and char == "\n":
            state = "code"
        index += 1
    raise ValueError(f"unterminated function containing {signature!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--windows", action="store_true")
    parser.add_argument("--diagnostics", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = (root / "rhi/rhi_lib_vulkan.c").read_text(encoding="utf-8")
    filestream_source = (root / "libretro-common/streams/file_stream.c").read_text(
        encoding="utf-8")
    start = source.index('#define VULKAN_PIPELINE_CACHE_DIR_NAME ')
    end = source.index('   static void device_init(Device *self)', start)
    # Windows interop needs the executable and test data on the mounted drive.
    with tempfile.TemporaryDirectory(prefix="beetle-vulkan-cache-test-",
                                     dir=root if args.windows else None) as tmp:
        work = Path(tmp)
        (work / "persistence_under_test.h").write_text(
            source[start:end], encoding="utf-8")
        (work / "atomic_write_under_test.h").write_text(
            function_definition(filestream_source,
                                "bool filestream_write_file_atomic("),
            encoding="utf-8")
        executable = work / ("persistence_test.exe" if args.windows else "persistence_test")
        compiler = "x86_64-w64-mingw32-gcc" if args.windows else "cc"
        flags = ["-DBEETLE_VULKAN_PIPELINE_DIAGNOSTICS"] if args.diagnostics else []
        subprocess.run([
            os.environ.get("CC", compiler), "-std=c99", "-O2", "-Wall", "-Wextra",
            "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
            "-Wno-missing-field-initializers", "-I", str(work),
            "-I", str(root / "libretro-common/include"), *flags,
            str(root / "tools/vulkan_cache/persistence_test.c"),
            "-o", str(executable),
        ], check=True)
        test_directory = str(work)
        if args.windows:
            test_directory = subprocess.check_output(
                ["wslpath", "-w", str(work)], text=True).strip()
        subprocess.run([str(executable), test_directory], check=True)


if __name__ == "__main__":
    main()
