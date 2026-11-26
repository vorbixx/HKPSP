"""Small client for the local AssetRipper 2 API; exports stay in _private.

Start AssetRipper with --headless --port 58291, then load a private input
folder containing Managed assemblies. Collection indices are session-specific.
"""
import argparse
import html
import json
import re
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class Client:
    def __init__(self, port=58291):
        self.base = f"http://127.0.0.1:{port}"

    def request(self, endpoint, **query):
        url = self.base + endpoint + "?" + urllib.parse.urlencode(query)
        with urllib.request.urlopen(url, timeout=120) as response:
            return response.read()

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
