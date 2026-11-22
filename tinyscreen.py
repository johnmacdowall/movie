#!/usr/bin/env python3
"""tinyscreen - stream movies to an ESP32 + ST7789 (320x240) display.

ffmpeg turns any movie into small JPEG frames plus 8-bit mono audio. They are
sent to the ESP32 over plain HTTP as records: <1-byte type 'V' or 'A'>
<4-byte little-endian length><data>. 'V' = one JPEG frame, 'A' = unsigned
8-bit PCM samples at AUDIO_RATE (header X-Audio-Rate).

  http://emby.lan:8090/        web page: pick a movie, start minute, stop
  http://emby.lan:8090/stream  what the ESP32 connects to (204 = nothing playing)

Settings come from environment variables (see tinyscreen.service).
Standard library only - no pip installs needed.
"""
import collections
import os
import queue
import random
import struct
import subprocess
import threading
import time
import unicodedata
import urllib.parse
from html import escape
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MOVIE_DIR = os.environ.get("MOVIE_DIR", "/mnt/movies")
PORT = int(os.environ.get("PORT", "8090"))
FPS = float(os.environ.get("FPS", "15"))
QUALITY = os.environ.get("QUALITY", "7")
AUDIO_RATE = int(os.environ.get("AUDIO_RATE", "16000"))  # 0 = no audio
# Video is held back this long so audio reaches the ESP32 early and it can keep
# a cushion of sound against WiFi hiccups; the ESP32 delays audio to match.
AUDIO_LEAD = float(os.environ.get("AUDIO_LEAD", "0.4"))  # mjpeg -q:v, 2 = best/largest, 31 = worst/smallest
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
# Extra library folders for ffmpeg only (Emby's bundled ffmpeg needs its own libs)
FFMPEG_LIBS = os.environ.get("FFMPEG_LIBS", "")
FFMPEG_ENV = dict(os.environ)
if FFMPEG_LIBS:
    FFMPEG_ENV["LD_LIBRARY_PATH"] = FFMPEG_LIBS + (
        ":" + os.environ["LD_LIBRARY_PATH"] if os.environ.get("LD_LIBRARY_PATH") else "")
# ffprobe sits next to ffmpeg (Emby ships both in /opt/emby-server/bin)
FFPROBE = os.environ.get("FFPROBE", os.path.join(os.path.dirname(FFMPEG), "ffprobe")
                         if os.path.dirname(FFMPEG) else "ffprobe")
EXTS = {".mkv", ".mp4", ".m4v", ".avi", ".mov", ".ts", ".m2ts", ".wmv", ".mpg", ".webm"}

lock = threading.Lock()
# file: movie path relative to MOVIE_DIR (None = stopped)
# pos:  seconds into the movie (kept up to date so a reconnect resumes)
# gen:  bumped on every play/stop/new stream so older streams know to quit
state = {"file": None, "pos": 0.0, "gen": 0, "error": ""}


def list_movies():
    out = []
    for root, dirs, files in os.walk(MOVIE_DIR):
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        for f in files:
            if os.path.splitext(f)[1].lower() in EXTS:
                out.append(os.path.relpath(os.path.join(root, f), MOVIE_DIR))
    return sorted(out, key=str.lower)


def safe_path(rel):
    base = os.path.realpath(MOVIE_DIR)
    p = os.path.realpath(os.path.join(base, rel or ""))
    if not p.startswith(base + os.sep) or not os.path.isfile(p):
        return None
    return p


def has_filters(*names):
    try:
        out = subprocess.run([FFMPEG, "-hide_banner", "-filters"], env=FFMPEG_ENV,
                             capture_output=True, text=True, timeout=20).stdout
    except Exception:
        return False
    return all(f" {n} " in out for n in names)


CAN_TONEMAP = None  # checked on first use
NO_BURST = None     # ffmpeg 6.1+ sends the first 0.5 s at once unless told not to


def burst_opts():
    global NO_BURST
    if NO_BURST is None:
        try:
            out = subprocess.run([FFMPEG, "-hide_banner", "-h", "full"], env=FFMPEG_ENV,
                                 capture_output=True, text=True, timeout=20).stdout
            NO_BURST = "readrate_initial_burst" in out
        except Exception:
            NO_BURST = False
    return ["-readrate_initial_burst", "0"] if NO_BURST else []


def transfer_of(path):
    """Video transfer curve, e.g. 'smpte2084' (HDR10), 'arib-std-b67' (HLG), 'bt709'."""
    try:
        out = subprocess.run([FFPROBE, "-v", "error", "-select_streams", "v:0",
                              "-show_entries", "stream=color_transfer", "-of", "csv=p=0", path],
                             env=FFMPEG_ENV, capture_output=True, text=True, timeout=30).stdout
        return out.strip().splitlines()[0].strip() if out.strip() else ""
    except Exception as e:
        print(f"ffprobe failed: {e}", flush=True)
        return ""


def has_audio(path):
    try:
        out = subprocess.run([FFPROBE, "-v", "error", "-select_streams", "a",
                              "-show_entries", "stream=index", "-of", "csv=p=0", path],
                             env=FFMPEG_ENV, capture_output=True, text=True, timeout=30).stdout
        return bool(out.strip())
    except Exception:
        return False


# Audio for a 20x30 mm speaker: drop bass it can't play, even out loud and quiet
# parts (compressor, no look-ahead so no delay), 8-bit mono.
AUDIO_FILTER = ("highpass=f=200,"
                "acompressor=threshold=0.06:ratio=6:attack=5:release=120:makeup=4,"
                "aresample=async=1")


CROP_CACHE = {}  # path -> "w:h:x:y" or ""


def detect_crop(path):
    """Find black bars baked into the video (e.g. a 4:3 film inside a 16:9 frame)
    by sampling 1 s at 15%, 40% and 65% of the movie; keep the largest picture
    area seen so a dark scene can't over-crop. Cached per file."""
    if path in CROP_CACHE:
        return CROP_CACHE[path]
    crop = ""
    try:
        info = subprocess.run([FFPROBE, "-v", "error", "-select_streams", "v:0",
                               "-show_entries", "stream=width,height:format=duration",
                               "-of", "default=nw=1", path],
                              env=FFMPEG_ENV, capture_output=True, text=True, timeout=30).stdout
        vals = dict(l.split("=", 1) for l in info.splitlines() if "=" in l)
        W, H = int(vals["width"]), int(vals["height"])
        dur = float(vals.get("duration") or 0) or 600.0
        x0, y0, x1, y1 = W, H, 0, 0
        for frac in (0.15, 0.40, 0.65):
            out = subprocess.run([FFMPEG, "-hide_banner", "-nostdin", "-ss", f"{dur * frac:.1f}",
                                  "-i", path, "-map", "0:v:0", "-t", "1", "-an", "-sn",
                                  "-vf", "cropdetect=limit=24:round=2:reset=0",
                                  "-f", "null", "-"],
                                 env=FFMPEG_ENV, capture_output=True, text=True, timeout=60).stderr
            found = [l.rsplit("crop=", 1)[1].split()[0] for l in out.splitlines() if "crop=" in l]
            if not found:
                continue
            w, h, x, y = (int(v) for v in found[-1].split(":"))
            if w <= 0 or h <= 0:
                continue
            x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x + w), max(y1, y + h)
        if x1 > x0 and y1 > y0:
            w, h = (x1 - x0) // 2 * 2, (y1 - y0) // 2 * 2
            # only bother if it removes at least 4% in one direction
            if w < W * 0.96 or h < H * 0.96:
                crop = f"{w}:{h}:{x0}:{y0}"
    except Exception as e:
        print(f"cropdetect failed: {e}", flush=True)
    CROP_CACHE[path] = crop
    print(f"black-bar crop for {os.path.basename(path)}: {crop or 'none'}", flush=True)
    return crop


def ffmpeg_cmd(path, start, audio_fd=None):
    global CAN_TONEMAP
    # Cut off baked-in black bars, fix non-square pixels (DVD rips), then fit
    # inside 320x240 keeping aspect, so the picture fills at least one side.
    # Dimensions are kept to multiples of 16 (whole JPEG blocks) so the ESP32
    # decoder never draws partial blocks.
    crop = detect_crop(path)
    vf = (f"fps={FPS:g},"
          + (f"crop={crop}," if crop else "")
          + "scale='trunc(iw*sar/2)*2':ih,setsar=1,"
          "scale=320:240:force_original_aspect_ratio=decrease:"
          "force_divisible_by=16:flags=fast_bilinear")
    trc = transfer_of(path)
    if trc in ("smpte2084", "arib-std-b67"):
        if CAN_TONEMAP is None:
            CAN_TONEMAP = has_filters("zscale", "tonemap")
        if CAN_TONEMAP:
            # HDR -> SDR after shrinking (cheap at 320x240), otherwise it looks washed out
            vf += (f",zscale=tin={trc}:pin=bt2020:min=bt2020nc:t=linear:npl=100,"
                   "format=gbrpf32le,zscale=p=bt709,tonemap=hable:desat=0,"
                   "zscale=t=bt709:m=bt709:r=tv,format=yuv420p")
            print(f"HDR ({trc}): tone-mapping to SDR", flush=True)
        else:
            print(f"HDR ({trc}) but ffmpeg lacks zscale/tonemap: colors will look washed out",
                  flush=True)
    cmd = [FFMPEG, "-hide_banner", "-loglevel", "error", "-nostdin",
           "-re", *burst_opts(), "-ss", f"{start:.2f}", "-i", path,
           "-map", "0:v:0", "-an", "-sn", "-dn",
           "-vf", vf, "-pix_fmt", "yuvj420p",
           "-c:v", "mjpeg", "-q:v", str(QUALITY),
           "-f", "image2pipe", "pipe:1"]
    if audio_fd is not None:
        cmd += ["-map", "0:a:0", "-vn", "-sn", "-dn",
                "-af", AUDIO_FILTER, "-ac", "1", "-ar", str(AUDIO_RATE),
                "-c:a", "pcm_u8", "-f", "u8", f"pipe:{audio_fd}"]
    return cmd


def jpeg_frames(stream):
    """Split ffmpeg's concatenated JPEG output into single frames."""
    buf = b""
    while True:
        chunk = stream.read1(65536)
        if not chunk:
            return
        buf += chunk
        while True:
            s = buf.find(b"\xff\xd8")
            if s < 0:
                buf = buf[-1:]
                break
            e = buf.find(b"\xff\xd9", s + 2)
            if e < 0:
                buf = buf[s:]
                break
            yield buf[s:e + 2]
            buf = buf[e + 2:]


PAGE = """<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>tinyscreen</title><style>
body{{font-family:system-ui,sans-serif;max-width:900px;margin:1em auto;padding:0 1em}}
.bar{{position:sticky;top:0;background:#fff;padding:.5em 0;border-bottom:1px solid #ccc}}
input{{font-size:1em;padding:.3em}} #q{{width:50%}}
ul{{list-style:none;padding:0}} li{{padding:.25em 0;border-bottom:1px solid #eee}}
a{{text-decoration:none}} .now{{background:#eef;padding:.5em}}
</style></head><body>
<div class="bar">
<div class="now">Now playing: <b>{now}</b> &nbsp; <a href="/stop">[stop]</a>
<a href="/skip?s=300">[+5 min]</a> <a href="/random">[random]</a></div>
<div style="color:#b00">{err}</div>
<p><input id="q" placeholder="filter..." oninput="filt()">
start at minute <input id="t" type="number" value="0" min="0" style="width:5em"></p>
</div>
<ul id="list">{items}</ul>
<script>
function play(f){{location='/play?f='+encodeURIComponent(f)+'&t='+(document.getElementById('t').value||0)}}
function filt(){{var q=document.getElementById('q').value.toLowerCase();
for(const li of document.querySelectorAll('#list li'))li.style.display=li.textContent.toLowerCase().includes(q)?'':'none'}}
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path == "/":
            return self.page()
        if u.path == "/play":
            rel = q.get("f", [""])[0]
            try:
                start = max(0.0, float(q.get("t", ["0"])[0]) * 60)
            except ValueError:
                start = 0.0
            if safe_path(rel):
                with lock:
                    state.update(file=rel, pos=start, gen=state["gen"] + 1, error="")
            return self.redirect()
        if u.path == "/stop":
            with lock:
                state.update(file=None, pos=0.0, gen=state["gen"] + 1, error="")
            return self.redirect()
        if u.path == "/skip":  # ESP32 button short press; ?s=seconds (negative = back)
            try:
                secs = float(q.get("s", ["300"])[0])
            except ValueError:
                secs = 300.0
            with lock:
                if state["file"]:
                    # bumping gen ends the current stream; the ESP32 reconnects at pos
                    state.update(pos=max(0.0, state["pos"] + secs), gen=state["gen"] + 1)
                    print(f"skip {secs:+.0f}s -> {state['pos']:.0f}s", flush=True)
            return self.redirect()
        if u.path == "/random":  # ESP32 button long press
            movies = list_movies()
            with lock:
                cur = state["file"]
            choices = [m for m in movies if m != cur] or movies
            if choices:
                pick = random.choice(choices)
                with lock:
                    state.update(file=pick, pos=0.0, gen=state["gen"] + 1, error="")
                print(f"random pick: {pick}", flush=True)
            return self.redirect()
        if u.path == "/stream":
            return self.stream()
        self.send_error(404)

    def redirect(self):
        self.send_response(303)
        self.send_header("Location", "/")
        self.end_headers()

    def page(self):
        with lock:
            now, err = state["file"], state["error"]
        items = "".join(
            f'<li><a href="#" onclick="play({escape(repr(m))});return false">{escape(m)}</a></li>'
            for m in list_movies())
        body = PAGE.format(now=escape(now) if now else "nothing", err=escape(err), items=items).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    @staticmethod
    def title_of(rel):
        """Display title: the movie's folder name (e.g. 'Alien (1979)'), or the
        file name if it sits loose. Plain ASCII so the ESP32 font can show it."""
        parts = rel.replace("\\", "/").split("/")
        name = parts[-2] if len(parts) > 1 else os.path.splitext(parts[-1])[0]
        name = unicodedata.normalize("NFKD", name).encode("ascii", "ignore").decode()
        return " ".join(name.replace(".", " ").replace("_", " ").split())[:90]

    def stream(self):
        with lock:
            rel, start = state["file"], state["pos"]
            if rel:
                state["gen"] += 1  # a new connection replaces any older stream
            gen = state["gen"]
        path = safe_path(rel) if rel else None
        if not path:
            self.send_response(204)
            self.end_headers()
            return

        audio = AUDIO_RATE > 0 and has_audio(path)
        print(f"stream start: {rel} at {start:.0f}s for {self.client_address[0]}"
              f"{'' if audio else ' (no audio)'}", flush=True)
        a_read = a_write = None
        if audio:
            a_read, a_write = os.pipe()
        # ffmpeg errors go to stderr, which systemd puts in the journal
        proc = subprocess.Popen(ffmpeg_cmd(path, start, a_write), stdout=subprocess.PIPE,
                                env=FFMPEG_ENV, pass_fds=(a_write,) if audio else ())
        if audio:
            os.close(a_write)

        # Video and audio come from two pipes; two reader threads feed one queue
        # so the socket gets them interleaved in the order ffmpeg produced them.
        q = queue.Queue(maxsize=64)
        stop = threading.Event()

        def put(item):
            while not stop.is_set():
                try:
                    q.put(item, timeout=0.5)
                    return True
                except queue.Full:
                    pass
            return False

        def read_video():
            for jpg in jpeg_frames(proc.stdout):
                if not put((b"V", jpg)):
                    return
            put(None)  # video finished = stream finished

        def read_audio():
            with os.fdopen(a_read, "rb", buffering=0) as f:
                while not stop.is_set():
                    data = f.read(2048)
                    if not data:
                        put((b"AEND", b""))
                        return
                    if not put((b"A", data)):
                        return

        threading.Thread(target=read_video, daemon=True).start()
        if audio:
            threading.Thread(target=read_audio, daemon=True).start()

        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("X-FPS", f"{FPS:g}")
        self.send_header("X-Proto", "2")
        self.send_header("X-Title", self.title_of(rel))
        self.send_header("X-Audio-Rate", str(AUDIO_RATE if audio else 0))
        lead = AUDIO_LEAD if audio else 0.0
        self.send_header("X-Audio-Lead", str(int(lead * 1000)))
        self.send_header("Connection", "close")
        self.end_headers()
        n = 0
        finished = False
        t0 = time.monotonic()
        # Video frame n is held until audio up to n/FPS + lead has been sent, so
        # the ESP32 always has `lead` seconds of sound in hand (measured in movie
        # time, not wall time, so ffmpeg's audio bursts don't matter). Safety:
        # never hold a frame longer than lead + 1 s, or once audio has ended.
        held = collections.deque()  # (held_since, jpeg)
        done = False
        audio_bytes = 0
        audio_done = not audio
        vsent = 0

        def send(kind, data):
            nonlocal n, audio_bytes
            self.wfile.write(struct.pack("<cI", kind, len(data)) + data)
            if kind == b"A":
                audio_bytes += len(data)
            else:
                n += 1
                with lock:
                    if state["gen"] == gen:
                        state["pos"] = start + n / FPS

        def releasable(now):
            if not held:
                return False
            if done or audio_done or now - held[0][0] > lead + 1.0:
                return True
            return audio_bytes / AUDIO_RATE >= (n + 1) / FPS + lead

        try:
            while True:
                now = time.monotonic()
                while releasable(now):
                    send(b"V", held.popleft()[1])
                if done and not held:
                    finished = proc.wait() == 0
                    break
                try:
                    item = q.get(timeout=0.02)
                except queue.Empty:
                    continue
                with lock:
                    if state["gen"] != gen:
                        break
                if item is None:
                    done = True
                elif item[0] == b"A":
                    send(b"A", item[1])
                elif item[0] == b"AEND":
                    audio_done = True
                else:
                    held.append((time.monotonic(), item[1]))
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            pass
        finally:
            stop.set()
            proc.kill()
            rc = proc.wait()
        secs = time.monotonic() - t0
        rate = f", {n / secs:.1f} fps average" if secs > 1 else ""
        print(f"stream end: {n} frames sent in {secs:.0f}s{rate}, ffmpeg exit {rc}"
              f"{' (movie finished)' if finished else ''}", flush=True)
        if n == 0 and rc not in (0, -9):
            with lock:
                if state["gen"] == gen:  # ffmpeg failed outright: stop, don't retry forever
                    state.update(file=None, pos=0.0, gen=gen + 1,
                                 error=f"ffmpeg could not play {rel} (exit {rc}) - see journalctl -u tinyscreen")
        if finished:
            with lock:
                if state["gen"] == gen:  # movie ended: go back to idle
                    state.update(file=None, pos=0.0, gen=gen + 1)


if __name__ == "__main__":
    print(f"tinyscreen: serving {MOVIE_DIR} on port {PORT} ({FPS:g} fps, q={QUALITY}, "
          f"ffmpeg={FFMPEG}, ffprobe={FFPROBE})", flush=True)
    ThreadingHTTPServer(("", PORT), Handler).serve_forever()
