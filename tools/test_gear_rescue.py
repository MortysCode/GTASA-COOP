"""Pure inventory and revive state tests. No GTA process or game saves are used."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
if __name__ == "__main__":
    compiler = ROOT / "toolchain/llvm-mingw/bin/i686-w64-mingw32-clang++.exe"
    exe = ROOT / "build/release/bin/gear-rescue-test.exe"
    subprocess.run([str(compiler), "-std=c++20", "-O2", "-Wall", "-Wextra", "-static", "-Isrc",
                    "tools/tests/gear_rescue_test.cpp", "-o", str(exe)], cwd=ROOT, check=True)
    subprocess.run([str(exe)], cwd=ROOT, check=True)
