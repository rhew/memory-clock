#!/usr/bin/env python3

import argparse
import hashlib
import json
import secrets
import sys
from pathlib import Path


DEFAULT_DEVICES_PATH = Path("devices.jsonl")


def hash_token(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


def description_exists(path: Path, description: str) -> bool:
    if not path.exists():
        return False

    with path.open("r", encoding="utf-8") as f:
        for line in f:
            if not line.strip():
                continue

            record = json.loads(line)
            if record.get("description") == description:
                return True

    return False


def store_device(path: Path, description: str, token_hash: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)

    if description_exists(path, description):
        raise ValueError(f"device description already exists: {description}")

    record = {
        "description": description,
        "token_hash": token_hash,
    }

    with path.open("a", encoding="utf-8") as f:
        f.write(json.dumps(record, separators=(",", ":")) + "\n")


def print_result(description: str, token: str, token_only: bool) -> None:
    if token_only:
        print(token)
        return

    print(f"Device: {description}")
    print(f"Bearer token: {token}")
    print()
    print("Copy this bearer token into the device captive portal.")
    print("It will not be shown again.")


def add_device(path: Path, description: str, token_only: bool) -> int:
    token = "mc_" + secrets.token_urlsafe(32)
    store_device(path, description, hash_token(token))
    print_result(description, token, token_only)
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="add-device",
        description="Create a device bearer token.",
    )
    parser.add_argument(
        "-d",
        "--description",
        required=True,
        metavar="DESCRIPTION",
        help="device description",
    )
    parser.add_argument(
        "-p",
        "--path",
        type=Path,
        default=DEFAULT_DEVICES_PATH,
        metavar="PATH",
        help=f"device token file path, default: {DEFAULT_DEVICES_PATH}",
    )
    parser.add_argument(
        "--token-only",
        action="store_true",
        help="print only the bearer token",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    try:
        return add_device(args.path, args.description, args.token_only)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
