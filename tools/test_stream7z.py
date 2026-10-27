"""A 7z downloaded over HTTP by the native downloader and extracted while it downloads: the hosts' wiring
(network::progress_of, network::fetch_range) between the download and rar-extract, against a paced server."""
from pathlib import Path
import http.server, os, random, re, shutil, subprocess, tempfile, threading, time
from rar_worker import build_host

ROOT = Path(__file__).resolve().parents[1]
RATE = 16 * 1024 * 1024  # bytes per second across the 8 connections


class Handler(http.server.BaseHTTPRequestHandler):
    archive = b""
    ranges = []
    lock = threading.Lock()

    def log_message(self, *args): pass

    def do_HEAD(self): self.serve(head=True)

    def do_GET(self): self.serve()

    def serve(self, head=False):
        data = Handler.archive
        start, end, status = 0, len(data) - 1, 200
        match = re.fullmatch(r"bytes=(\d+)-(\d+)", self.headers.get("Range") or "")
        if match:
            start, end = int(match[1]), min(int(match[2]), len(data) - 1)
            status = 206
        with Handler.lock: Handler.ranges.append((start, end))
        self.send_response(status)
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("ETag", '"fixed"')
        if status == 206: self.send_header("Content-Range", f"bytes {start}-{end}/{len(data)}")
        self.end_headers()
        if head: return
        # Paced like a slow host: each connection gets its share of RATE.
        at = start
        while at <= end:
            chunk = data[at:min(end + 1, at + 65536)]
            try: self.wfile.write(chunk)
            except OSError: return
            at += len(chunk)
            time.sleep(len(chunk) * 8 / RATE)


def main():
    seven = shutil.which("7zz")
    if not seven:
        print("SKIP: 7-Zip (7zz) is not installed; brew install sevenzip")
        return
    with tempfile.TemporaryDirectory(prefix="ps5-react-stream7z-") as temporary:
        root = Path(temporary)
        brew = lambda name: subprocess.check_output(["brew", "--prefix", name], text=True).strip()
        prefix, xz, crypto = brew("libarchive"), brew("xz"), brew("openssl@3")
        client = root / "stream-client"
        sources = ["tools/tests/stream_client.cpp", "native/shared/network.cpp", "tools/tests/network_test_platform.cpp",
                   "native/shared/archives.cpp", "native/shared/archive_preflight.cpp", "native/shared/rar5_password.cpp",
                   "native/shared/archive_writer.cpp", "native/desktop/rar_worker_process.cpp"]
        subprocess.run(["clang++", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(ROOT / "native/shared"),
                        "-I", prefix + "/include", "-I", xz + "/include", "-I", crypto + "/include", *(str(ROOT / s) for s in sources),
                        "-L", prefix + "/lib", "-L", xz + "/lib", "-L", crypto + "/lib", "-lcurl", "-larchive", "-llzma", "-lcrypto", "-lz",
                        "-o", str(client)], check=True)
        worker = build_host(root / "rar-extract")
        # A game-like file: 7-Zip's fastest level, as the large repacks use, in 1 MiB LZMA2 blocks.
        random.seed(9)
        game = root / "src/game.exfat"; game.parent.mkdir()
        game.write_bytes(b"".join(os.urandom(2048) + bytes([random.randrange(3)]) * 6144 for _ in range(16384)))
        archive = root / "game.7z"
        subprocess.run([seven, "a", "-bd", "-bso0", "-mx1", "-mmt=4", "-md=256k", str(archive), "game.exfat"], cwd=game.parent, check=True)
        Handler.archive = archive.read_bytes()
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        (root / "downloads").mkdir()
        result = subprocess.run([str(client), f"http://127.0.0.1:{server.server_port}/game.7z", str(root / "downloads/game.7z"),
                                 str(root / "out")], capture_output=True, text=True, timeout=180,
                                env={**os.environ, "PS5_REACT_RAR_WORKER": str(worker)})
        server.shutdown()
        lines = dict(line.split(" ", 1) for line in result.stdout.splitlines())
        assert result.returncode == 0 and lines.get("extracted", "").startswith("completed"), (result.stdout, result.stderr[-3000:])
        assert (root / "out/game.exfat").read_bytes() == game.read_bytes()
        assert "7z headers fetched" in result.stderr, result.stderr[-3000:]
        # The headers came from a ranged request for the archive's last bytes, ahead of the download reaching them.
        tail = [r for r in Handler.ranges if r[1] == len(Handler.archive) - 1 and r[1] - r[0] < 65536]
        assert tail, Handler.ranges
        downloaded, early = map(int, lines["downloaded"].split())
        finished = int(lines["extracted"].split()[1])
        # Decoded as it arrived: most of the game was written before the download ended. What is left is about
        # the ranges in flight at the end (8 of 1 MiB here, a quarter of the archive).
        assert early >= game.stat().st_size // 2, (early, game.stat().st_size)
        print(f"PASS: a {len(Handler.archive) >> 20} MiB 7z downloaded in {downloaded} ms, {early * 100 // game.stat().st_size}% "
              f"extracted by then and all of it {finished - downloaded} ms later")


if __name__ == "__main__":
    main()
