"""Verify camera-only control scopes without touching a running game."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
if __name__ == "__main__":
    compiler = ROOT / "toolchain/llvm-mingw/bin/i686-w64-mingw32-clang++.exe"
    exe = ROOT / "build/release/bin/control-locks-test.exe"
    subprocess.run([str(compiler), "-std=c++20", "-O2", "-Wall", "-Wextra", "-static", "-Isrc",
                    "tools/tests/control_locks_test.cpp", "-o", str(exe)], cwd=ROOT, check=True)
    subprocess.run([str(exe)], cwd=ROOT, check=True)
