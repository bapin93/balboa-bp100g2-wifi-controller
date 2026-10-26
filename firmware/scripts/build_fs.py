# Builds the LittleFS image source from data/ before buildfs/uploadfs.
#
# - Text assets are stored gzipped only; ESPAsyncWebServer serves foo.js.gz
#   for /foo.js with Content-Encoding: gzip.
# - Asset references in index.html and manifest.json get a ?v=<content hash>
#   suffix so the firmware can tell browsers to cache them indefinitely.
#
# data/ stays the editable source; the generated tree lives in the build dir.

Import("env")  # noqa: F821

import gzip
import hashlib
import re
import shutil
from pathlib import Path

GZIP_SUFFIXES = {".html", ".js", ".css", ".json", ".svg", ".txt"}
# Files whose asset references are rewritten, in dependency order.
REWRITE_FILES = ["manifest.json", "index.html"]
# Never versioned: the browser fetches these by fixed URL.
UNVERSIONED = {"index.html", "sw.js"}

src_dir = Path(env.subst("$PROJECT_DIR")) / "data"  # noqa: F821
out_dir = Path(env.subst("$BUILD_DIR")) / "fs-data"  # noqa: F821


def content_hash(data):
    return hashlib.sha1(data).hexdigest()[:8]


def rewrite_refs(text, hashes):
    for name, digest in hashes.items():
        pattern = r'(["\'])(/?)' + re.escape(name) + r'(["\'])'
        text = re.sub(pattern, r"\g<1>\g<2>" + name + "?v=" + digest + r"\g<3>", text)
    return text


def build():
    contents = {p.name: p.read_bytes() for p in sorted(src_dir.iterdir()) if p.is_file() and not p.name.startswith(".")}

    hashes = {}
    for name, data in contents.items():
        if name not in UNVERSIONED and name not in REWRITE_FILES:
            hashes[name] = content_hash(data)
    for name in REWRITE_FILES:
        if name in contents:
            contents[name] = rewrite_refs(contents[name].decode("utf-8"), hashes).encode("utf-8")
            if name not in UNVERSIONED:
                hashes[name] = content_hash(contents[name])

    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    raw_total = 0
    stored_total = 0
    for name, data in contents.items():
        raw_total += len(data)
        if Path(name).suffix in GZIP_SUFFIXES:
            packed = gzip.compress(data, compresslevel=9, mtime=0)
            (out_dir / (name + ".gz")).write_bytes(packed)
            stored_total += len(packed)
        else:
            (out_dir / name).write_bytes(data)
            stored_total += len(data)

    print("[build_fs] %s -> %s (%d -> %d bytes)" % (src_dir, out_dir, raw_total, stored_total))


build()
env.Replace(PROJECT_DATA_DIR=str(out_dir))  # noqa: F821
