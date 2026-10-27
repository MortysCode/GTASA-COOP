"""Build and run wallet tests against an isolated local server (after tools/build.py)."""
from pathlib import Path
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build/release/bin"
TC = ROOT / "toolchain/llvm-mingw/bin"


def main(suite="money"):
    exe = BIN / f"{suite}-test.exe"
    cmd = [str(TC / "i686-w64-mingw32-clang++.exe"), "-std=c++20", "-O2", "-Wall", "-Wextra", "-static",
           "-DWIN32_LEAN_AND_MEAN", "-DNOMINMAX", "-Isrc", "-Ithird_party/enet/include",
           f"tools/tests/{suite}_test.cpp", *map(str, (ROOT / "build/release/obj/enet").glob("*.o")),
           "-lws2_32", "-lwinmm", "-o", str(exe)]
    subprocess.run(cmd, cwd=ROOT, check=True)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix=f"sa-coop-{suite}-test-") as td:
        config = Path(td) / "server.ini"
        config.write_text("[server]\ninternet=0\nhost=Host\n", encoding="utf-8")
        with (Path(td) / "console.log").open("w", encoding="utf-8") as output:
            server = subprocess.Popen([str(BIN / "coop-server.exe"), "--config", str(config),
                                       "--data", td, "--port", str(port)], cwd=ROOT,
                                      stdin=subprocess.DEVNULL, stdout=output, stderr=output,
                                      creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                deadline = time.monotonic() + 5
                while not (Path(td) / "server.log").exists() and time.monotonic() < deadline:
                    if server.poll() is not None:
                        raise RuntimeError("test server failed to start")
                    time.sleep(0.05)
                subprocess.run([str(exe), str(port)], cwd=ROOT, check=True)
            except BaseException:
                for log in Path(td).glob("*.log"):
                    print(log.read_text(encoding="utf-8-sig", errors="replace").encode("ascii", "backslashreplace").decode())
                raise
            finally:
                server.terminate()
                server.wait(timeout=5)


if __name__ == "__main__":
    main()
