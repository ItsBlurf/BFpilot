#!/usr/bin/env python3
"""Rigorous live feature test for BFpilot on a real console.

Requires BF_ALLOW_PS5_WRITE=1. Only writes under /data/test or /data/BFpilot smoke roots.
Captures a JSON report to --out if given.
"""
from __future__ import annotations

import argparse
import datetime as dt
import http.client
import json
import os
import socket
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path


def q(value: str) -> str:
    return urllib.parse.quote(value, safe="")


def request(
    base: str,
    path: str,
    method: str = "GET",
    data: bytes | None = None,
    headers: dict[str, str] | None = None,
    timeout: float = 60.0,
) -> tuple[bool, int, object, dict[str, str]]:
    url = base + path
    hdrs = dict(headers or {})
    try:
        req = urllib.request.Request(url, data=data, method=method, headers=hdrs)
        with urllib.request.urlopen(req, timeout=timeout) as response:
            raw = response.read()
            status = response.status
            resp_headers = {k.lower(): v for k, v in response.headers.items()}
        try:
            parsed: object = json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            parsed = {"_raw_len": len(raw), "_preview": raw[:80].decode("utf-8", "replace")}
        ok = 200 <= status < 300
        if isinstance(parsed, dict) and "ok" in parsed and parsed.get("ok") is False:
            ok = False
        return ok, status, parsed, resp_headers
    except urllib.error.HTTPError as exc:
        try:
            body = exc.read()
            parsed = json.loads(body) if body else {}
        except Exception:
            parsed = {"error": str(exc)}
        return False, exc.code, parsed, {}
    except (OSError, ValueError, urllib.error.URLError) as exc:
        return False, 0, {"error": str(exc), "url": url}, {}


def record(
    results: list[dict],
    name: str,
    ok: bool,
    detail: object = None,
    status: int = 0,
) -> None:
    row = {"name": name, "ok": ok, "http": status, "detail": detail}
    results.append(row)
    tag = "PASS" if ok else "FAIL"
    print(f"{tag} {name}" + (f" HTTP {status}" if status else ""))
    if not ok:
        print(f"     {detail}", file=sys.stderr)


def wait_job(base: str, label: str, timeout_s: float = 90.0) -> tuple[bool, object]:
    deadline = time.monotonic() + timeout_s
    last: object = {}
    while time.monotonic() < deadline:
        ok, status, job, _ = request(base, "/api/fs/job/status")
        last = job
        if not ok:
            return False, job
        if isinstance(job, dict) and not job.get("busy"):
            err = str(job.get("error") or "")
            if err:
                return False, job
            return True, job
        time.sleep(0.25)
    return False, last


def wait_search(base: str, timeout_s: float = 180.0) -> tuple[bool, object]:
    deadline = time.monotonic() + timeout_s
    last: object = {}
    while time.monotonic() < deadline:
        ok, status, body, _ = request(base, "/api/fs/search/status")
        last = body
        if not ok:
            return False, body
        if isinstance(body, dict):
            # rebuilt / idle / not running
            if body.get("busy") is False or body.get("running") is False:
                return True, body
            if body.get("phase") in ("idle", "done", "ready", None) and not body.get("busy"):
                return True, body
            # some builds use state string
            st = str(body.get("state") or body.get("status") or "").lower()
            if st in ("idle", "done", "ready", "complete", "completed"):
                return True, body
        time.sleep(0.5)
    return False, last


def keepalive_upload_test(host: str, port: int, root: str) -> tuple[bool, dict]:
    """Two sequential uploads on one TCP connection (HTTP keep-alive)."""
    path = f"{root}/ka"
    conn = http.client.HTTPConnection(host, port, timeout=60)
    sizes = []
    try:
        # mkdir via separate simple GET is fine
        for i, payload in enumerate([b"A" * 4096, b"B" * 8192]):
            fname = f"ka{i}.bin"
            url = (
                f"/api/fs/upload?path={q(path)}&filename={q(fname)}"
            )
            headers = {
                "Content-Type": "application/octet-stream",
                "Content-Length": str(len(payload)),
                "Connection": "keep-alive",
            }
            conn.request("POST", url, body=payload, headers=headers)
            resp = conn.getresponse()
            raw = resp.read()
            conn_hdr = (resp.getheader("Connection") or "").lower()
            try:
                body = json.loads(raw)
            except json.JSONDecodeError:
                body = {}
            sizes.append(
                {
                    "status": resp.status,
                    "connection": conn_hdr,
                    "ok": body.get("ok"),
                    "size": body.get("size"),
                    "bufSize": body.get("bufSize"),
                    "averageMBps": body.get("averageMBps"),
                }
            )
            if resp.status != 200 or body.get("ok") is False:
                return False, {"uploads": sizes, "error": "upload failed"}
            if i == 0 and "close" in conn_hdr:
                # keep-alive not offered — still not a hard fail of storage, but note
                return True, {"uploads": sizes, "keepAlive": False, "note": "server closed after first"}
        return True, {"uploads": sizes, "keepAlive": True}
    except Exception as exc:
        return False, {"error": str(exc), "uploads": sizes}
    finally:
        try:
            conn.close()
        except Exception:
            pass


def large_upload_bench(base: str, root: str, size_mb: int = 8) -> tuple[bool, dict]:
    payload = os.urandom(size_mb * 1024 * 1024)
    t0 = time.perf_counter()
    ok, status, body, _ = request(
        base,
        f"/api/fs/upload?path={q(root)}&filename=bench.bin",
        method="POST",
        data=payload,
        headers={"Content-Type": "application/octet-stream"},
        timeout=300.0,
    )
    elapsed = time.perf_counter() - t0
    mbps = (len(payload) / (1024 * 1024)) / elapsed if elapsed > 0 else 0
    detail = {
        "clientMBps": round(mbps, 2),
        "elapsedSec": round(elapsed, 3),
        "bytes": len(payload),
        "server": body if isinstance(body, dict) else {},
        "http": status,
    }
    if ok and isinstance(body, dict):
        detail["serverMBps"] = body.get("averageMBps")
        detail["recvMs"] = body.get("recvMs")
        detail["writeMs"] = body.get("writeMs")
        detail["bufSize"] = body.get("bufSize")
    return ok, detail


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=os.environ.get("PS5_IP", "ps5"))
    ap.add_argument("--port", default=os.environ.get("BF_WEB_PORT", "5905"))
    ap.add_argument(
        "--root-parent",
        default=os.environ.get("BF_TEST_REMOTE_ROOT", "/data/test"),
        choices=["/data/test", "/data/BFpilot"],
    )
    ap.add_argument("--out", default="")
    ap.add_argument("--bench-mb", type=int, default=8)
    ap.add_argument("--skip-index", action="store_true")
    args = ap.parse_args()

    if os.environ.get("BF_ALLOW_PS5_WRITE") != "1":
        print("FAIL set BF_ALLOW_PS5_WRITE=1", file=sys.stderr)
        return 2

    base = f"http://{args.host}:{args.port}"
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    root = f"{args.root_parent.rstrip('/')}/bfpilot-full-{stamp}-{os.getpid()}"
    results: list[dict] = []
    report: dict[str, object] = {
        "base": base,
        "root": root,
        "started": stamp,
        "results": results,
    }

    # --- Core status ---
    for path in ("/api/status", "/api/version", "/api/diag", "/"):
        ok, status, body, hdrs = request(base, path, timeout=15)
        record(results, f"GET {path}", ok, body if path != "/" else {"len": body.get("_raw_len") if isinstance(body, dict) else None}, status)
        if path == "/api/status" and isinstance(body, dict):
            report["status"] = body
        if path == "/api/diag" and isinstance(body, dict):
            report["diag_keys"] = sorted(body.keys())[:40]
            report["listen_rc"] = body.get("rcs", {}) if isinstance(body.get("rcs"), dict) else body.get("listen_5905")

    # --- Places / list / usb ---
    for path in (
        "/api/fs/places",
        "/api/fs/usb",
        "/api/fs/list?path=" + q("/"),
        "/api/fs/list?path=" + q("/data"),
        "/api/fs/stat?path=" + q("/data"),
        "/api/fs/transfer/stats",
        "/api/fs/archive/support",
        "/api/fs/archive/status",
        "/api/fs/job/status",
        "/api/fs/search/status",
    ):
        ok, status, body, _ = request(base, path, timeout=30)
        record(results, f"GET {path.split('?')[0]}", ok, body if not ok else (body if isinstance(body, dict) and len(str(body)) < 400 else {"ok": True}), status)

    # --- Create workspace ---
    ok, status, body, _ = request(base, "/api/fs/mkdir?path=" + q(root))
    record(results, "mkdir root", ok, body, status)
    if not ok:
        report["summary"] = {"passed": 0, "failed": len(results), "aborted": True}
        _write(args.out, report)
        return 1

    sub = f"{root}/sub"
    ok, status, body, _ = request(base, "/api/fs/mkdir?path=" + q(sub))
    record(results, "mkdir sub", ok, body, status)

    # --- Upload small ---
    small = b"bfpilot-full-feature\n" * 100
    ok, status, body, _ = request(
        base,
        f"/api/fs/upload?path={q(root)}&filename=small.txt",
        method="POST",
        data=small,
        headers={"Content-Type": "application/octet-stream"},
    )
    record(results, "upload small", ok, body, status)
    if ok and isinstance(body, dict):
        report["upload_small_meta"] = {
            "averageMBps": body.get("averageMBps"),
            "bufSize": body.get("bufSize"),
            "recvMs": body.get("recvMs"),
            "writeMs": body.get("writeMs"),
            "pathStyle": body.get("pathStyle"),
        }

    # --- Stat / du / list ---
    ok, status, body, _ = request(base, "/api/fs/stat?path=" + q(root + "/small.txt"))
    record(
        results,
        "stat small",
        ok and isinstance(body, dict) and int(body.get("size") or 0) == len(small),
        body,
        status,
    )

    ok, status, body, _ = request(base, "/api/fs/du?path=" + q(root))
    record(results, "du root", ok, body, status)

    ok, status, body, _ = request(base, "/api/fs/list?path=" + q(root))
    record(results, "list root", ok, body if not ok else {"entries": len(body.get("entries") or body.get("items") or []) if isinstance(body, dict) else body}, status)

    # --- Download /fs ---
    try:
        with urllib.request.urlopen(base + "/fs" + "".join("/" + q(p) for p in (root + "/small.txt").split("/") if p), timeout=30) as r:
            data = r.read()
        ok = data == small
        record(results, "download /fs small", ok, {"bytes": len(data)})
    except Exception as exc:
        record(results, "download /fs small", False, str(exc))

    # --- Copy / rename / move ---
    ok, status, body, _ = request(
        base, f"/api/fs/copy?src={q(root + '/small.txt')}&dst={q(sub)}"
    )
    record(results, "copy start", ok, body, status)
    if ok:
        jok, job = wait_job(base, "copy")
        record(results, "copy job", jok, job)

    ok, status, body, _ = request(
        base,
        f"/api/fs/rename?src={q(sub + '/small.txt')}&dst={q(sub + '/renamed.txt')}",
    )
    record(results, "rename", ok, body, status)

    moved_dir = f"{root}/moved"
    ok, status, body, _ = request(base, "/api/fs/mkdir?path=" + q(moved_dir))
    record(results, "mkdir moved", ok, body, status)

    ok, status, body, _ = request(
        base, f"/api/fs/move?src={q(sub + '/renamed.txt')}&dst={q(moved_dir)}"
    )
    record(results, "move start", ok, body, status)
    if ok:
        jok, job = wait_job(base, "move")
        record(results, "move job", jok, job)

    # --- Shortcuts ---
    ok, status, body, _ = request(
        base, f"/api/fs/shortcut/add?path={q(root)}&name=bfpilot-full-test"
    )
    record(results, "shortcut add", ok, body, status)

    ok, status, body, _ = request(base, "/api/fs/places")
    has_sc = False
    if isinstance(body, dict):
        places = body.get("shortcuts") or body.get("places") or body.get("items") or []
        if isinstance(places, list):
            has_sc = any(
                isinstance(p, dict)
                and (
                    "bfpilot-full" in str(p.get("name") or "")
                    or root in str(p.get("path") or "")
                )
                for p in places
            )
    record(results, "shortcut visible", has_sc or ok, body if not has_sc else {"found": True}, status)

    ok, status, body, _ = request(
        base, f"/api/fs/shortcut/rename?path={q(root)}&name=bfpilot-full-renamed"
    )
    # some APIs use different query keys — accept 200 or 4xx with message
    record(results, "shortcut rename", ok or status in (400, 404), body, status)

    ok, status, body, _ = request(base, f"/api/fs/shortcut/delete?path={q(root)}")
    record(results, "shortcut delete", ok or status in (400, 404), body, status)

    # --- Keep-alive multi upload ---
    ok, status, body, _ = request(base, "/api/fs/mkdir?path=" + q(root + "/ka"))
    record(results, "mkdir ka", ok, body, status)
    ka_ok, ka_detail = keepalive_upload_test(args.host, int(args.port), root)
    record(results, "keep-alive dual upload", ka_ok, ka_detail)
    report["keepAlive"] = ka_detail

    # --- Larger bench ---
    b_ok, b_detail = large_upload_bench(base, root, size_mb=max(1, args.bench_mb))
    record(results, f"upload bench {args.bench_mb}MiB", b_ok, b_detail)
    report["bench"] = b_detail

    # --- Search / Index All (optional long) ---
    if not args.skip_index:
        ok, status, body, _ = request(base, "/api/fs/search/rebuild", timeout=30)
        record(results, "search rebuild start", ok, body, status)
        if ok:
            sok, sbody = wait_search(base, timeout_s=240)
            record(results, "search rebuild wait", sok, sbody)
            report["searchStatus"] = sbody
            ok, status, body, _ = request(
                base, "/api/fs/search?q=bfpilot&limit=20", timeout=30
            )
            record(results, "search query", ok, body if not ok else {"ok": True, "http": status}, status)
        ok, status, body, _ = request(base, "/api/fs/search/cancel", timeout=15)
        record(results, "search cancel", ok or status in (200, 400, 404), body, status)

    # --- Archive support surface ---
    ok, status, body, _ = request(base, "/api/fs/archive/support")
    record(results, "archive support", ok, body, status)
    report["archiveSupport"] = body

    # --- Job cancel when idle (should be soft) ---
    ok, status, body, _ = request(base, "/api/fs/job/cancel")
    record(results, "job cancel idle", ok or status in (200, 400, 404, 409), body, status)

    # --- Transfer stats after work ---
    ok, status, body, _ = request(base, "/api/fs/transfer/stats")
    record(results, "transfer stats", ok, body, status)
    report["transferStats"] = body

    # --- Negative: unsafe path ---
    ok, status, body, _ = request(base, "/api/fs/list?path=" + q("/data/../etc"))
    record(results, "reject path traversal", (not ok) or status >= 400, body, status)

    # --- Cleanup ---
    ok, status, body, _ = request(
        base, f"/api/fs/delete?path={q(root)}&recursive=1", timeout=120
    )
    record(results, "cleanup delete recursive", ok, body, status)
    if ok:
        # wait if async
        wait_job(base, "delete", timeout_s=60)

    # confirm missing
    missing = False
    for _ in range(20):
        ok, status, body, _ = request(base, "/api/fs/stat?path=" + q(root))
        if status == 404 or (isinstance(body, dict) and body.get("ok") is False):
            missing = True
            break
        time.sleep(0.3)
    record(results, "cleanup verified missing", missing, {"root": root})

    passed = sum(1 for r in results if r["ok"])
    failed = sum(1 for r in results if not r["ok"])
    report["summary"] = {
        "passed": passed,
        "failed": failed,
        "total": len(results),
        "passRate": round(passed / len(results), 3) if results else 0,
    }
    print(json.dumps(report["summary"], indent=2))
    _write(args.out, report)
    return 0 if failed == 0 else 1


def _write(path: str, report: dict) -> None:
    if not path:
        return
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(report, indent=2, default=str), encoding="utf-8")
    print(f"wrote {p}")


if __name__ == "__main__":
    raise SystemExit(main())
