"""AFK protocol / damage response tests; no game process or saves are touched."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent

if __name__ == "__main__":
    compiler = ROOT / "toolchain/llvm-mingw/bin/i686-w64-mingw32-clang++.exe"
    exe = ROOT / "build/release/bin/afk-test.exe"
    subprocess.run([str(compiler), "-std=c++20", "-O2", "-Wall", "-Wextra", "-static",
                    "-Isrc", "tools/tests/afk_test.cpp", "-o", str(exe)], cwd=ROOT, check=True)
    subprocess.run([str(exe)], cwd=ROOT, check=True)
