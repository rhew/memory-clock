#!/usr/bin/python3

from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
import urllib.error
import urllib.request
from urllib.parse import urljoin
from pathlib import Path


BASE_DIR = Path(__file__).resolve().parent
DEFAULT_ENV_PATH = BASE_DIR.parent / ".env"


def load_env_token(path: Path) -> str | None:
    if not path.exists():
        return None

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("BEARER_TOKEN="):
            return line.split("=", 1)[1]
    return None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Fetch /memory-clock and dump image pages to a temporary directory.",
    )
    parser.add_argument(
        "--url",
        default="http://127.0.0.1:8000/memory-clock",
        help="clock endpoint URL, default: http://127.0.0.1:8000/memory-clock",
    )
    parser.add_argument(
        "--token",
        help="bearer token; defaults to BEARER_TOKEN from .env",
    )
    parser.add_argument(
        "--env-file",
        type=Path,
        default=DEFAULT_ENV_PATH,
        help=f".env path, default: {DEFAULT_ENV_PATH}",
    )
    return parser.parse_args()


def resolve_token(args: argparse.Namespace) -> str:
    if args.token:
        return args.token

    env_token = os.environ.get("BEARER_TOKEN")
    if env_token:
        return env_token

    file_token = load_env_token(args.env_file)
    if file_token:
        return file_token

    raise SystemExit(
        "missing bearer token: pass --token, set BEARER_TOKEN, or add it to .env"
    )


def fetch_bytes(url: str, token: str) -> bytes:
    request = urllib.request.Request(
        url,
        headers={"Authorization": f"Bearer {token}"},
    )
    try:
        with urllib.request.urlopen(request) as response:
            return response.read()
    except urllib.error.HTTPError as exc:
        raise SystemExit(f"request failed: {exc.code} {exc.reason}") from exc
    except urllib.error.URLError as exc:
        raise SystemExit(f"request failed: {exc.reason}") from exc


def fetch_clock_payload(url: str, token: str) -> dict:
    body = fetch_bytes(url, token)

    try:
        return json.loads(body)
    except json.JSONDecodeError as exc:
        raise SystemExit(f"invalid JSON response: {exc}") from exc


def write_images(payload: dict, url: str, token: str) -> Path:
    output_dir = Path(tempfile.mkdtemp(prefix="memory-clock-pages-"))
    for image in payload.get("images", []):
        name = image["name"]
        target = output_dir / name
        bits = fetch_bytes(urljoin(url, image["bits_path"]), token)
        target.write_bytes(render_xbm(name.removesuffix(".xbm"), image["width"],
                                      image["height"], bits))
    return output_dir


def render_xbm(name: str, width: int, height: int, bits: bytes) -> bytes:
    stride = (width + 7) // 8
    expected_size = stride * height
    if len(bits) != expected_size:
        raise SystemExit(
            f"invalid bit data for {name}: expected {expected_size} bytes, got {len(bits)}"
        )

    lines = [
        f"#define {name}_width {width}",
        f"#define {name}_height {height}",
        f"static char {name}_bits[] = {{",
    ]
    for index in range(0, len(bits), 12):
        chunk = bits[index:index + 12]
        rendered = ", ".join(f"0x{value:02x}" for value in chunk)
        suffix = "," if index + len(chunk) < len(bits) else ""
        lines.append(f"  {rendered}{suffix}")
    lines.append("};")
    lines.append("")
    return "\n".join(lines).encode("ascii")


def main() -> int:
    args = parse_args()
    token = resolve_token(args)
    payload = fetch_clock_payload(args.url, token)
    output_dir = write_images(payload, args.url, token)
    print(output_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
