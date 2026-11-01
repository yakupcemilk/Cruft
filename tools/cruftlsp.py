#!/usr/bin/env python3
"""Minimal Cruft language server.

Speaks JSON-RPC over stdin/stdout. `textDocument/didOpen` asks `cruftc --check`
about the buffer and publishes the diagnostics. This is a tool, not a language
feature: it does not extend the type system.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def read_message():
    headers = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        if line in (b"\r\n", b"\n"):
            break
        text = line.decode("ascii", errors="replace")
        key, _, value = text.partition(":")
        headers[key.strip().lower()] = value.strip()
    length = int(headers.get("content-length", "0"))
    body = sys.stdin.buffer.read(length)
    if not body:
        return None
    return json.loads(body.decode("utf-8"))


def write_message(payload) -> None:
    data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()


def severity_of(kind: str) -> int:
    if kind == "warning":
        return 2
    if kind == "note":
        return 3
    if kind == "info":
        return 4
    return 1


def diagnostics_for(text: str, cruftc: str, lib: str | None, uri: str):
    directory = tempfile.mkdtemp(prefix="cruftlsp-")
    path = Path(directory) / "buffer.cruft"
    path.write_text(text, encoding="utf-8")
    cmd = [cruftc, "--check", str(path)]
    if lib:
        cmd[1:1] = ["--lib", lib]
    proc = subprocess.run(cmd, text=True, capture_output=True)
    items = []
    for line in (proc.stderr or "").splitlines():
        # <file>:<line>:<col>: error: message
        # The file may be a Windows path, so the severity marker is the anchor.
        found = None
        for kind in ("error", "warning", "note"):
            marker = f": {kind}: "
            idx = line.find(marker)
            if idx >= 0:
                found = (kind, idx, marker)
                break
        if found is None:
            continue
        kind, idx, marker = found
        head = line[:idx]
        message = line[idx + len(marker) :]
        bits = head.rsplit(":", 2)
        if len(bits) != 3:
            continue
        try:
            line_no = int(bits[-2])
            col = int(bits[-1])
        except ValueError:
            continue
        items.append(
            {
                "range": {
                    "start": {"line": max(0, line_no - 1), "character": max(0, col - 1)},
                    "end": {"line": max(0, line_no - 1), "character": max(0, col)},
                },
                "severity": severity_of(kind),
                "source": "cruftc",
                "message": message,
            }
        )
    return {
        "jsonrpc": "2.0",
        "method": "textDocument/publishDiagnostics",
        "params": {"uri": uri, "diagnostics": items},
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cruftc", required=True)
    ap.add_argument("--lib", default="")
    args = ap.parse_args()
    if sys.platform == "win32":
        import msvcrt

        msvcrt.setmode(sys.stdin.fileno(), os.O_BINARY)
        msvcrt.setmode(sys.stdout.fileno(), os.O_BINARY)

    lib = args.lib or None
    while True:
        msg = read_message()
        if msg is None:
            return 0
        method = msg.get("method", "")
        if method == "initialize":
            write_message(
                {
                    "jsonrpc": "2.0",
                    "id": msg.get("id"),
                    "result": {"capabilities": {"textDocumentSync": 1}},
                }
            )
        elif method == "shutdown":
            write_message({"jsonrpc": "2.0", "id": msg.get("id"), "result": None})
        elif method == "exit":
            return 0
        elif method == "textDocument/didOpen":
            doc = msg.get("params", {}).get("textDocument", {})
            text = doc.get("text", "")
            uri = doc.get("uri", "")
            write_message(diagnostics_for(text, args.cruftc, lib, uri))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BrokenPipeError:
        sys.exit(0)
