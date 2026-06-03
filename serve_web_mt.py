#!/usr/bin/env python3
# serve_web_mt.py — static server for the MULTITHREADED (pthreads) web build that
# sends the cross-origin-isolation headers SharedArrayBuffer requires.
#
# Plain `python3 -m http.server` does NOT send COOP/COEP, so the page is NOT
# cross-origin isolated and `SharedArrayBuffer` (and therefore Emscripten pthreads /
# Web Workers) is unavailable — the wasm refuses to start. This server adds:
#     Cross-Origin-Opener-Policy:   same-origin
#     Cross-Origin-Embedder-Policy: require-corp
#     Cross-Origin-Resource-Policy: same-origin
#
# SECURE CONTEXT: SharedArrayBuffer also needs a secure context. `localhost` counts,
# so this is fine for local testing. To serve over your LAN, put it behind HTTPS
# (you have SSL) — e.g. a reverse proxy (caddy/nginx) terminating TLS in front of
# this, or pass a cert/key below. The File System Access API picker likewise needs a
# secure context for showDirectoryPicker(); over plain-HTTP LAN the harness falls
# back to <input webkitdirectory> (web_fs.js pickViaInput), but SharedArrayBuffer
# still requires HTTPS — so for the MT build, serve over HTTPS on the LAN.
#
# Usage:
#   python3 serve_web_mt.py [PORT] [DIR]
#       PORT defaults to 8443; DIR defaults to ./build_web_mt/web (or "." if run
#       from inside that dir).
#   HTTPS (recommended for LAN): set KB_CERT and KB_KEY to a PEM cert/key pair:
#       KB_CERT=cert.pem KB_KEY=key.pem python3 serve_web_mt.py 8443
import http.server, os, ssl, sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8443
if len(sys.argv) > 2:
    DIR = sys.argv[2]
elif os.path.isdir("build_web_mt/web"):
    DIR = "build_web_mt/web"
else:
    DIR = "."

class COOPHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        # WASM streaming + no stale caching during iteration.
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def guess_type(self, path):
        if path.endswith(".wasm"):
            return "application/wasm"
        return super().guess_type(path)

def main():
    os.chdir(DIR)
    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), COOPHandler)
    cert, key = os.environ.get("KB_CERT"), os.environ.get("KB_KEY")
    scheme = "http"
    if cert and key:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(certfile=cert, keyfile=key)
        httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
        scheme = "https"
    print(f"serving {os.getcwd()} at {scheme}://localhost:{PORT}/  "
          f"(COOP/COEP cross-origin isolated{' + TLS' if scheme=='https' else ''})")
    print("  open index.html, pick your Steam 'Call of Duty Black Ops' folder.")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")

if __name__ == "__main__":
    main()
