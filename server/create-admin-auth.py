#!/usr/bin/env python3

import argparse
import hashlib
import os
import secrets
import sys
from pathlib import Path


BASE_DIR = Path(__file__).resolve().parent
DEFAULT_TOKEN_PATH = BASE_DIR / "admin.token"
DEFAULT_HASH_PATH = BASE_DIR / "local-data" / "admin-token.sha256"


def write_secret(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "w", encoding="ascii") as handle:
            handle.write(value)
            handle.write("\n")
        path.chmod(0o600)
    except Exception:
        path.unlink(missing_ok=True)
        raise


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Create a Memory Clock admin token and its server-side hash file.",
    )
    parser.add_argument("--token-file", type=Path, default=DEFAULT_TOKEN_PATH,
                        help=f"browser auth file, default: {DEFAULT_TOKEN_PATH}")
    parser.add_argument("--hash-file", type=Path, default=DEFAULT_HASH_PATH,
                        help=f"server hash file, default: {DEFAULT_HASH_PATH}")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    token = "ma_" + secrets.token_urlsafe(32)
    token_hash = hashlib.sha256(token.encode("utf-8")).hexdigest()
    try:
        write_secret(args.token_file, token)
        try:
            write_secret(args.hash_file, token_hash)
        except Exception:
            args.token_file.unlink(missing_ok=True)
            raise
    except FileExistsError as exc:
        print(f"error: refusing to overwrite {exc.filename}", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print(f"Admin auth file: {args.token_file}")
    print(f"Server hash file: {args.hash_file}")
    print("Keep both files private. Upload the auth file on the admin sign-in page.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
