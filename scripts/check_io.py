#!/usr/bin/env python3
"""Check I/O source rules and filesystem traversal.

Exit 0 when all checks pass. Use --out to save the results.
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
    # Streaming copies check available space for each file before copying it.
    cw = t.split("copy_worker", 1)
    must(len(cw) == 2, "copy_worker missing", failures)
    if len(cw) != 2:
        return
    body = cw[1]
    body = body.split("\nstruct delete_arg", 1)[0]
    must("copy_recursive(a->src, staging_dst" in body,
         "copy worker does not use the staged tree copy", failures)
    start = t.find("static int copy_entry(")
    end = t.find("static int copy_recursive(", start)
    must(start >= 0 and end > start, "copy entry callback missing", failures)
    if start < 0 or end <= start:
        return
    entry = t[start:end]
    space = entry.find("check_free_space_for_bytes(")
    write = entry.find("copy_file(")
    must(
        space >= 0 and write > space,
        "copy entry must check space before writing",
        failures,
    )
    must(
        "if(space != 0) return space > 0 ? -ENOSPC : -errno;" in entry,
        "copy entry must return the free-space error",
        failures,
    )
    walker = t[end:].split("\nstruct delete_context", 1)[0]
    must("bfpilot_tree_walk(src, root_dev, copy_entry," in walker,
         "tree copy does not use the checked entry callback", failures)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=None)
    args = ap.parse_args()
    failures: list[str] = []
    check_upload(failures)
    check_index(failures)
    check_copy(failures)
    lines = []
    if failures:
        lines.append("FAIL")
        lines.extend(f"  - {f}" for f in failures)
        code = 1
    else:
        lines.append("PASS")
        lines.append("upload: buffered transfer and keep-alive checks passed")
        lines.append("index: root, visited-directory and filesystem checks passed")
        lines.append("copy: per-file space check and error handling passed")
        code = 0
    text = "\n".join(lines) + "\n"
    sys.stdout.write(text)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding="utf-8")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
