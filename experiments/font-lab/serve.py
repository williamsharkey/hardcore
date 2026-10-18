#!/usr/bin/env python3
"""Rebuild and serve the font lab locally: python3 experiments/font-lab/serve.py."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from build import build_data, bundle

HERE = Path(__file__).resolve().parent
STYLE = '''
:root { color-scheme:dark; --background:#14171b; --foreground:#e1e5e8;
  --muted-foreground:#a1acb7; --border:#343c43; --blue:#8caed4; --orange:#d1aa7e; --green:#89b79c; }
* { box-sizing:border-box; }
body { margin:0; padding:24px; background:var(--background); color:var(--foreground);
  font:14px/1.55 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif; }
main { max-width:1180px; margin:auto; }
h2 { font-size:26px; font-weight:600; letter-spacing:-.5px; } h3 { font-size:17px; font-weight:600; }
.text-small { font-size:12px; } .text-muted { color:var(--muted-foreground); }
.tabular-nums { font-variant-numeric:tabular-nums; }
.form-label { display:block; } .form-select { display:block; width:100%; margin-top:4px; }
input,select,textarea,button { font:inherit; color:inherit; }
input[type=number],select,textarea { background:#1d2228; border:1px solid #46515c; border-radius:5px; padding:6px 8px; }
input[type=checkbox],input[type=range] { accent-color:#8caed4; }
.form-range { width:100%; } .form-check { display:flex; align-items:center; gap:8px; }
.btn { cursor:pointer; border:1px solid #46515c; background:#222a32; border-radius:5px; padding:6px 11px; }
.btn:hover { background:#303c49; } .btn-ghost { background:transparent; }
button:disabled,select:disabled { opacity:.45; cursor:default; }
:focus-visible { outline:2px solid #a8c7e7; outline-offset:3px; }
hr { border:0; border-top:1px solid var(--border); margin:22px 0; }
a { color:#a8c7e7; } summary { cursor:pointer; } .text-destructive { color:#f0a49b; }
@media(max-width:600px) { body { padding:16px; } }
'''

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path not in ('/', '/index.html'):
            self.send_error(404)
            return
        fragment = (HERE / 'hairline-font-lab.html').read_text()
        document = '<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Hairline Font Lab</title><style>' + STYLE + '</style></head><body><main>' + fragment + '</main></body></html>'
        content = document.encode('utf-8')
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(content)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(content)

    def log_message(self, *args):
        pass

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=50093)
    args = parser.parse_args()
    bundle(build_data(), HERE / 'hairline-font-lab.html')
    with ThreadingHTTPServer(('127.0.0.1', args.port), Handler) as server:
        print(f'http://127.0.0.1:{args.port}/', flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
