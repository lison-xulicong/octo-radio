"""Static file server for the octopus asset export.

Serves the repository root so tools/octopus/export.html can load the scene
renderer, and accepts POST /tools/octopus/out/<name>.json so the page can save
what it rendered. Run through .claude/launch.json ("octopus-export").
"""

import http.server
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "tools", "octopus", "out")


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    def do_POST(self):
        name = os.path.basename(self.path.split("?")[0])
        if not name.endswith(".json"):
            self.send_error(400, "only .json")
            return
        length = int(self.headers.get("Content-Length", 0))
        os.makedirs(OUT, exist_ok=True)
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(self.rfile.read(length))
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"ok")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8766
    http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
