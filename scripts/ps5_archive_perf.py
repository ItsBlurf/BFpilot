#!/usr/bin/env python3
"""Run safe BFpilot archive extraction performance tests on a PS5."""

from __future__ import annotations

import datetime as dt
import http.client
import json
import os
import pathlib
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


def q(value: str) -> str:
    return urllib.parse.quote(value, safe="")


def safe_root(path: str) -> bool:
    path = path.rstrip("/")
    return (
        path.startswith("/data/BFpilot/bfpilot-archive-perf-")
        or path.startswith("/data/test/bfpilot-archive-perf-")
    )


def request_json(base: str, path: str, method: str = "GET",
                 data: bytes | None = None,
                 timeout: float = 30.0) -> tuple[bool, object]:
    url = base + path
    try:
        req = urllib.request.Request(url, data=data, method=method)
        with urllib.request.urlopen(req, timeout=timeout) as response:
            parsed = json.loads(response.read())
        ok = response.status == 200 and isinstance(parsed, dict) and parsed.get("ok") is not False
        print(("PASS" if ok else "FAIL") + f" {method} {url} HTTP {response.status}", flush=True)
        return ok, parsed
    except urllib.error.HTTPError as exc:
        try:
            body = exc.read().decode("utf-8", "replace")
        except Exception:
            body = ""
        print(f"FAIL {method} {url}: HTTP {exc.code} {body}", file=sys.stderr, flush=True)
        return False, {"ok": False, "url": url, "http": exc.code, "body": body}
    except (OSError, ValueError, urllib.error.URLError) as exc:
        print(f"FAIL {method} {url}: {exc}", file=sys.stderr, flush=True)
        return False, {"ok": False, "url": url, "error": str(exc)}


def fetch_bytes(base: str, remote_path: str, timeout: float = 30.0) -> tuple[bool, bytes]:
    url = base + "/fs" + "/".join(q(part) for part in remote_path.split("/"))
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            data = response.read()
        print(f"PASS GET {url} HTTP {response.status} bytes={len(data)}", flush=True)
        return True, data
    except urllib.error.HTTPError as exc:
        print(f"SKIP GET {url}: HTTP {exc.code}", flush=True)
        return False, b""
    except (OSError, urllib.error.URLError) as exc:
        print(f"FAIL GET {url}: {exc}", file=sys.stderr, flush=True)
        return False, b""


def wait_job_idle(base: str, label: str) -> tuple[bool, object]:
    last: object = {}
    for _ in range(240):
        ok, job = request_json(base, "/api/fs/job/status", timeout=30)
        last = job
        if not ok:
            return False, job
        if isinstance(job, dict) and not job.get("busy"):
            err = str(job.get("error") or "")
            if err:
                print(f"FAIL {label}: job error={err}", file=sys.stderr, flush=True)
                return False, job
            return True, job
        time.sleep(0.5)
    print(f"FAIL {label}: job timeout", file=sys.stderr, flush=True)
    return False, last


def wait_archive(base: str, poll_interval: float) -> tuple[bool, list[object], object]:
    samples: list[object] = []
    last: object = {}
    failures = 0
    while True:
        ok, status = request_json(base, "/api/fs/archive/status", timeout=15)
        if ok:
            failures = 0
            last = status
            samples.append(status)
            if isinstance(status, dict):
                state = str(status.get("state") or "")
                if state in ("done", "error", "idle"):
                    return state == "done", samples, status
        else:
            failures += 1
            last = status
            samples.append(status)
            if failures >= 5:
                return False, samples, status
        time.sleep(poll_interval)


def archive_result_matches_expectation(done: bool, final_status: object,
                                       expect_error: bool,
                                       error_contains: str) -> bool:
    if done:
        return not expect_error
    if not expect_error or not isinstance(final_status, dict):
        return False
    state = str(final_status.get("state") or "")
    error = str(final_status.get("error") or "")
    if state != "error":
        return False
    if error_contains and error_contains not in error:
        print(
            f"FAIL expected archive error containing {error_contains!r}, got {error!r}",
            file=sys.stderr,
            flush=True,
        )
        return False
    print(f"PASS expected archive error: {error}", flush=True)
    return True


def collect_logs(base: str, out_dir: pathlib.Path, stamp: str) -> dict[str, object]:
    logs = {
        "archive-integrated-status.json": "/data/BFpilot/archive-integrated/status.json",
        "archive-integrated-worker.log": "/data/BFpilot/archive-integrated/archive-worker.log",
        "boot.log": "/data/BFpilot/boot.log",
        "log.txt": "/data/BFpilot/log.txt",
    }
    report: dict[str, object] = {}
    for name, remote in logs.items():
        ok, data = fetch_bytes(base, remote)
        if ok:
            path = out_dir / f"{stamp}-{name}"
            path.write_bytes(data)
            report[name] = {"ok": True, "bytes": len(data), "saved": str(path)}
        else:
            report[name] = {"ok": False}
    return report


def detect_volume_siblings(local_path: pathlib.Path) -> list[pathlib.Path]:
    name = local_path.name
    lower = name.lower()
    parent = local_path.parent
    siblings: list[pathlib.Path] = []

    if lower.endswith(".7z.001"):
        prefix = name[:-3]
        pattern = re.compile(re.escape(prefix) + r"\d{3}$", re.IGNORECASE)
        siblings = [p for p in parent.iterdir() if p.is_file() and pattern.fullmatch(p.name)]
    elif lower.endswith(".part1.rar"):
        prefix = name[:-len("part1.rar")]
        pattern = re.compile(re.escape(prefix) + r"part(\d+)\.rar$", re.IGNORECASE)
        siblings = [p for p in parent.iterdir() if p.is_file() and pattern.fullmatch(p.name)]
        siblings.sort(key=lambda p: int(pattern.fullmatch(p.name).group(1)))  # type: ignore[union-attr]
        return siblings
    elif lower.endswith(".rar"):
        stem = name[:-4]
        pattern = re.compile(re.escape(stem) + r"\.r\d{2}$", re.IGNORECASE)
        siblings = [local_path] + [p for p in parent.iterdir() if p.is_file() and pattern.fullmatch(p.name)]

    if not siblings:
        return [local_path]
    return sorted(set(siblings), key=lambda p: p.name.lower())


def parse_extra_local_paths(value: str) -> list[pathlib.Path]:
    paths: list[pathlib.Path] = []
    for raw in value.split(";"):
        raw = raw.strip()
        if raw:
            paths.append(pathlib.Path(raw))
    return paths


def upload_file(base: str, local_path: pathlib.Path,
                remote_root: str) -> tuple[bool, object, str]:
    upload_timeout = float(os.environ.get("BF_ARCHIVE_UPLOAD_TIMEOUT", "1800"))
    upload_chunk = int(os.environ.get("BF_ARCHIVE_UPLOAD_CHUNK", str(1024 * 1024)))
    filename = local_path.name
    remote_path = f"{remote_root}/{filename}"
    path = (
        "/api/fs/upload?path=" + q(remote_root) +
        "&filename=" + q(filename)
    )
    started = time.monotonic()
    url = urllib.parse.urlsplit(base)
    conn = http.client.HTTPConnection(url.hostname or "", url.port or 80,
                                      timeout=upload_timeout)
    try:
        conn.putrequest("POST", path)
        conn.putheader("Host", url.netloc)
        conn.putheader("Content-Length", str(local_path.stat().st_size))
        conn.endheaders()
        with local_path.open("rb") as src:
            while True:
                chunk = src.read(upload_chunk)
                if not chunk:
                    break
                conn.send(chunk)
        response = conn.getresponse()
        raw = response.read()
        parsed = json.loads(raw)
        ok = response.status == 200 and isinstance(parsed, dict) and parsed.get("ok") is not False
        body = dict(parsed) if isinstance(parsed, dict) else {"ok": False, "body": parsed}
        body["clientElapsedMs"] = round((time.monotonic() - started) * 1000)
        print(("PASS" if ok else "FAIL") + f" POST {base + path} HTTP {response.status}", flush=True)
        return ok, body, remote_path
    except (OSError, ValueError, urllib.error.URLError) as exc:
        print(f"FAIL POST {base + path}: {exc}", file=sys.stderr, flush=True)
        return False, {"ok": False, "url": base + path, "error": str(exc)}, remote_path
    finally:
        conn.close()


def upload_archive_set(base: str, primary: pathlib.Path,
                       remote_root: str) -> tuple[bool, list[object], str, list[str]]:
    upload_siblings = os.environ.get("BF_ARCHIVE_UPLOAD_SIBLINGS", "1") == "1"
    local_paths = detect_volume_siblings(primary) if upload_siblings else [primary]
    local_paths.extend(parse_extra_local_paths(os.environ.get("BF_ARCHIVE_EXTRA_LOCAL", "")))

    deduped: list[pathlib.Path] = []
    seen: set[str] = set()
    for path in local_paths:
        resolved = str(path.resolve()).lower()
        if resolved not in seen:
            deduped.append(path)
            seen.add(resolved)

    uploads: list[object] = []
    remote_primary = f"{remote_root}/{primary.name}"
    uploaded_names: list[str] = []
    all_ok = True
    for path in deduped:
        if not path.is_file():
            body = {"ok": False, "path": str(path), "error": "volume file is missing"}
            print(f"FAIL missing archive volume {path}", file=sys.stderr, flush=True)
            uploads.append(body)
            all_ok = False
            continue
        ok, upload, remote_path = upload_file(base, path, remote_root)
        uploads.append(upload)
        uploaded_names.append(path.name)
        if path.resolve() == primary.resolve():
            remote_primary = remote_path
        all_ok = all_ok and ok
    return all_ok, uploads, remote_primary, uploaded_names


def prepare_archive(base: str, src: str, dst: str, password: str,
                    threads: int, nice: int,
                    cleanup_partial: bool) -> tuple[bool, object]:
    body = urllib.parse.urlencode({
        "src": src,
        "dst": dst,
        "password": password,
        "threads": str(threads),
        "nice": str(nice),
        "cleanupPartial": "1" if cleanup_partial else "0",
    }).encode()
    return request_json(
        base,
        "/api/fs/archive/prepare",
        method="POST",
        data=body,
        timeout=30,
    )


def cleanup_remote_root(base: str, remote_root: str) -> tuple[bool, object]:
    if not safe_root(remote_root):
        return False, {"ok": False, "error": f"unsafe cleanup root {remote_root}"}
    ok, body = request_json(
        base,
        "/api/fs/delete?path=" + q(remote_root) + "&recursive=1",
        timeout=30,
    )
    if not ok:
        return False, body
    job_ok, job = wait_job_idle(base, "cleanup")
    missing_ok, missing = expect_missing(base, remote_root)
    return job_ok and missing_ok, {"delete": body, "job": job, "statAfterDelete": missing}


def expect_missing(base: str, remote_path: str) -> tuple[bool, object]:
    url = base + "/api/fs/stat?path=" + q(remote_path)
    try:
        with urllib.request.urlopen(url, timeout=30) as response:
            parsed = json.loads(response.read())
        print(f"FAIL GET {url} HTTP {response.status}: cleanup path still exists",
              file=sys.stderr, flush=True)
        return False, {"ok": False, "http": response.status, "body": parsed}
    except urllib.error.HTTPError as exc:
        if exc.code == 404:
            print(f"PASS GET {url} HTTP 404 expected missing", flush=True)
            return True, {"ok": True, "missing": True, "http": exc.code}
        try:
            body = exc.read().decode("utf-8", "replace")
        except Exception:
            body = ""
        print(f"FAIL GET {url}: HTTP {exc.code} {body}",
              file=sys.stderr, flush=True)
        return False, {"ok": False, "http": exc.code, "body": body}
    except (OSError, ValueError, urllib.error.URLError) as exc:
        print(f"FAIL GET {url}: {exc}", file=sys.stderr, flush=True)
        return False, {"ok": False, "url": url, "error": str(exc)}


def save_report(report: dict[str, object], output: pathlib.Path,
                phase: str) -> None:
    report["lastSavedPhase"] = phase
    report["lastSavedUtc"] = dt.datetime.now(dt.timezone.utc).strftime(
        "%Y%m%dT%H%M%SZ"
    )
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(f"saved {output} phase={phase}", flush=True)


def parse_threads(value: str) -> list[int]:
    threads: list[int] = []
    for part in value.split(","):
        part = part.strip()
        if not part:
            continue
        try:
            number = int(part, 10)
        except ValueError:
            raise ValueError(f"bad thread value {part!r}") from None
        if number < 0 or number > 8:
            raise ValueError(f"thread value out of range: {number}")
        threads.append(number)
    if not threads:
        raise ValueError("empty thread list")
    return threads


def parse_nice_values(value: str) -> list[int]:
    values: list[int] = []
    for raw in value.split(","):
        raw = raw.strip()
        if not raw:
            continue
        number = int(raw, 10)
        if number < -20:
            number = -20
        if number > 20:
            number = 20
        values.append(number)
    if not values:
        raise ValueError("empty nice list")
    return values


def main() -> int:
    if os.environ.get("BF_ALLOW_PS5_WRITE") != "1":
        print("FAIL BF_ALLOW_PS5_WRITE must be 1 for archive perf tests", file=sys.stderr, flush=True)
        return 2

    local = os.environ.get("BF_ARCHIVE_LOCAL")
    if not local:
        print("FAIL set BF_ARCHIVE_LOCAL=/path/to/test archive", file=sys.stderr, flush=True)
        return 2
    local_path = pathlib.Path(local)
    if not local_path.is_file():
        print(f"FAIL archive does not exist: {local_path}", file=sys.stderr, flush=True)
        return 2

    try:
        threads = parse_threads(os.environ.get("BF_ARCHIVE_THREADS", "0,1,2"))
    except ValueError as exc:
        print(f"FAIL {exc}", file=sys.stderr, flush=True)
        return 2
    try:
        nice_values = parse_nice_values(os.environ.get("BF_ARCHIVE_NICE", "4"))
    except ValueError as exc:
        print(f"FAIL {exc}", file=sys.stderr, flush=True)
        return 2
    if any(value > 2 for value in threads):
        print("WARN manual thread counts above 2 can destabilize some PS5 setups", flush=True)
    if any(value < 0 for value in nice_values):
        print("WARN negative nice values raise archive priority; test them only after stable baseline runs",
              flush=True)

    host = os.environ.get("PS5_IP", "192.168.1.100")
    port = os.environ.get("BF_WEB_PORT", "5905")
    base = f"http://{host}:{port}"
    password = os.environ.get("BF_ARCHIVE_PASSWORD", "")
    cleanup = os.environ.get("BF_ARCHIVE_CLEANUP", "1") == "1"
    cleanup_partial = os.environ.get("BF_ARCHIVE_CLEANUP_PARTIAL", "1") == "1"
    expect_error = os.environ.get("BF_ARCHIVE_EXPECT_ERROR", "0") == "1"
    error_contains = os.environ.get("BF_ARCHIVE_EXPECT_ERROR_CONTAINS", "")
    poll_interval = float(os.environ.get("BF_ARCHIVE_POLL_INTERVAL", "2.0"))
    parent = os.environ.get("BF_ARCHIVE_REMOTE_PARENT", "/data/BFpilot").rstrip("/")
    if parent not in ("/data/BFpilot", "/data/test"):
        print("FAIL BF_ARCHIVE_REMOTE_PARENT must be /data/BFpilot or /data/test", file=sys.stderr, flush=True)
        return 2

    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out_dir = pathlib.Path("diagnostics")
    out_dir.mkdir(exist_ok=True)
    remote_root = f"{parent}/bfpilot-archive-perf-{stamp}-{os.getpid()}"
    if not safe_root(remote_root):
        print(f"FAIL unsafe test root {remote_root}", file=sys.stderr, flush=True)
        return 2

    report: dict[str, object] = {
        "timestampUtc": stamp,
        "baseUrl": base,
        "localArchive": str(local_path),
        "localArchiveBytes": local_path.stat().st_size,
        "uploadSiblings": os.environ.get("BF_ARCHIVE_UPLOAD_SIBLINGS", "1") == "1",
        "passwordProvided": bool(password),
        "threads": threads,
        "niceValues": nice_values,
        "remoteRoot": remote_root,
        "cleanupRequested": cleanup,
        "cleanupPartialRequested": cleanup_partial,
        "expectError": expect_error,
        "expectErrorContains": error_contains,
    }
    output = out_dir / f"ps5-archive-perf-{stamp}.json"
    save_report(report, output, "created")

    ok, support = request_json(base, "/api/fs/archive/support")
    report["support"] = support
    checks = [ok]
    save_report(report, output, "support")

    if all(checks):
        ok, mkdir = request_json(base, "/api/fs/mkdir?path=" + q(remote_root))
        report["mkdir"] = mkdir
        checks.append(ok)
        save_report(report, output, "mkdir")

    remote_archive = ""
    if all(checks):
        ok, uploads, remote_archive, uploaded_names = upload_archive_set(
            base, local_path, remote_root
        )
        report["uploads"] = uploads
        report["uploadedNames"] = uploaded_names
        report["remoteArchive"] = remote_archive
        checks.append(ok)
        save_report(report, output, "upload")

    runs: list[object] = []
    if all(checks):
        stop = False
        for nice in nice_values:
            for thread_count in threads:
                nice_label = f"m{abs(nice)}" if nice < 0 else str(nice)
                dst = f"{remote_root}/out-t{thread_count}-n{nice_label}"
                run: dict[str, object] = {
                    "threads": thread_count,
                    "nice": nice,
                    "destination": dst,
                }
                ok, prepared = prepare_archive(base, remote_archive, dst,
                                               password, thread_count, nice,
                                               cleanup_partial)
                run["prepare"] = prepared
                if ok:
                    done, samples, final_status = wait_archive(base, poll_interval)
                    run["ok"] = archive_result_matches_expectation(
                        done, final_status, expect_error, error_contains
                    )
                    run["samples"] = samples
                    run["finalStatus"] = final_status
                    checks.append(bool(run["ok"]))
                else:
                    run["ok"] = False
                    checks.append(False)
                runs.append(run)
                report["runs"] = runs
                save_report(report, output, f"run-t{thread_count}-n{nice_label}")
                if not run["ok"]:
                    stop = True
                    break
            if stop:
                break
    report["runs"] = runs
    report["logs"] = collect_logs(base, out_dir, stamp)
    save_report(report, output, "logs")

    if cleanup:
        ok, cleanup_body = cleanup_remote_root(base, remote_root)
        report["cleanup"] = cleanup_body
        checks.append(ok)
    else:
        report["cleanup"] = "skipped"

    save_report(report, output, "complete")
    return 0 if all(checks) else 1


if __name__ == "__main__":
    raise SystemExit(main())
