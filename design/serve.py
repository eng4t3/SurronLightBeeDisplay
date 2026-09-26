#!/usr/bin/env python3
"""Tiny dev server for design/mockups.html.

Serves the repository root (so ../tools/fonts resolves) and accepts two POSTs
from the mockup page:

  POST /save?name=<rel/path>   body = PNG bytes  -> design/png/<rel/path>.png
  POST /spec                   body = JSON {block: markdown}
                               -> replaces <!-- GEN:block --> ... <!-- /GEN:block -->
                                  sections inside design/DESIGN_SPEC.md

Usage:  python design/serve.py [port]      then open
        http://127.0.0.1:8765/design/mockups.html   (add ?export=1 to auto-export)
"""
import json
import os
import re
import sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
from functools import partial

DESIGN = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(DESIGN)
PNG_DIR = os.path.join(DESIGN, "png")
SPEC = os.path.join(DESIGN, "DESIGN_SPEC.md")


class Handler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def _reply(self, code, msg):
        body = msg.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        u = urlparse(self.path)
        data = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if u.path == "/save":
            name = parse_qs(u.query).get("name", [""])[0]
            if not re.fullmatch(r"[a-z0-9_\-]+(/[a-z0-9_\-]+)?", name):
                return self._reply(400, "bad name")
            out = os.path.join(PNG_DIR, name + ".png")
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, "wb") as f:
                f.write(data)
            return self._reply(200, "ok " + name)
        if u.path == "/spec":
            blocks = json.loads(data.decode("utf-8"))
            with open(SPEC, encoding="utf-8") as f:
                text = f.read()
            for key, md in blocks.items():
                pat = re.compile(r"(<!-- GEN:%s -->\n).*?(<!-- /GEN:%s -->)" % (key, key), re.S)
                text, n = pat.subn(lambda m: m.group(1) + md.rstrip() + "\n" + m.group(2), text)
                if n == 0:
                    return self._reply(400, "marker not found: " + key)
            with open(SPEC, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            return self._reply(200, "spec ok")
        self._reply(404, "not found")

    def log_message(self, fmt, *args):
        if "POST" in (args[0] if args else ""):
            super().log_message(fmt, *args)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    srv = ThreadingHTTPServer(("127.0.0.1", port), partial(Handler, directory=ROOT))
    print(f"serving {ROOT} on http://127.0.0.1:{port}/design/mockups.html")
    srv.serve_forever()
