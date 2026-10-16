#!/usr/bin/env python3
"""Capture bounded, real Wikipedia HTML for offline adapter tests.

Uses only the Python standard library. Network access is needed only when
capturing; the application and fixture tests have no dependency on this tool.
The hand-authored tests/fixtures/wikipedia files are deliberately kept separate.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import sys
from urllib.error import URLError
from urllib.parse import quote, unquote, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener


MAX_BYTES = 8 * 1024 * 1024
ARTICLE_URLS = [
    "https://en.wikipedia.org/wiki/Ada_Lovelace",
    "https://en.wikipedia.org/wiki/Tokyo",
    "https://en.wikipedia.org/wiki/Fourier_transform",
    "https://en.wikipedia.org/wiki/List_of_countries_and_dependencies_by_population",
    "https://en.wikipedia.org/wiki/Printing_press",
]


def wikipedia_url(value):
    if "://" not in value:
        value = "https://en.wikipedia.org/wiki/" + quote(value.replace(" ", "_"), safe="_()")
    parsed = urlsplit(value)
    host = parsed.hostname or ""
    if (parsed.scheme != "https" or not host.endswith(".wikipedia.org")
            or not re.fullmatch(r"[a-z0-9-]+(?:\.m)?\.wikipedia\.org", host)
            or parsed.username or parsed.password or parsed.port not in (None, 443)
            or not parsed.path.startswith("/wiki/") or parsed.query or parsed.fragment):
        raise ValueError("expected an HTTPS Wikipedia /wiki/ article URL or an article title")
    return value


class WikipediaRedirects(HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, new_url):
        wikipedia_url(new_url)
        return super().redirect_request(request, fp, code, message, headers, new_url)


def capture(url, destination):
    request = Request(url, headers={
        "User-Agent": "HardcoreWikipediaAdapterFixture/1.0 (offline rendering regression capture)",
        "Accept": "text/html",
        "Accept-Encoding": "identity",
    })
    with build_opener(WikipediaRedirects()).open(request, timeout=30) as response:
        if "text/html" not in response.headers.get("Content-Type", ""):
            raise ValueError("article response is not HTML")
        data = response.read(MAX_BYTES + 1)
        if len(data) > MAX_BYTES:
            raise ValueError("article exceeds the 8 MiB capture limit")
        final_url = wikipedia_url(response.geturl())
        title = unquote(urlsplit(final_url).path[len("/wiki/"):])
        stem = re.sub(r"[^a-z0-9-]", "-", title.lower()).strip("-")[:120] or "article"
        digest = hashlib.sha256(data).hexdigest()
        filename = stem + "-" + digest[:12] + ".html"
        metadata = {
            "kind": "downloaded Wikipedia HTML",
            "url": final_url,
            "requested_url": url,
            "captured_at": datetime.now(timezone.utc).isoformat(),
            "file": filename,
            "bytes": len(data),
            "sha256": digest,
            "etag": response.headers.get("ETag"),
            "last_modified": response.headers.get("Last-Modified"),
            "license": "Wikipedia content attribution and license are retained in the captured document.",
            "note": "Original server HTML; scripts are not executed by this capture tool.",
        }
    destination.mkdir(parents=True, exist_ok=True)
    (destination / filename).write_bytes(data)
    (destination / (filename + ".json")).write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("articles", nargs="*", help="article titles or HTTPS Wikipedia article URLs")
    parser.add_argument("--representative", action="store_true", help="capture the five article families used by the fixtures")
    parser.add_argument("--output", type=Path, default=Path("build/wikipedia-captures"))
    args = parser.parse_args()
    values = (ARTICLE_URLS if args.representative else []) + args.articles
    if not values:
        parser.error("provide articles or --representative")
    failures = 0
    for value in dict.fromkeys(values):
        try:
            result = capture(wikipedia_url(value), args.output)
            print(f"{result['file']}: {result['bytes']} bytes from {result['url']}")
        except (URLError, OSError, ValueError) as error:
            failures += 1
            print(f"Capture failed for {value}: {error}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
