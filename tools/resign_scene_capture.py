#!/usr/bin/env python3
"""Re-sign a scene capture's files in place and say what changed, next to it.

A capture signs the files it was taken against - the expansion's manifest and
each realm patch archive - with their size and last write, and a replay
refuses one that is not as signed (src/core/scene_capture_files.cpp).

Captures before 2026-11-01 signed the write in the local-time form of
libstdc++'s std::filesystem clock on Windows, which moves by an hour for an
unchanged file when daylight saving starts or ends. The replay no longer reads
that form. This converts it to the UTC write time ("written", nanoseconds since
1970) where the file is still the one signed: the same size, and the same
second or exactly an hour apart. Anything else is a changed file and stops the
script, naming it.

A realm replaces its patch archives in place now and then (2026-10-31: three
of them). Such an archive is accepted only when named with --accept-replaced;
its new signature and the old one go into the provenance. The manifest is
never accepted changed: that is other game data, not the same capture.

    C:\\msys64\\mingw64\\bin\\python.exe tools/resign_scene_capture.py CAPTURE.json
    ... --accept-replaced D:\\games\\WoWee\\Data\\realm_patches\\...\\dbc_diff.MPQ

The capture is rewritten only when something changed, and gains a "resigned"
entry the replay logs as a warning on every run. The provenance file
CAPTURE.resign-<UTC time>.json beside it holds the capture's SHA-256 before
and after and every signature's old and new value.
"""
import argparse
import datetime
import hashlib
import json
import os
import sys
from pathlib import Path

# libstdc++'s std::filesystem::file_time_type counts from 2174-01-01 UTC.
LIBSTDCXX_EPOCH_S = 6437664000
NS = 1_000_000_000
HOUR_S = 3600


def same_file(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def check(entry, accept, is_archive):
    """Return (new signature or None when unchanged, record, refusal)."""
    path = entry["path"]
    try:
        st = os.stat(path)
    except OSError as e:
        return None, None, f"{path}: cannot read ({e.strerror})"
    current = {"path": path, "bytes": st.st_size, "written": st.st_mtime_ns}
    if "written" in entry:
        if entry["bytes"] == st.st_size and entry["written"] == st.st_mtime_ns:
            return None, None, None
        what = f"bytes {entry['bytes']} -> {st.st_size}, written {entry['written']} -> {st.st_mtime_ns} ns UTC"
    else:
        signed_s = entry["modified"] // NS + LIBSTDCXX_EPOCH_S
        offset = signed_s - st.st_mtime_ns // NS
        if entry["bytes"] == st.st_size and offset in (0, HOUR_S, -HOUR_S):
            return current, {"path": path, "reason": f"local-time signature, same size, write {offset:+d} s",
                             "from": entry, "to": current}, None
        what = f"bytes {entry['bytes']} -> {st.st_size}, write moved {offset:+d} s"
    if is_archive and any(same_file(path, a) for a in accept):
        return current, {"path": path, "reason": f"replaced content accepted: {what}",
                         "from": entry, "to": current, "replaced": True}, None
    if not is_archive:
        return None, None, f"{path}: manifest changed ({what}); a capture of other game data cannot be re-signed"
    return None, None, f"{path}: changed ({what}); name it with --accept-replaced if the realm replaced it"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--accept-replaced", action="append", default=[], metavar="ARCHIVE",
                        help="a realm archive whose new content is accepted (repeat for each)")
    args = parser.parse_args()

    source = args.capture.read_bytes()
    doc = json.loads(source)
    if doc.get("version") != 1 or not doc.get("complete"):
        sys.exit(f"{args.capture}: not a complete version 1 scene capture")

    entries = [("manifest", None, doc["manifest"], False)]
    entries += [("archives", i, a, True) for i, a in enumerate(doc["archives"])]
    records, refusals = [], []
    for key, index, entry, is_archive in entries:
        signed, record, refusal = check(entry, args.accept_replaced, is_archive)
        if refusal:
            refusals.append(refusal)
        elif signed:
            if index is None:
                doc[key] = signed
            else:
                doc[key][index] = signed
            records.append(record)
    unused = [a for a in args.accept_replaced
              if not any(r.get("replaced") and same_file(r["path"], a) for r in records)]
    for path in unused:
        refusals.append(f"{path}: named with --accept-replaced but not a changed archive of this capture")
    if refusals:
        for refusal in refusals:
            print("refused:", refusal, file=sys.stderr)
        sys.exit(f"{args.capture}: not re-signed")
    if not records:
        print(f"{args.capture}: every signature is current; nothing re-signed")
        return

    now = datetime.datetime.now(datetime.timezone.utc)
    provenance_path = args.capture.with_name(f"{args.capture.stem}.resign-{now:%Y%m%dT%H%M%SZ}.json")
    replaced = [r["path"] for r in records if r.get("replaced")]
    doc.setdefault("resigned", []).append({
        "at": now.isoformat(timespec="seconds"), "tool": "tools/resign_scene_capture.py",
        "provenance": provenance_path.name, "converted": len(records) - len(replaced), "replaced": replaced})
    derived = json.dumps(doc, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    provenance = {
        "capture": str(args.capture.resolve()),
        "sha256Before": hashlib.sha256(source).hexdigest(),
        "sha256After": hashlib.sha256(derived).hexdigest(),
        "signatures": records,
        "note": "Signatures only. Frames, events, settings and fields are unchanged. A replaced archive "
                "makes runs after this not a controlled comparison with runs before it.",
    }
    with open(provenance_path, "x", encoding="utf-8") as out:
        json.dump(provenance, out, indent=2)
    temporary = args.capture.with_name(args.capture.name + ".part")
    with open(temporary, "xb") as out:
        out.write(derived)
    os.replace(temporary, args.capture)
    print(f"{args.capture}: {len(records) - len(replaced)} signatures moved to UTC write times, "
          f"{len(replaced)} replaced archives accepted; provenance {provenance_path}")
    for path in replaced:
        print("  replaced", path)


if __name__ == "__main__":
    main()
