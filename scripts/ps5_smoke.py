#!/usr/bin/env python3
"""Exercise BFpilot file APIs using only BFpilot-created files under /data/test."""

from __future__ import annotations

import datetime as dt
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


PAYLOAD = (b"BFpilot v0.3 smoke test\n" * 2048)[:65536]


def q(value: str) -> str:
    return urllib.parse.quote(value, safe="")


def safe_root(path: str) -> bool:
    path = path.rstrip("/")
    return (
        path.startswith("/data/test/bfpilot-smoke-")
        or path.startswith("/data/bfpilot/bfpilot-smoke-")
    )


def request_json(base: str, path: str, method: str = "GET",
                 data: bytes | None = None) -> tuple[bool, object]:
    url = base + path
    try:
        req = urllib.request.Request(url, data=data, method=method)
        with urllib.request.urlopen(req, timeout=30) as response:
            body = response.read()
            parsed = json.loads(body)
        ok = response.status == 200 and isinstance(parsed, dict) and parsed.get("ok") is not False
        print(("PASS" if ok else "FAIL") + f" {method} {url} HTTP {response.status}")
        return ok, parsed
    except (OSError, ValueError, urllib.error.URLError) as exc:
        print(f"FAIL {method} {url}: {exc}", file=sys.stderr)
        return False, {"ok": False, "url": url, "error": str(exc)}


def download(base: str, path: str) -> tuple[bool, bytes]:
    url = base + "/fs" + "/".join(q(part) for part in path.split("/"))
    try:
        with urllib.request.urlopen(url, timeout=30) as response:
            data = response.read()
        ok = data == PAYLOAD
        print(("PASS" if ok else "FAIL") + f" GET {url} HTTP {response.status} bytes={len(data)}")
        return ok, data
    except (OSError, urllib.error.URLError) as exc:
        print(f"FAIL GET {url}: {exc}", file=sys.stderr)
        return False, b""


def expect_missing(base: str, path: str, timeout: float = 3.0) -> tuple[bool, object]:
    url = base + "/api/fs/stat?path=" + q(path)
    deadline = time.monotonic() + timeout
    last: object = {}
    while True:
        try:
            with urllib.request.urlopen(url, timeout=30) as response:
                last = json.loads(response.read())
            if time.monotonic() >= deadline:
                print(f"FAIL GET {url} HTTP {response.status}: path still exists", file=sys.stderr)
                return False, last
        except urllib.error.HTTPError as exc:
            if exc.code == 404:
                print(f"PASS GET {url} HTTP 404 expected missing")
                return True, {"ok": True, "missing": True, "http": exc.code}
            print(f"FAIL GET {url}: HTTP {exc.code}", file=sys.stderr)
            return False, {"ok": False, "http": exc.code}
        except (OSError, ValueError, urllib.error.URLError) as exc:
            print(f"FAIL GET {url}: {exc}", file=sys.stderr)
            return False, {"ok": False, "url": url, "error": str(exc)}
        time.sleep(0.2)


def wait_job(base: str, label: str) -> tuple[bool, object]:
    last: object = {}
    for _ in range(120):
        ok, job = request_json(base, "/api/fs/job/status")
        last = job
        if not ok:
            return False, job
        if isinstance(job, dict) and not job.get("busy"):
            err = str(job.get("error") or "")
            if err:
                print(f"FAIL {label}: job error={err}", file=sys.stderr)
                return False, job
            print(f"PASS {label}: elapsedMs={job.get('elapsedMs')} averageMBps={job.get('averageMBps')}")
            return True, job
        time.sleep(0.25)
    print(f"FAIL {label}: job timeout", file=sys.stderr)
    return False, last


def main() -> int:
    if os.environ.get("BF_ALLOW_PS5_WRITE") != "1":
        print("FAIL BF_ALLOW_PS5_WRITE must be 1 for smoke tests", file=sys.stderr)
        return 2

    host = os.environ.get("PS5_IP", "192.168.1.204")
    port = os.environ.get("BF_WEB_PORT", "5905")
    base = f"http://{host}:{port}"
    parent = os.environ.get("BF_TEST_REMOTE_ROOT", "/data/test").rstrip("/")
    if parent not in ("/data/test", "/data/bfpilot"):
        print("FAIL BF_TEST_REMOTE_ROOT must be /data/test or /data/bfpilot", file=sys.stderr)
        return 2

    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    root = f"{parent}/bfpilot-smoke-{stamp}-{os.getpid()}"
    if not safe_root(root):
        print(f"FAIL unsafe smoke root {root}", file=sys.stderr)
        return 2

    source_file = f"{root}/source.bin"
    copy_dir = f"{root}/copy"
    copy_file = f"{copy_dir}/source.bin"
    renamed_file = f"{copy_dir}/renamed.bin"
    move_dir = f"{root}/moved"
    moved_file = f"{move_dir}/renamed.bin"

    checks: list[bool] = []
    report: dict[str, object] = {"baseUrl": base, "root": root, "cleanup": "pending"}

    for path in (root, copy_dir, move_dir):
        ok, body = request_json(base, "/api/fs/mkdir?path=" + q(path))
        report[f"mkdir:{path}"] = body
        checks.append(ok)
        if not ok:
            break

    if all(checks):
        ok, body = request_json(
            base,
            "/api/fs/upload?path=" + q(root) + "&filename=" + q("source.bin"),
            method="POST",
            data=PAYLOAD,
        )
        report["upload"] = body
        checks.append(ok)

    if all(checks):
        ok, body = request_json(base, "/api/fs/stat?path=" + q(source_file))
        report["statSource"] = body
        checks.append(ok and isinstance(body, dict) and body.get("size") == len(PAYLOAD))

    if all(checks):
        ok, _ = download(base, source_file)
        checks.append(ok)

    if all(checks):
        ok, body = request_json(base, "/api/fs/copy?src=" + q(source_file) + "&dst=" + q(copy_dir))
        report["copyStart"] = body
        checks.append(ok)
        if ok:
            ok, job = wait_job(base, "copy")
            report["copyJob"] = job
            checks.append(ok)

    if all(checks):
        ok, _ = download(base, copy_file)
        checks.append(ok)

    if all(checks):
        ok, body = request_json(base, "/api/fs/rename?src=" + q(copy_file) + "&dst=" + q(renamed_file))
        report["rename"] = body
        checks.append(ok)

    if all(checks):
        ok, body = request_json(base, "/api/fs/move?src=" + q(renamed_file) + "&dst=" + q(move_dir))
        report["moveStart"] = body
        checks.append(ok)
        if ok:
            ok, job = wait_job(base, "move")
            report["moveJob"] = job
            checks.append(ok)

    if all(checks):
        ok, _ = download(base, moved_file)
        checks.append(ok)

    ok, body = request_json(base, "/api/fs/delete?path=" + q(root) + "&recursive=1")
    report["cleanupDelete"] = body
    report["cleanup"] = "deleted" if ok else "failed"
    checks.append(ok)

    ok, body = expect_missing(base, root)
    report["cleanupStat"] = body
    checks.append(ok)

    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if all(checks) else 1


if __name__ == "__main__":
    raise SystemExit(main())
