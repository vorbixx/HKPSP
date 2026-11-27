"""Small client for the local AssetRipper 2 API; exports stay in _private.

Start AssetRipper with --headless --port 58291, then load a private input
folder containing Managed assemblies. Collection indices are session-specific.
"""
import argparse
import html
import http.client
import json
import re
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class Client:
    def __init__(self, port=58291):
        self.port = port
        self.base = f"http://127.0.0.1:{port}"
        self._local = threading.local()

    def _connection(self):
        connection = getattr(self._local, "connection", None)
        if connection is None:
            connection = self._local.connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=120)
        return connection

    def request(self, endpoint, **query):
        # One kept-alive connection per thread: a new connection per request left thousands of
        # sockets in TIME_WAIT and ran Windows out of ports (WSAENOBUFS 10055) on fast PCs.
        path = endpoint + "?" + urllib.parse.urlencode(query)
        for attempt in range(5):
            connection = self._connection()
            try:
                connection.request("GET", path, headers={"Connection": "keep-alive"})
                response = connection.getresponse()
                body = response.read()
                if response.will_close:
                    connection.close()
                    self._local.connection = None
            except (OSError, http.client.HTTPException):
                connection.close()
                self._local.connection = None
                if attempt == 4:
                    raise
                time.sleep(0.5 * 2 ** attempt)
                continue
            if response.status >= 400:
                raise urllib.error.HTTPError(self.base + path, response.status, response.reason,
                                             response.headers, None)
            return body

    def search(self, query):
        page = self.request("/Search/View", q=query).decode()
        result = []
        for row in re.findall(r'<tr data-class="[\s\S]*?</tr>', page):
            cells = re.findall(r"<td>([\s\S]*?)</td>", row)
            url = html.unescape(re.search(r'href="([^"]+)"', cells[2])[1])
            path = json.loads(urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)["Path"][0])
            result.append(dict(path=path, type=re.sub("<[^>]+>", "", cells[1]),
                               name=html.unescape(re.sub("<[^>]+>", "", cells[2])),
                               collection=html.unescape(re.sub("<[^>]+>", "", cells[3]))))
        return result

    def asset(self, path, format="Json"):
        return self.request("/Assets/" + format, Path=json.dumps(path, separators=(",", ":")))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=58291)
    commands = parser.add_subparsers(dest="command", required=True)
    search = commands.add_parser("search")
    search.add_argument("query")
    fetch = commands.add_parser("fetch")
    fetch.add_argument("collection", type=int)
    fetch.add_argument("ids", type=int, nargs="+")
    args = parser.parse_args()
    client = Client(args.port)
    if args.command == "search":
        print(json.dumps(client.search(args.query), indent=2, ensure_ascii=False))
    else:
        output = ROOT / "_private/research/assets"
        output.mkdir(parents=True, exist_ok=True)
        for asset_id in args.ids:
            path = {"C": {"B": {"P": []}, "I": args.collection}, "D": asset_id}
            target = output / f"c{args.collection}-{asset_id}.json"
            target.write_bytes(client.asset(path))
            print(target)


if __name__ == "__main__":
    main()
