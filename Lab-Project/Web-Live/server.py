#!/usr/bin/env python3
"""AVIMS live bridge — connects the REAL FreeRTOS engine to the browser.

- Spawns ./avims_live (the FreeRTOS POSIX binary) as a child process.
- Streams the engine's stdout (JSON event lines) to browsers via Server-Sent Events
  (GET /events).
- Forwards browser commands (POST /cmd, body = "ADD 4" etc.) to the engine's stdin.
- Serves the static web client (index.html, app-live.js, style.css, conflict_data.js).

Pure Python standard library — no pip installs. Run:  python3 server.py  [port]
Then open http://localhost:8100/
"""
import http.server, socketserver, subprocess, threading, queue, os, sys, json, time

BASE = os.path.dirname(os.path.abspath(__file__))
SHARED = os.path.normpath(os.path.join(BASE, "..", "web"))   # for conflict_data.js
BIN = os.path.join(BASE, "avims_live")
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8100

subscribers = set()          # set of queue.Queue, one per connected browser
subs_lock = threading.Lock()
proc = None

def broadcast(line):
    with subs_lock:
        for q in list(subscribers):
            q.put(line)

def reader_thread():
    """Read the engine's stdout line-by-line and fan out to all browsers."""
    for raw in proc.stdout:
        line = raw.rstrip("\n")
        if line:
            broadcast(line)
    broadcast(json.dumps({"ev": "engine_exit"}))

def ensure_binary():
    if os.path.exists(BIN):
        return True
    print("avims_live not built — running make…")
    r = subprocess.run(["make"], cwd=BASE)
    if r.returncode != 0 or not os.path.exists(BIN):
        print("\nBUILD FAILED. Make sure the FreeRTOS kernel is present:")
        print("  git clone --depth 1 https://github.com/FreeRTOS/FreeRTOS-Kernel.git "
              "(in the project root)")
        return False
    return True

CTYPES = {".html": "text/html", ".js": "application/javascript",
          ".css": "text/css", ".json": "application/json"}

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass  # keep the console clean

    # ---- browser -> engine ------------------------------------------------
    def do_POST(self):
        if self.path == "/cmd":
            n = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(n).decode("utf-8", "replace").strip()
            if body and proc and proc.poll() is None:
                try:
                    proc.stdin.write(body + "\n")
                    proc.stdin.flush()
                except (BrokenPipeError, ValueError):
                    pass
            self.send_response(204); self.end_headers()
        else:
            self.send_response(404); self.end_headers()

    # ---- engine -> browser (SSE) and static files -------------------------
    def do_GET(self):
        if self.path == "/events":
            return self._sse()
        if self.path == "/health":
            alive = proc is not None and proc.poll() is None
            body = json.dumps({"engine": "avims_live", "alive": alive,
                               "pid": proc.pid if proc else None}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers(); self.wfile.write(body)
            return
        return self._static()

    def _sse(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        q = queue.Queue()
        with subs_lock:
            subscribers.add(q)
        try:
            self.wfile.write(b": connected\n\n"); self.wfile.flush()
            while True:
                try:
                    line = q.get(timeout=15)
                    self.wfile.write(("data: " + line + "\n\n").encode()); self.wfile.flush()
                except queue.Empty:
                    self.wfile.write(b": ping\n\n"); self.wfile.flush()  # heartbeat
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            with subs_lock:
                subscribers.discard(q)

    def _static(self):
        path = self.path.split("?", 1)[0]
        if path in ("/", "/index.html"):
            path = "/index.html"
        name = os.path.normpath(path).lstrip("/\\")
        if ".." in name:
            self.send_response(403); self.end_headers(); return
        full = os.path.join(BASE, name)
        if not os.path.exists(full):                 # shared assets (e.g. conflict_data.js)
            full = os.path.join(SHARED, name)
        if not os.path.exists(full):
            self.send_response(404); self.end_headers(); return
        ext = os.path.splitext(full)[1]
        with open(full, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", CTYPES.get(ext, "application/octet-stream"))
        self.send_header("Content-Length", str(len(data)))
        self.end_headers(); self.wfile.write(data)

class ThreadingHTTP(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True

def main():
    global proc
    if not ensure_binary():
        sys.exit(1)
    proc = subprocess.Popen([BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            text=True, bufsize=1)
    threading.Thread(target=reader_thread, daemon=True).start()
    srv = ThreadingHTTP(("", PORT), Handler)
    print(f"AVIMS live: real FreeRTOS engine PID {proc.pid}")
    print(f"open  http://localhost:{PORT}/   (Ctrl-C to stop)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstopping…")
    finally:
        if proc and proc.poll() is None:
            proc.terminate()
            try: proc.wait(timeout=2)
            except subprocess.TimeoutExpired: proc.kill()

if __name__ == "__main__":
    main()
