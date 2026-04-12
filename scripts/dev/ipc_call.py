#!/usr/bin/env python3
import argparse
import json
import socket
import struct
import sys
import time


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Send a BetterSpotlight IPC request over a Unix domain socket.")
    parser.add_argument("--socket", required=True, dest="socket_path")
    parser.add_argument("--method", required=True)
    parser.add_argument("--params", default="{}",
                        help="Compact JSON object for request params")
    parser.add_argument("--timeout-ms", type=int, default=2000)
    parser.add_argument("--request-id", type=int, default=None)
    args = parser.parse_args()

    try:
        params = json.loads(args.params)
    except json.JSONDecodeError as exc:
        print(f"invalid params JSON: {exc}", file=sys.stderr)
        return 2

    if not isinstance(params, dict):
        print("params must decode to a JSON object", file=sys.stderr)
        return 2

    request_id = args.request_id
    if request_id is None:
        request_id = int(time.time() * 1000) % 1_000_000_000

    payload = {
        "type": "request",
        "id": request_id,
        "method": args.method,
    }
    if params:
        payload["params"] = params

    body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    message = struct.pack(">I", len(body)) + body

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(max(args.timeout_ms, 1) / 1000.0)
        client.connect(args.socket_path)
        client.sendall(message)

        header = b""
        while len(header) < 4:
            chunk = client.recv(4 - len(header))
            if not chunk:
                raise RuntimeError("short IPC header")
            header += chunk

        (payload_size,) = struct.unpack(">I", header)
        remaining = payload_size
        chunks = []
        while remaining:
            chunk = client.recv(remaining)
            if not chunk:
                raise RuntimeError("short IPC payload")
            chunks.append(chunk)
            remaining -= len(chunk)

    response = json.loads(b"".join(chunks).decode("utf-8"))
    json.dump(response, sys.stdout, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
