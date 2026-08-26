#!/usr/bin/env python3
"""Check that every internal href, img src and in-page anchor in a built
MkDocs site resolves.

    tools/check_site_links.py [build/site] [--base /BLSmartFlow/]

Root-relative links in a deployed MkDocs site carry the site_url path prefix
(404.html is generated entirely with them), so that prefix has to be stripped
before resolving against the local build directory. It is read from mkdocs.yml
when --base is not given.
"""
import os, re, sys
from html.parser import HTMLParser
from urllib.parse import urlsplit, unquote

args = [a for a in sys.argv[1:] if not a.startswith("--")]
ROOT = os.path.abspath(args[0] if args else "build/site")

BASE = ""
for i, a in enumerate(sys.argv):
    if a == "--base" and i + 1 < len(sys.argv):
        BASE = sys.argv[i + 1]
if not BASE:
    try:
        for line in open(os.path.join(os.path.dirname(ROOT), "..", "mkdocs.yml")):
            m = re.match(r"\s*site_url:\s*\S+://[^/]+(/.*)", line)
            if m:
                BASE = m.group(1).strip()
                break
    except OSError:
        pass
BASE = "/" + BASE.strip("/") + "/" if BASE.strip("/") else "/"
EXTERNAL = ("http://", "https://", "mailto:", "tel:", "data:", "javascript:", "//")


class Refs(HTMLParser):
    def __init__(self):
        super().__init__()
        self.refs = []      # (kind, value)
        self.ids = set()

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if a.get("id"):
            self.ids.add(a["id"])
        if a.get("name") and tag == "a":
            self.ids.add(a["name"])
        if tag == "a" and a.get("href"):
            self.refs.append(("href", a["href"]))
        elif tag in ("img", "source") and a.get("src"):
            self.refs.append(("img", a["src"]))
        elif tag == "img" and a.get("srcset"):
            self.refs.append(("img", a["srcset"].split()[0]))
        elif tag == "link" and a.get("href") and "stylesheet" in (a.get("rel") or ""):
            self.refs.append(("css", a["href"]))
        elif tag == "script" and a.get("src"):
            self.refs.append(("js", a["src"]))


pages, errors, counts = [], [], {"href": 0, "img": 0, "css": 0, "js": 0, "anchor": 0}
ids_by_page = {}
alt_missing = []

for dirpath, _dirs, files in os.walk(ROOT):
    for f in files:
        if f.endswith(".html"):
            pages.append(os.path.join(dirpath, f))

parsed = {}
for page in pages:
    with open(page, encoding="utf-8") as fh:
        html = fh.read()
    p = Refs()
    p.feed(html)
    parsed[page] = p
    ids_by_page[page] = p.ids
    for tag in re.findall(r"<img\b[^>]*>", html):
        if 'alt="' not in tag and "alt='" not in tag:
            alt_missing.append((os.path.relpath(page, ROOT), tag[:90]))

for page, p in parsed.items():
    base = os.path.dirname(page)
    rel = os.path.relpath(page, ROOT)
    for kind, ref in p.refs:
        if ref.startswith(EXTERNAL):
            continue
        counts[kind] += 1
        split = urlsplit(ref)
        path, frag = unquote(split.path), split.fragment
        if not path:                                    # same-page anchor
            if frag and frag not in p.ids:
                counts["anchor"] += 1
                errors.append("%s: missing anchor #%s" % (rel, frag))
            continue
        if path.startswith("/"):
            stripped = path[len(BASE):] if path.startswith(BASE) else path.lstrip("/")
            target = os.path.normpath(os.path.join(ROOT, stripped))
        else:
            target = os.path.normpath(os.path.join(base, path))
        if os.path.isdir(target):
            target = os.path.join(target, "index.html")
        if not os.path.exists(target):
            errors.append("%s: %s -> %s (missing)" % (rel, kind, ref))
        elif frag and target.endswith(".html"):
            counts["anchor"] += 1
            if target not in ids_by_page:
                q = Refs()
                q.feed(open(target, encoding="utf-8").read())
                ids_by_page[target] = q.ids
            if frag not in ids_by_page[target]:
                errors.append("%s: %s -> %s (anchor #%s not found)" % (rel, kind, ref, frag))

print("site root          : %s" % ROOT)
print("url base path      : %s" % BASE)
print("pages checked      : %d" % len(pages))
print("internal links     : %d" % counts["href"])
print("images             : %d" % counts["img"])
print("css / js           : %d / %d" % (counts["css"], counts["js"]))
print("anchors verified   : %d" % counts["anchor"])
print("images without alt : %d" % len(alt_missing))
for rel, tag in alt_missing:
    print("   %s  %s" % (rel, tag))
if errors:
    print("\nBROKEN (%d):" % len(errors))
    for e in errors:
        print("  " + e)
    sys.exit(1)
print("\nOK - every internal href, image and anchor resolves.")
