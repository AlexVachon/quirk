# tools/flaky_server.py <dir> <port> — serve <dir> over HTTP, but answer 504 to
# the first request for any `*-csv-*` file (a transient failure that a retry
# fixes) and to every request for `*-crypto-*` (a persistent one).
# Used by tools/rehearse_release.sh to check download retries and the
# per-package fallback to the stdlib bundled in the compiler tarball.
import http.server, os, sys

root, port = sys.argv[1], int(sys.argv[2])
seen = {}

class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=root, **kw)

    def _flaky(self):
        name = os.path.basename(self.path)
        seen[name] = seen.get(name, 0) + 1
        if "-crypto-" in name or ("-csv-" in name and seen[name] == 1):
            self.send_response(504)
            self.end_headers()
            return True
        return False

    def do_GET(self):
        if not self._flaky():
            super().do_GET()

    def do_HEAD(self):
        super().do_HEAD()

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.command, self.path))

http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
