#!/usr/bin/env python3
"""Collect BFpilot HTTP diagnostics without mutating the PS5."""

from __future__ import annotations

import datetime as dt
import json
import os
import pathlib
import sys
import time
import urllib.parse
import urllib.error
import urllib.request


def fetch_json(base: str, path: str) -> tuple[bool, object]:
    url = base + path
    try:
        with urllib.request.urlopen(url, timeout=8) as response:
            raw = response.read()
            body = json.loads(raw)
            if response.status != 200:
                raise RuntimeError(f"HTTP {response.status}")
            print(f"PASS GET {url} HTTP {response.status}")
            return True, body
    except (OSError, ValueError, urllib.error.URLError) as exc:
        print(f"FAIL GET {url}: {exc}", file=sys.stderr)
        return False, {"ok": False, "url": url, "error": str(exc)}

def fetch_logs(base: str, out_dir: pathlib.Path, stamp: str) -> dict[str, object]:
    results: dict[str, object] = {}
    for name in ("boot.log", "log.txt", "crash.log", "launcher-installer.log"):
        url = base + "/fs/data/BFpilot/" + urllib.parse.quote(name)
        try:
            with urllib.request.urlopen(url, timeout=8) as response:
                data = response.read()
            path = out_dir / f"{stamp}-{name}"
            path.write_bytes(data)
            results[name] = {"ok": True, "bytes": len(data), "saved": str(path)}
            print(f"PASS GET {url} HTTP {response.status}")
        except urllib.error.HTTPError as exc:
            results[name] = {"ok": False, "http": exc.code}
            print(f"SKIP GET {url}: HTTP {exc.code}")
        except OSError as exc:
            results[name] = {"ok": False, "error": str(exc)}
            print(f"FAIL GET {url}: {exc}", file=sys.stderr)
    return results

def allowed_remote(path: str) -> bool:
    roots = os.environ.get("BF_ALLOWED_REMOTE_ROOTS", "/data/test/bfpilot-bench")
    return any(path == root.rstrip("/") or path.startswith(root.rstrip("/") + "/")
               for root in roots.split(",")
               if root.startswith("/data/test") or root.startswith("/data/bfpilot"))


def run_bench(base: str) -> tuple[bool, object]:
    if os.environ.get("BF_ALLOW_PS5_WRITE", "0") != "1":
        return False, {"ok": False, "skipped": True,
                       "reason": "BF_ALLOW_PS5_WRITE is not 1"}

    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    root = "/data/test/bfpilot-bench"
    source_dir = f"{root}/source-{stamp}"
    copy_dir = f"{root}/copy-{stamp}"
    filename = "bfpilot-bench-8m.bin"
    if not allowed_remote(source_dir) or not allowed_remote(copy_dir):
        return False, {"ok": False, "skipped": True,
                       "reason": "benchmark paths not in BF_ALLOWED_REMOTE_ROOTS"}

    results: dict[str, object] = {
        "sourceDir": source_dir,
        "copyDir": copy_dir,
        "cleanup": "not performed; diagnostics never delete remote files",
    }
    for path in (source_dir, copy_dir):
        ok, body = fetch_json(base, "/api/fs/mkdir?path=" + urllib.parse.quote(path))
        results[f"mkdir:{path}"] = body
        if not ok:
            return False, results

    payload = bytes(8 * 1024 * 1024)
    upload_url = (base + "/api/fs/upload?path=" + urllib.parse.quote(source_dir) +
                  "&filename=" + urllib.parse.quote(filename))
    request = urllib.request.Request(upload_url, data=payload, method="POST")
    try:
        started = time.monotonic()
        with urllib.request.urlopen(request, timeout=120) as response:
            upload = json.loads(response.read())
        results["upload"] = upload
        results["clientUploadElapsedMs"] = round((time.monotonic() - started) * 1000)
        print(f"PASS POST {upload_url} HTTP {response.status}")
    except (OSError, ValueError, urllib.error.URLError) as exc:
        results["upload"] = {"ok": False, "error": str(exc)}
        print(f"FAIL POST {upload_url}: {exc}", file=sys.stderr)
        return False, results

    source = f"{source_dir}/{filename}"
    copy_path = ("/api/fs/copy?src=" + urllib.parse.quote(source) +
                 "&dst=" + urllib.parse.quote(copy_dir))
    ok, results["copyStart"] = fetch_json(base, copy_path)
    if not ok:
        return False, results
    for _ in range(240):
        ok, job = fetch_json(base, "/api/fs/job/status")
        results["copyJob"] = job
        if not ok or not isinstance(job, dict) or not job.get("busy"):
            return bool(ok and isinstance(job, dict) and not job.get("error")), results
        time.sleep(0.5)
    results["copyJob"] = {"ok": False, "error": "timeout"}
    return False, results


def main() -> int:
    host = os.environ.get("PS5_IP", "192.168.1.204")
    port = os.environ.get("BF_WEB_PORT", "5905")
    allow_write = os.environ.get("BF_ALLOW_PS5_WRITE", "0") == "1"
    base = f"http://{host}:{port}"
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out_dir = pathlib.Path("diagnostics")
    out_dir.mkdir(exist_ok=True)

    status_ok, status = fetch_json(base, "/api/status")
    places_ok, places = fetch_json(base, "/api/fs/places")
    diag_ok = False
    if isinstance(status, dict) and status.get("diagReadOnly") is True:
        diag_ok, diag = fetch_json(base, "/api/diag")
    elif allow_write:
        print("WARN /api/diag read-only capability absent; querying because BF_ALLOW_PS5_WRITE=1")
        diag_ok, diag = fetch_json(base, "/api/diag")
    else:
        diag = {
            "ok": False,
            "skipped": True,
            "reason": "server does not advertise diagReadOnly=true",
        }
        diag_ok = True
        print("SKIP GET /api/diag: deployed server may create/delete probe files")

    report = {
        "timestampUtc": stamp,
        "baseUrl": base,
        "allowPs5Write": allow_write,
        "status": status,
        "places": places,
        "diag": diag,
    }
    if "--bench" in sys.argv:
        bench_ok, bench = run_bench(base)
        report["bench"] = bench
    else:
        bench_ok = True
    if "--logs" in sys.argv:
        report["logs"] = fetch_logs(base, out_dir, stamp)
    output = out_dir / f"ps5-diag-{stamp}.json"
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(f"saved {output}")
    return 0 if status_ok and places_ok and diag_ok and bench_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
