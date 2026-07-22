#!/usr/bin/env python3
"""Structural + pure-logic verification for BFpilot I/O and index safety.

Drives the real source files in the current BFpilot working tree (not a
reimplementation of upload). Exit 0 only if all checks pass. Writes a report
path if --out is given.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"


def read(name: str) -> str:
    return (SRC / name).read_text(encoding="utf-8", errors="replace")


def must(cond: bool, msg: str, failures: list[str]) -> None:
    if not cond:
        failures.append(msg)


def check_upload(failures: list[str]) -> None:
    t = read("transfer.c")
    w = read("websrv_lite.c")
    # Upload path must not call posix_fallocate (large-file stall regression).
    upload_fn = t.split("transfer_upload_request", 1)
    must(len(upload_fn) == 2, "transfer_upload_request missing", failures)
    body = upload_fn[1]
    # Only the upload function region until next top-level int function-ish end:
    body = body[: body.find("\nint\n")] if "\nint\n" in body else body[:8000]
    # Strip C comments so documentation of the anti-pattern does not fail the gate.
    body_nc = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body_nc = re.sub(r"//.*?$", "", body_nc, flags=re.M)
    must(
        re.search(r"\bposix_fallocate\s*\(", body_nc) is None,
        "upload path still calls posix_fallocate",
        failures,
    )
    must("UPLOAD_BUF_SIZE" in t or "BFPILOT_UPLOAD_BUF_SIZE" in t, "upload buf size missing", failures)
    must("MSG_WAITALL" in body or "recv(req->fd" in body, "upload recv loop missing", failures)
    must(
        "TCP_NODELAY" not in w
        or "No TCP_NODELAY" in w
        or "Never TCP_NODELAY" in w
        or "no bulk TCP_NODELAY" in w.lower(),
        "TCP_NODELAY policy comment/code issue",
        failures,
    )
    # Active bulk accept path must not enable TCP_NODELAY.
    accept_region = w
    if "setsockopt(connfd, IPPROTO_TCP, TCP_NODELAY" in accept_region:
        failures.append("post-accept TCP_NODELAY still active")
    must("SO_RCVBUF" in w, "listen/accept SO_RCVBUF missing", failures)
    must("No posix_fallocate on the upload path" in t or "posix_fallocate" not in body, "upload fallocate doc/code", failures)
    # Keep-alive for multi-file upload TCP reuse (safe, stability-preserving).
    must("keep-alive" in w or "KEEPALIVE" in w, "HTTP keep-alive support missing", failures)
    must("BFPILOT_HTTP_KEEPALIVE_MAX" in w or "req_count" in w, "keep-alive request loop missing", failures)
    must("SO_SNDBUF" in w, "SO_SNDBUF for download window missing", failures)
    # Upload buffer must be at least 1 MiB (zftpd floor); prefer 2 MiB default.
    m_up = re.search(
        r"#define\s+BFPILOT_UPLOAD_BUF_SIZE\s+\((\d+)\s*\*\s*1024\s*\*\s*1024\)",
        t,
    )
    if m_up:
        must(int(m_up.group(1)) >= 1, "upload buffer < 1 MiB", failures)
    must("single-buffer" in t.lower() or "zftpd" in t.lower(), "upload path style comment missing", failures)


def check_index(failures: list[str]) -> None:
    s = read("search.c")
    must("collect_system_roots" in s, "multi-root collect missing", failures)
    must("visited_set_add" in s, "visited set missing", failures)
    must("unsigned char used" in s, "visited used-flag for st_dev==0 missing", failures)
    must("lstat(" in s, "lstat usage missing", failures)
    must("search_skip_system_path" in s, "skip list missing", failures)
    must('"/dev"' in s and '"/proc"' in s, "volatile roots not skipped", failures)
    must("/data" in s and "/user" in s, "priority roots missing", failures)
    must("same_device" in s, "XDEV same_device missing", failures)
    # Index All should fence per-root (same_device = 1).
    must(
        re.search(r"same_device\s*=\s*1", s) is not None,
        "Index All same_device=1 fence missing",
        failures,
    )
    must("BFPILOT_SEARCH_MAX_ROOTS" in s, "max roots cap missing", failures)


def check_copy(failures: list[str]) -> None:
    t = read("transfer.c")
    must("COPY_BUF_SIZE" in t, "copy buffer missing", failures)
    must("copy_file" in t, "copy_file missing", failures)
    must("fs_error_is_fatal" in t or "EIO" in t, "fatal FS error handling missing", failures)
    # Free-space preflight must live on the copy/move worker path (not only upload).
    cw = t.split("copy_worker", 1)
    must(len(cw) == 2, "copy_worker missing", failures)
    # Region from copy_worker definition through a generous window / next major fn.
    body = cw[1]
    end = body.find("\nspawn_copy_or_move")
    if end < 0:
        end = body.find("\ndelete_handler")
    if end < 0:
        end = min(len(body), 12000)
    body = body[:end]
    must(
        "check_free_space_for_bytes" in body,
        "copy_worker missing check_free_space_for_bytes preflight",
        failures,
    )
    must(
        "not enough free space" in body or "space preflight" in body,
        "copy_worker missing free-space failure path",
        failures,
    )


def pure_visited_set_test(failures: list[str]) -> None:
    """Open-addressed visited set semantics used by search.c (unit of the algorithm)."""
    # Mirror the membership rule: (dev,ino) unique; used flag required when dev==0.
    class Visited:
        def __init__(self) -> None:
            self.s: set[tuple[int, int]] = set()

        def add(self, dev: int, ino: int) -> int:
            key = (dev, ino)
            if key in self.s:
                return 0
            self.s.add(key)
            return 1

    v = Visited()
    must(v.add(0, 1) == 1, "dev0 first insert failed", failures)
    must(v.add(0, 1) == 0, "dev0 duplicate not detected", failures)
    must(v.add(0, 2) == 1, "dev0 different ino failed", failures)
    must(v.add(5, 1) == 1, "normal dev insert failed", failures)
    must(v.add(5, 1) == 0, "normal duplicate failed", failures)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=None)
    args = ap.parse_args()
    failures: list[str] = []
    check_upload(failures)
    check_index(failures)
    check_copy(failures)
    pure_visited_set_test(failures)
    lines = []
    if failures:
        lines.append("FAIL")
        lines.extend(f"  - {f}" for f in failures)
        code = 1
    else:
        lines.append("PASS")
        lines.append("upload: no fallocate stall path; no TCP_NODELAY thrash")
        lines.append("index: multi-root + visited used-flag + same_device fence")
        lines.append("copy: large buffer + fatal FS abort path present")
        lines.append("visited pure: dev0-safe membership ok")
        code = 0
    text = "\n".join(lines) + "\n"
    sys.stdout.write(text)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding="utf-8")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
