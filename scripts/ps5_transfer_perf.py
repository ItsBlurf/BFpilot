#!/usr/bin/env python3
"""Measure BFpilot upload, download, and local-copy throughput on hardware.

Writes only below a uniquely named /data/test or /data/BFpilot directory and
requires BF_ALLOW_PS5_WRITE=1. The upload body is streamed; RAM use stays
bounded even for multi-gigabyte tests.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import http.client
import json
import os
import pathlib
import sys
import time
import urllib.parse
import urllib.request


def quote(value: str, safe: str = "") -> str:
    return urllib.parse.quote(value, safe=safe)


def json_request(
    host: str,
    port: int,
    path: str,
    method: str = "GET",
    body: bytes | None = None,
    headers: dict[str, str] | None = None,
    timeout: float = 60.0,
) -> tuple[bool, int, object]:
    request = urllib.request.Request(
        f"http://{host}:{port}{path}",
        data=body,
        method=method,
        headers=headers or {},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read()
            status = response.status
        parsed = json.loads(raw) if raw else {}
        return 200 <= status < 300 and parsed.get("ok", True), status, parsed
    except Exception as exc:
        return False, 0, {"error": str(exc), "path": path}


def make_dir(host: str, port: int, path: str) -> tuple[bool, object]:
    ok, _, body = json_request(host, port, f"/api/fs/mkdir?path={quote(path)}")
    return ok, body


def stream_upload(
    host: str, port: int, remote_dir: str, filename: str, size: int, block: bytes
) -> tuple[bool, dict, str]:
    conn = http.client.HTTPConnection(host, port, timeout=600)
    digest = hashlib.sha256()
    sent = 0
    started = time.perf_counter()
    try:
        path = (
            f"/api/fs/upload?path={quote(remote_dir)}&filename={quote(filename)}"
        )
        conn.putrequest("POST", path)
        conn.putheader("Content-Type", "application/octet-stream")
        conn.putheader("Content-Length", str(size))
        conn.putheader("Connection", "close")
        conn.endheaders()
        view = memoryview(block)
        while sent < size:
            length = min(len(block), size - sent)
            conn.send(view[:length])
            digest.update(view[:length])
            sent += length
        response = conn.getresponse()
        raw = response.read()
        elapsed = time.perf_counter() - started
        try:
            server = json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            server = {"raw": raw[:200].decode("utf-8", "replace")}
        result = {
            "ok": response.status == 200 and server.get("ok", True),
            "http": response.status,
            "bytes": sent,
            "elapsedSec": round(elapsed, 3),
            "clientMiBps": round((sent / 1048576.0) / elapsed, 2)
            if elapsed > 0
            else 0.0,
            "server": server,
        }
        return bool(result["ok"]), result, digest.hexdigest()
    except Exception as exc:
        return False, {"ok": False, "bytes": sent, "error": str(exc)}, ""
    finally:
        conn.close()


def stream_download(
    host: str, port: int, remote_path: str, expected_size: int, expected_sha: str
) -> tuple[bool, dict]:
    conn = http.client.HTTPConnection(host, port, timeout=600)
    digest = hashlib.sha256()
    received = 0
    started = time.perf_counter()
    try:
        conn.request("GET", "/fs" + quote(remote_path, safe="/"))
        response = conn.getresponse()
        while True:
            chunk = response.read(2 * 1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
            received += len(chunk)
        elapsed = time.perf_counter() - started
        actual_sha = digest.hexdigest()
        ok = (
            response.status == 200
            and received == expected_size
            and actual_sha == expected_sha
        )
        return ok, {
            "ok": ok,
            "http": response.status,
            "bytes": received,
            "elapsedSec": round(elapsed, 3),
            "clientMiBps": round((received / 1048576.0) / elapsed, 2)
            if elapsed > 0
            else 0.0,
            "sha256": actual_sha,
            "checksumMatch": actual_sha == expected_sha,
        }
    except Exception as exc:
        return False, {"ok": False, "bytes": received, "error": str(exc)}
    finally:
        conn.close()


def start_copy(host: str, port: int, source: str, target_dir: str) -> tuple[bool, object]:
    form = urllib.parse.urlencode(
        {"src": source, "dst": target_dir, "overwrite": "0"}
    ).encode()
    ok, _, body = json_request(
        host,
        port,
        "/api/fs/copy",
        method="POST",
        body=form,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    return ok, body


def wait_job(host: str, port: int, timeout: float) -> tuple[bool, object]:
    deadline = time.monotonic() + timeout
    last: object = {}
    while time.monotonic() < deadline:
        ok, _, body = json_request(host, port, "/api/fs/job/status", timeout=15)
        last = body
        if not ok:
            return False, body
        if isinstance(body, dict) and not body.get("busy"):
            return not bool(body.get("error")), body
        time.sleep(0.25)
    return False, {"error": "copy timeout", "last": last}


def cleanup(host: str, port: int, root: str) -> tuple[bool, object]:
    form = urllib.parse.urlencode({"path": root, "recursive": "1"}).encode()
    ok, _, body = json_request(
        host,
        port,
        "/api/fs/delete",
        method="POST",
        body=form,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    if not ok:
        return False, body
    return wait_job(host, port, 600)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default=os.environ.get("PS5_IP", "ps5"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("BF_WEB_PORT", "5905")))
    parser.add_argument("--size-mib", type=int, default=512)
    parser.add_argument(
        "--root-parent",
        choices=["/data/test", "/data/BFpilot"],
        default=os.environ.get("BF_TEST_REMOTE_ROOT", "/data/test"),
    )
    parser.add_argument("--keep-remote", action="store_true")
    parser.add_argument("--out", default="")
    args = parser.parse_args()

    if os.environ.get("BF_ALLOW_PS5_WRITE") != "1":
        print("FAIL set BF_ALLOW_PS5_WRITE=1", file=sys.stderr)
        return 2
    if args.size_mib < 64:
        print("FAIL --size-mib must be at least 64 to exercise the copy pipeline", file=sys.stderr)
        return 2

    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    root = f"{args.root_parent}/bfpilot-transfer-perf-{stamp}-{os.getpid()}"
    if not root.startswith(args.root_parent + "/bfpilot-transfer-perf-"):
        print("FAIL unsafe generated root", file=sys.stderr)
        return 2
    source = root + "/source.bin"
    target = root + "/copy-target"
    size = args.size_mib * 1024 * 1024
    block = os.urandom(4 * 1024 * 1024)
    report: dict[str, object] = {
        "host": args.host,
        "port": args.port,
        "root": root,
        "sizeMiB": args.size_mib,
        "started": stamp,
    }

    status_ok, _, status = json_request(args.host, args.port, "/api/status", timeout=15)
    report["status"] = status
    if not status_ok:
        print(f"FAIL BFpilot unavailable: {status}", file=sys.stderr)
        return 1

    ok, detail = make_dir(args.host, args.port, root)
    if not ok:
        print(f"FAIL create test root: {detail}", file=sys.stderr)
        return 1

    success = True
    try:
        upload_ok, upload, source_sha = stream_upload(
            args.host, args.port, root, "source.bin", size, block
        )
        report["upload"] = upload
        print(f"{'PASS' if upload_ok else 'FAIL'} upload: {upload}")
        success &= upload_ok

        if upload_ok:
            download_ok, download = stream_download(
                args.host, args.port, source, size, source_sha
            )
            report["download"] = download
            print(f"{'PASS' if download_ok else 'FAIL'} download: {download}")
            success &= download_ok

            stats_ok, _, stats = json_request(
                args.host, args.port, "/api/fs/transfer/stats", timeout=15
            )
            report["transferStatsAfterDownload"] = stats
            success &= stats_ok

            made_target, target_detail = make_dir(args.host, args.port, target)
            if made_target:
                copy_started, copy_start = start_copy(
                    args.host, args.port, source, target
                )
                if copy_started:
                    copy_ok, copy_status = wait_job(args.host, args.port, 1800)
                else:
                    copy_ok, copy_status = False, copy_start
            else:
                copy_ok, copy_status = False, target_detail
            report["copy"] = copy_status
            print(f"{'PASS' if copy_ok else 'FAIL'} copy: {copy_status}")
            success &= copy_ok

            if copy_ok:
                copied_path = target + "/source.bin"
                integrity_ok, integrity = stream_download(
                    args.host, args.port, copied_path, size, source_sha
                )
                report["copyIntegrity"] = integrity
                print(
                    f"{'PASS' if integrity_ok else 'FAIL'} "
                    f"copy integrity: {integrity}"
                )
                success &= integrity_ok

        stats_ok, _, stats = json_request(
            args.host, args.port, "/api/fs/transfer/stats", timeout=15
        )
        report["transferStats"] = stats
        success &= stats_ok
    finally:
        if not args.keep_remote:
            cleanup_ok, cleanup_detail = cleanup(args.host, args.port, root)
            report["cleanup"] = cleanup_detail
            success &= cleanup_ok
            print(f"{'PASS' if cleanup_ok else 'FAIL'} cleanup: {cleanup_detail}")
        else:
            report["cleanup"] = {"skipped": True, "root": root}

    report["ok"] = success
    report["finished"] = dt.datetime.now(dt.timezone.utc).isoformat()
    output = pathlib.Path(args.out) if args.out else pathlib.Path(
        f"logs/ps5-transfer-perf-{stamp}.json"
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"report: {output}")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
