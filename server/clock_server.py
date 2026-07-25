#!/usr/bin/python3

from __future__ import annotations

import argparse
import hashlib
import hmac
import io
import json
import os
import re
import secrets
import sqlite3
import threading
import time
from http.cookies import SimpleCookie
from dataclasses import dataclass
from datetime import date, datetime, time as datetime_time, timezone
from email.utils import formatdate, parsedate_to_datetime
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse
from zoneinfo import ZoneInfo

from PIL import Image, ImageDraw, ImageFont
import yaml


PAGE_WIDTH = 400
PAGE_HEIGHT = 480
PAGE_PADDING = 14
FRAME_TOP = 32
HEADING_Y = 22
FRAME_RADIUS = 18
FRAME_BORDER = 4
DATE_CHIP_TOP = 14
DATE_CHIP_LEFT = 14
PLAN_TOP_PADDING = 24
PLAN_SIDE_PADDING = 12
PLAN_BOTTOM_PADDING = 10
APPOINTMENTS_LEFT = 18
APPOINTMENTS_TOP = 8
APPOINTMENT_GAP = 8
SECTION_GAP = 1
NEXT_HEADING_GAP = 20

STATIC_TZ = "EST5EDT,M3.2.0/2,M11.1.0/2"
STATIC_NTP = "time.cloudflare.com"
DISPLAY_TIMEZONE = ZoneInfo("America/New_York")
BASE_PATH = "/memory-clock"
IMAGE_PATH_PREFIX = f"{BASE_PATH}/images/"
ADMIN_PATH = f"{BASE_PATH}/admin"
ADMIN_API_PATH = f"{ADMIN_PATH}/api"
ADMIN_COOKIE_NAME = "memory_clock_admin"
CLIENT_VERSION_HEADER = "X-Memory-Clock-Version"
TELEMETRY_HEADER_PREFIX = "x-memory-clock-"

ADMIN_SESSION_TTL_SECONDS = 12 * 60 * 60
ADMIN_LOGIN_WINDOW_SECONDS = 5 * 60
ADMIN_LOGIN_ATTEMPTS = 10
MAX_ADMIN_LOGIN_BYTES = 4096
MAX_CALENDAR_SOURCE_BYTES = 1024 * 1024
MAX_CLIENT_VERSION_LENGTH = 128

TELEMETRY_HEADERS = {
    "x-memory-clock-battery-mv": ("battery", 2500, 5000),
    "x-memory-clock-last-interaction-s": ("last_input", 0, 3155760000),
    "x-memory-clock-wifi-rssi": ("rssi", -127, 0),
    "x-memory-clock-uptime-s": ("uptime", 0, 4294967295),
}

BASE_DIR = Path(__file__).resolve().parent
DEFAULT_CALENDAR_PATH = BASE_DIR / "calendar.yaml"
DEFAULT_DEVICES_PATH = BASE_DIR / "devices.jsonl"
DEFAULT_STATE_PATH = BASE_DIR / "local-data" / "memory-clock.sqlite3"
DEFAULT_ADMIN_ASSETS_PATH = BASE_DIR / "admin"

FONT_PATHS = {
    "regular": Path("/usr/share/fonts/truetype/lato/Lato-Regular.ttf"),
    "medium": Path("/usr/share/fonts/truetype/lato/Lato-Medium.ttf"),
    "semibold": Path("/usr/share/fonts/truetype/lato/Lato-Semibold.ttf"),
    "bold": Path("/usr/share/fonts/truetype/lato/Lato-Bold.ttf"),
}


@dataclass(frozen=True)
class Appointment:
    time: str
    title: str
    location: str


@dataclass(frozen=True)
class CalendarPage:
    when: date
    label: str
    plan: str
    appointments: tuple[Appointment, ...]
    heading: str = ""


@dataclass(frozen=True)
class Device:
    device_id: str
    description: str
    token_hash: str


def load_font(kind: str, size: int) -> ImageFont.FreeTypeFont:
    path = FONT_PATHS[kind]
    if not path.exists():
        raise FileNotFoundError(f"missing server font: {path}")
    return ImageFont.truetype(str(path), size)


FONT_DATE = load_font("semibold", 23)
FONT_PLAN = load_font("semibold", 21)
FONT_TIME = load_font("bold", 24)
FONT_HEADING = load_font("semibold", 26)
FONT_TITLE = load_font("semibold", 23)
FONT_LOCATION = load_font("medium", 19)


def parse_calendar(path: Path) -> list[CalendarPage]:
    raw = yaml.safe_load(path.read_text(encoding="utf-8")) or []
    today = datetime.now(DISPLAY_TIMEZONE).date()
    pages: list[CalendarPage] = []
    for entry in raw:
        when = date.fromisoformat(str(entry["date"]))
        if when < today:
            continue

        plan = str(entry.get("plan", "")).strip()
        appointments = tuple(
            Appointment(
                time=str(item["time"]).strip(),
                title=str(item["title"]).strip(),
                location=str(item["location"]).strip(),
            )
            for item in entry.get("appointments", [])
        )
        label = when.strftime("%B ").replace(" 0", " ") + str(when.day)
        heading = "Today" if when == today else ""
        pages.append(
            CalendarPage(when=when, label=label, plan=plan,
                         appointments=appointments, heading=heading)
        )
    pages.sort(key=lambda page: page.when)
    if pages and pages[0].heading == "":
        first_page = pages[0]
        pages[0] = CalendarPage(
            when=first_page.when,
            label=first_page.label,
            plan=first_page.plan,
            appointments=first_page.appointments,
            heading="Next Appointment",
        )
    return pages


def legacy_device_id(token_hash: str) -> str:
    digest = hashlib.sha256(f"memory-clock-device-id:{token_hash}".encode("ascii")).hexdigest()
    return f"legacy-{digest[:20]}"


def valid_device_id(value: str) -> bool:
    return re.fullmatch(r"[A-Za-z0-9_-]{1,64}", value) is not None


def load_devices(path: Path) -> dict[str, Device]:
    devices: dict[str, Device] = {}
    device_ids: set[str] = set()
    if not path.exists():
        return devices

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            if not line.strip():
                continue
            record = json.loads(line)
            description = str(record["description"])
            token_hash = str(record["token_hash"]).lower()
            if not re.fullmatch(r"[0-9a-f]{64}", token_hash):
                raise ValueError("device token_hash must be a SHA-256 hexadecimal digest")
            device_id = str(record.get("id") or legacy_device_id(token_hash))
            if not valid_device_id(device_id):
                raise ValueError(f"invalid device id: {device_id}")
            if token_hash in devices:
                raise ValueError("duplicate device token hash")
            if device_id in device_ids:
                raise ValueError(f"duplicate device id: {device_id}")
            device = Device(device_id=device_id, description=description,
                            token_hash=token_hash)
            devices[token_hash] = device
            device_ids.add(device_id)
    return devices


def httpdate_to_timestamp(value: str) -> int | None:
    try:
        parsed = parsedate_to_datetime(value)
    except (TypeError, ValueError, IndexError, OverflowError):
        return None

    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    else:
        parsed = parsed.astimezone(timezone.utc)
    return int(parsed.timestamp())


def start_of_today_timestamp() -> int:
    now = datetime.now(DISPLAY_TIMEZONE)
    start_of_today = datetime.combine(now.date(), datetime_time.min, DISPLAY_TIMEZONE)
    return int(start_of_today.astimezone(timezone.utc).timestamp())


def path_fingerprint(path: Path) -> str:
    if not path.exists():
        return "missing"
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError:
        return "unreadable"


def effective_last_modified(calendar_changed_at: int) -> int:
    return max(calendar_changed_at, start_of_today_timestamp())


def wrap_text(draw: ImageDraw.ImageDraw, text: str, font: ImageFont.ImageFont,
              max_width: int) -> list[str]:
    lines: list[str] = []
    for paragraph in text.splitlines() or [""]:
        words = paragraph.split()
        if not words:
            lines.append("")
            continue

        current = words[0]
        for word in words[1:]:
            candidate = f"{current} {word}"
            if draw.textlength(candidate, font=font) <= max_width:
                current = candidate
            else:
                lines.append(current)
                current = word
        lines.append(current)
    return lines


def line_height(font: ImageFont.ImageFont) -> int:
    bbox = font.getbbox("Ag")
    return bbox[3] - bbox[1]


def draw_multiline(draw: ImageDraw.ImageDraw, x: int, y: int, text: str,
                   font: ImageFont.ImageFont, max_width: int,
                   line_gap: int = 4) -> int:
    lines = wrap_text(draw, text, font, max_width)
    step = line_height(font) + line_gap
    for index, line in enumerate(lines):
        draw.text((x, y + index * step), line, fill=0, font=font)
    if not lines:
        return y
    return y + len(lines) * step - line_gap


def render_page_image(page: CalendarPage) -> Image.Image:
    image = Image.new("L", (PAGE_WIDTH, PAGE_HEIGHT), 255)
    draw = ImageDraw.Draw(image)

    content_left = PAGE_PADDING
    content_top = FRAME_TOP
    content_right = PAGE_WIDTH - PAGE_PADDING
    plan_width = content_right - content_left

    if page.heading:
        heading_text = page.heading
        heading_width = int(draw.textlength(heading_text, font=FONT_HEADING))
        heading_x = content_left + (plan_width - heading_width) // 2
        draw.text((heading_x, HEADING_Y), heading_text, fill=0, font=FONT_HEADING)
        content_top += line_height(FONT_HEADING) + NEXT_HEADING_GAP

    plan_date_text = f"{page.when.strftime('%A')}, {page.label}"

    plan_text_lines = wrap_text(
        draw,
        page.plan,
        FONT_PLAN,
        plan_width - PLAN_SIDE_PADDING * 2,
    )
    plan_text_height = max(1, len(plan_text_lines)) * line_height(FONT_PLAN) + max(0, len(plan_text_lines) - 1) * 4
    plan_height = PLAN_TOP_PADDING + plan_text_height + PLAN_BOTTOM_PADDING

    plan_box = (
        content_left,
        content_top,
        content_right,
        content_top + plan_height,
    )
    draw.rounded_rectangle(plan_box, radius=FRAME_RADIUS, outline=0, width=FRAME_BORDER)

    chip_width = int(draw.textlength(plan_date_text, font=FONT_DATE)) + 20
    chip_height = line_height(FONT_DATE) + 2
    chip_box = (
        content_left + DATE_CHIP_LEFT,
        content_top - DATE_CHIP_TOP,
        content_left + DATE_CHIP_LEFT + chip_width,
        content_top - DATE_CHIP_TOP + chip_height,
    )
    draw.rectangle(chip_box, fill=255)
    draw.text((chip_box[0] + 10, chip_box[1] - 1), plan_date_text, fill=0, font=FONT_DATE)

    plan_text = "\n".join(plan_text_lines)
    draw_multiline(
        draw,
        content_left + PLAN_SIDE_PADDING,
        content_top + PLAN_TOP_PADDING - 2,
        plan_text,
        FONT_PLAN,
        plan_width - PLAN_SIDE_PADDING * 2,
    )

    y = plan_box[3] + APPOINTMENTS_TOP
    text_width = plan_width - APPOINTMENTS_LEFT

    for appointment in page.appointments:
        draw.text((content_left + APPOINTMENTS_LEFT, y), appointment.time, fill=0, font=FONT_TIME)
        y += line_height(FONT_TIME) + SECTION_GAP
        y = draw_multiline(
            draw,
            content_left + APPOINTMENTS_LEFT,
            y,
            appointment.title,
            FONT_TITLE,
            text_width,
            line_gap=2,
        )
        y += SECTION_GAP
        y = draw_multiline(
            draw,
            content_left + APPOINTMENTS_LEFT,
            y,
            appointment.location,
            FONT_LOCATION,
            text_width,
            line_gap=2,
        )
        y += APPOINTMENT_GAP

    return image


def render_page_bits(image: Image.Image) -> bytes:
    mono = image.convert("1")
    width, height = mono.size
    data = bytearray()

    for y in range(height):
        for byte_start in range(0, width, 8):
            value = 0
            for bit in range(8):
                x = byte_start + bit
                if x >= width:
                    continue
                pixel = mono.getpixel((x, y))
                if pixel == 0:
                    value |= 1 << bit
            data.append(value)

    return bytes(data)


def build_payload(calendar_path: Path) -> dict[str, object]:
    pages = parse_calendar(calendar_path)
    image_pages = []
    for index, page in enumerate(pages, start=1):
        page_name = f"page{index:02d}"
        image_pages.append(
            {
                "name": f"{page_name}.xbm",
                "mime_type": "image/x-xbitmap",
                "encoding": "xbm-bits",
                "width": PAGE_WIDTH,
                "height": PAGE_HEIGHT,
                "date": page.when.isoformat(),
                "label": page.label,
                "bits_path": f"{IMAGE_PATH_PREFIX}{page_name}.bin",
            }
        )

    return {
        "tz": STATIC_TZ,
        "ntp": STATIC_NTP,
        "images": image_pages,
    }


def render_page_bits_by_name(calendar_path: Path, name: str) -> bytes | None:
    pages = parse_calendar(calendar_path)
    if not name.startswith("page") or not name.endswith(".bin"):
        return None
    try:
        index = int(name[4:-4])
    except ValueError:
        return None
    if index < 1 or index > len(pages):
        return None

    return render_page_bits(render_page_image(pages[index - 1]))


def hash_token(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


def bearer_token(headers) -> str | None:
    auth_header = headers.get("Authorization")
    if not auth_header:
        return None
    prefix = "Bearer "
    if not auth_header.startswith(prefix):
        return None
    token = auth_header[len(prefix):].strip()
    return token or None


def log_value(value: str, limit: int = 96) -> str:
    """Return a single-line, bounded representation of an untrusted header value."""
    cleaned = "".join(char if char.isprintable() else "?" for char in value)
    if len(cleaned) > limit:
        cleaned = cleaned[:limit] + "..."
    return json.dumps(cleaned)


def parse_telemetry_integer(value: str, minimum: int, maximum: int) -> int | None:
    value = value.strip()
    digits = value[1:] if value.startswith("-") else value
    if not digits or not digits.isascii() or not digits.isdecimal() or len(value) > 12:
        return None
    try:
        parsed = int(value, 10)
    except ValueError:
        return None
    return parsed if minimum <= parsed <= maximum else None


def format_duration(seconds: int) -> str:
    days, seconds = divmod(seconds, 86400)
    hours, seconds = divmod(seconds, 3600)
    minutes, seconds = divmod(seconds, 60)
    if days:
        return f"{days}d{hours:02}h"
    if hours:
        return f"{hours}h{minutes:02}m"
    if minutes:
        return f"{minutes}m{seconds:02}s"
    return f"{seconds}s"


def format_telemetry_header(name: str, value: str) -> str:
    normalized_name = name.lower()
    known = TELEMETRY_HEADERS.get(normalized_name)
    if known is None:
        return f"{name}={log_value(value)}"

    label, minimum, maximum = known
    parsed = parse_telemetry_integer(value, minimum, maximum)
    if parsed is None:
        return f"{label}=invalid({log_value(value)})"
    if label == "battery":
        return f"battery={parsed / 1000:.3f}V"
    if label == "last_input" or label == "uptime":
        return f"{label}={format_duration(parsed)}"
    return f"rssi={parsed}dBm"


def telemetry_log_fields(headers) -> str:
    fields = [
        format_telemetry_header(name, value)
        for name, value in headers.items()
        if name.lower().startswith(TELEMETRY_HEADER_PREFIX)
    ]
    return "" if not fields else " telemetry=" + ",".join(fields)


def telemetry_value(headers, name: str) -> int | None:
    value = headers.get(name)
    if value is None:
        return None
    _, minimum, maximum = TELEMETRY_HEADERS[name.lower()]
    return parse_telemetry_integer(value, minimum, maximum)


def bounded_client_version(value: str) -> str | None:
    value = "".join(char if char.isprintable() else "?" for char in value.strip())
    if not value or value == "unknown":
        return None
    return value[:MAX_CLIENT_VERSION_LENGTH]


class ClientStateStore:
    def __init__(self, path: Path) -> None:
        self.path = path
        path.parent.mkdir(parents=True, exist_ok=True)
        with self.connect() as connection:
            connection.execute(
                """
                CREATE TABLE IF NOT EXISTS client_status (
                    device_id TEXT PRIMARY KEY,
                    last_seen_at INTEGER NOT NULL,
                    client_version TEXT,
                    battery_mv INTEGER,
                    wifi_rssi INTEGER,
                    uptime_s INTEGER,
                    booted_at INTEGER,
                    last_interaction_at INTEGER
                )
                """
            )

    def connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(self.path, timeout=3)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA busy_timeout = 3000")
        return connection

    def record(self, device: Device, headers) -> None:
        observed_at = int(time.time())
        battery_mv = telemetry_value(headers, "x-memory-clock-battery-mv")
        wifi_rssi = telemetry_value(headers, "x-memory-clock-wifi-rssi")
        uptime_s = telemetry_value(headers, "x-memory-clock-uptime-s")
        last_interaction_s = telemetry_value(
            headers, "x-memory-clock-last-interaction-s"
        )
        booted_at = observed_at - uptime_s if uptime_s is not None else None
        last_interaction_at = (
            observed_at - last_interaction_s
            if last_interaction_s is not None else None
        )
        client_version = bounded_client_version(headers.get(CLIENT_VERSION_HEADER, ""))

        with self.connect() as connection:
            connection.execute(
                """
                INSERT INTO client_status (
                    device_id, last_seen_at, client_version, battery_mv,
                    wifi_rssi, uptime_s, booted_at, last_interaction_at
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(device_id) DO UPDATE SET
                    last_seen_at = excluded.last_seen_at,
                    client_version = excluded.client_version,
                    battery_mv = excluded.battery_mv,
                    wifi_rssi = excluded.wifi_rssi,
                    uptime_s = excluded.uptime_s,
                    booted_at = excluded.booted_at,
                    last_interaction_at = excluded.last_interaction_at
                """,
                (
                    device.device_id,
                    observed_at,
                    client_version,
                    battery_mv,
                    wifi_rssi,
                    uptime_s,
                    booted_at,
                    last_interaction_at,
                ),
            )

    def status_by_device(self, active_device_ids: set[str]) -> dict[str, dict[str, object]]:
        with self.connect() as connection:
            if active_device_ids:
                placeholders = ",".join("?" for _ in active_device_ids)
                connection.execute(
                    f"DELETE FROM client_status WHERE device_id NOT IN ({placeholders})",
                    tuple(sorted(active_device_ids)),
                )
            else:
                connection.execute("DELETE FROM client_status")
            rows = connection.execute(
                """
                SELECT device_id, last_seen_at, client_version, battery_mv,
                       wifi_rssi, uptime_s, booted_at, last_interaction_at
                FROM client_status
                """
            ).fetchall()
        return {str(row["device_id"]): dict(row) for row in rows}


class AdminSessions:
    def __init__(self, admin_token_hash: str | None) -> None:
        self.admin_token_hash = admin_token_hash
        self.sessions: dict[str, float] = {}
        self.failed_logins: list[float] = []
        self.lock = threading.Lock()

    @property
    def enabled(self) -> bool:
        return self.admin_token_hash is not None

    def verify_admin_token(self, token: str) -> bool:
        if self.admin_token_hash is None:
            return False
        return hmac.compare_digest(hash_token(token), self.admin_token_hash)

    def login_allowed(self) -> tuple[bool, int]:
        now = time.monotonic()
        with self.lock:
            self.failed_logins = [
                timestamp for timestamp in self.failed_logins
                if now - timestamp < ADMIN_LOGIN_WINDOW_SECONDS
            ]
            if len(self.failed_logins) < ADMIN_LOGIN_ATTEMPTS:
                return True, 0
            retry_after = int(
                ADMIN_LOGIN_WINDOW_SECONDS - (now - self.failed_logins[0])
            ) + 1
            return False, max(1, retry_after)

    def record_failed_login(self) -> None:
        with self.lock:
            self.failed_logins.append(time.monotonic())
            self.failed_logins = self.failed_logins[-ADMIN_LOGIN_ATTEMPTS:]

    def create(self) -> str:
        token = secrets.token_urlsafe(32)
        session_hash = hash_token(token)
        with self.lock:
            self.failed_logins.clear()
            self.sessions[session_hash] = time.time() + ADMIN_SESSION_TTL_SECONDS
        return token

    def valid(self, token: str | None) -> bool:
        if not token:
            return False
        now = time.time()
        session_hash = hash_token(token)
        with self.lock:
            self.sessions = {
                key: expires_at for key, expires_at in self.sessions.items()
                if expires_at > now
            }
            expires_at = self.sessions.get(session_hash)
        return expires_at is not None and expires_at > now

    def remove(self, token: str | None) -> None:
        if not token:
            return
        with self.lock:
            self.sessions.pop(hash_token(token), None)


class ClockRequestHandler(BaseHTTPRequestHandler):
    server_version = "MemoryClockHTTP/1.0"

    @property
    def app(self) -> "ClockServer":
        return self.server  # type: ignore[return-value]

    def send_bytes(self, status: HTTPStatus, body: bytes, content_type: str,
                   extra_headers: dict[str, str] | None = None) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        if extra_headers:
            for name, value in extra_headers.items():
                self.send_header(name, value)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def send_json(self, status: HTTPStatus, payload: object,
                  extra_headers: dict[str, str] | None = None) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_bytes(status, body, "application/json; charset=utf-8", extra_headers)

    def authenticated_device(self) -> Device | None:
        token = bearer_token(self.headers)
        if token is None:
            self.send_response(HTTPStatus.UNAUTHORIZED)
            self.send_header("WWW-Authenticate", "Bearer")
            self.end_headers()
            return None

        token_hash = hash_token(token)
        devices = load_devices(self.app.devices_path)
        device = devices.get(token_hash)
        if device is None:
            self.send_error(HTTPStatus.FORBIDDEN, "unknown device token")
            return None
        return device

    def client_version(self) -> str:
        value = self.headers.get(CLIENT_VERSION_HEADER, "").strip()
        return value or "unknown"

    def log_page_request(self, device: Device, status: HTTPStatus) -> None:
        print(f"device={log_value(device.description)} client={log_value(self.client_version())} "
              f"GET {BASE_PATH} {status.value}{telemetry_log_fields(self.headers)}", flush=True)

    def record_client_status(self, device: Device) -> None:
        if self.app.state_store is None:
            return
        try:
            self.app.state_store.record(device, self.headers)
        except (OSError, sqlite3.Error) as exc:
            print(f"status update failed for device={log_value(device.description)}: {exc}",
                  flush=True)

    def session_cookie_token(self) -> str | None:
        raw_cookie = self.headers.get("Cookie")
        if not raw_cookie:
            return None
        cookie = SimpleCookie()
        try:
            cookie.load(raw_cookie)
        except Exception:
            return None
        morsel = cookie.get(ADMIN_COOKIE_NAME)
        return morsel.value if morsel is not None else None

    def admin_authenticated(self) -> bool:
        token = bearer_token(self.headers)
        if token is not None and self.app.admin_sessions.verify_admin_token(token):
            return True
        return self.app.admin_sessions.valid(self.session_cookie_token())

    def require_admin(self) -> bool:
        if not self.app.admin_sessions.enabled:
            self.send_json(HTTPStatus.SERVICE_UNAVAILABLE,
                           {"error": "admin access is not configured"})
            return False
        if self.admin_authenticated():
            return True
        self.send_json(HTTPStatus.UNAUTHORIZED, {"error": "authentication required"})
        return False

    def do_GET(self) -> None:
        parsed_url = urlparse(self.path)
        if parsed_url.path == ADMIN_PATH:
            self.send_response(HTTPStatus.SEE_OTHER)
            self.send_header("Location", f"{ADMIN_PATH}/")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        if parsed_url.path.startswith(f"{ADMIN_PATH}/"):
            self.handle_admin_get(parsed_url.path)
            return

        if parsed_url.path.startswith(IMAGE_PATH_PREFIX):
            self.handle_image_request(parsed_url.path)
            return

        if parsed_url.path != BASE_PATH:
            self.send_error(HTTPStatus.NOT_FOUND, "not found")
            return

        device = self.authenticated_device()
        if device is None:
            return
        self.record_client_status(device)

        last_modified = self.app.effective_last_modified()
        if_modified_since = httpdate_to_timestamp(self.headers.get("If-Modified-Since", ""))
        if if_modified_since is not None and last_modified <= if_modified_since:
            self.send_response(HTTPStatus.NOT_MODIFIED)
            self.send_header("Last-Modified", formatdate(last_modified, usegmt=True))
            self.end_headers()
            self.log_page_request(device, HTTPStatus.NOT_MODIFIED)
            return

        payload = build_payload(self.app.calendar_path)
        payload["device"] = device.description
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")

        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Last-Modified", formatdate(last_modified, usegmt=True))
        self.end_headers()
        self.wfile.write(body)
        self.log_page_request(device, HTTPStatus.OK)

    def do_POST(self) -> None:
        path = urlparse(self.path).path
        if path == f"{ADMIN_API_PATH}/login":
            self.handle_admin_login()
            return
        if path == f"{ADMIN_API_PATH}/logout":
            self.handle_admin_logout()
            return
        self.send_error(HTTPStatus.NOT_FOUND, "not found")

    def handle_admin_get(self, path: str) -> None:
        if path == f"{ADMIN_PATH}/":
            self.serve_admin_asset("index.html", "text/html; charset=utf-8")
            return
        if path == f"{ADMIN_PATH}/admin.css":
            self.serve_admin_asset("admin.css", "text/css; charset=utf-8")
            return
        if path == f"{ADMIN_PATH}/admin.js":
            self.serve_admin_asset("admin.js", "text/javascript; charset=utf-8")
            return
        if path == f"{ADMIN_API_PATH}/clients":
            self.handle_admin_clients()
            return
        if path == f"{ADMIN_API_PATH}/pages":
            self.handle_admin_pages()
            return
        if path == f"{ADMIN_API_PATH}/calendar":
            self.handle_admin_calendar()
            return

        match = re.fullmatch(re.escape(f"{ADMIN_API_PATH}/pages/") + r"([1-9][0-9]*)\.png", path)
        if match:
            self.handle_admin_page_preview(int(match.group(1)))
            return
        self.send_error(HTTPStatus.NOT_FOUND, "not found")

    def serve_admin_asset(self, name: str, content_type: str) -> None:
        try:
            body = (self.app.admin_assets_path / name).read_bytes()
        except OSError:
            self.send_error(HTTPStatus.NOT_FOUND, "admin asset not found")
            return
        headers = {
            "Content-Security-Policy": (
                "default-src 'none'; script-src 'self'; style-src 'self'; "
                "img-src 'self'; connect-src 'self'; frame-ancestors 'none'; "
                "base-uri 'none'; form-action 'self'"
            ),
            "Permissions-Policy": "camera=(), microphone=(), geolocation=()",
        }
        self.send_bytes(HTTPStatus.OK, body, content_type, headers)

    def read_json_body(self, maximum: int) -> dict[str, object] | None:
        raw_length = self.headers.get("Content-Length", "")
        try:
            content_length = int(raw_length, 10)
        except ValueError:
            self.send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid Content-Length"})
            return None
        if content_length < 0 or content_length > maximum:
            self.close_connection = True
            self.send_json(HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                           {"error": "request body is too large"})
            return None
        try:
            payload = json.loads(self.rfile.read(content_length))
        except (UnicodeDecodeError, json.JSONDecodeError):
            self.send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid JSON body"})
            return None
        if not isinstance(payload, dict):
            self.send_json(HTTPStatus.BAD_REQUEST, {"error": "JSON body must be an object"})
            return None
        return payload

    def handle_admin_login(self) -> None:
        if not self.app.admin_sessions.enabled:
            self.send_json(HTTPStatus.SERVICE_UNAVAILABLE,
                           {"error": "admin access is not configured"})
            return
        payload = self.read_json_body(MAX_ADMIN_LOGIN_BYTES)
        if payload is None:
            return
        token = payload.get("token")
        if (isinstance(token, str) and 8 <= len(token) <= 512
                and self.app.admin_sessions.verify_admin_token(token)):
            session_token = self.app.admin_sessions.create()
            cookie = (
                f"{ADMIN_COOKIE_NAME}={session_token}; Path={ADMIN_PATH}; "
                f"Max-Age={ADMIN_SESSION_TTL_SECONDS}; HttpOnly; SameSite=Strict"
            )
            if self.app.secure_admin_cookie:
                cookie += "; Secure"
            self.send_json(HTTPStatus.OK, {"authenticated": True}, {"Set-Cookie": cookie})
            return

        allowed, retry_after = self.app.admin_sessions.login_allowed()
        self.app.admin_sessions.record_failed_login()
        if not allowed:
            self.send_json(HTTPStatus.TOO_MANY_REQUESTS,
                           {"error": "too many login attempts"},
                           {"Retry-After": str(retry_after)})
        else:
            self.send_json(HTTPStatus.UNAUTHORIZED, {"error": "invalid admin token"})

    def handle_admin_logout(self) -> None:
        if not self.require_admin():
            return
        if self.headers.get("X-Memory-Clock-CSRF") != "1":
            self.send_json(HTTPStatus.FORBIDDEN, {"error": "missing CSRF header"})
            return
        self.app.admin_sessions.remove(self.session_cookie_token())
        cookie = (
            f"{ADMIN_COOKIE_NAME}=; Path={ADMIN_PATH}; Max-Age=0; "
            "HttpOnly; SameSite=Strict"
        )
        if self.app.secure_admin_cookie:
            cookie += "; Secure"
        self.send_json(HTTPStatus.OK, {"authenticated": False}, {"Set-Cookie": cookie})

    def handle_admin_clients(self) -> None:
        if not self.require_admin():
            return
        try:
            devices = list(load_devices(self.app.devices_path).values())
            status_by_device = (
                self.app.state_store.status_by_device(
                    {device.device_id for device in devices}
                )
                if self.app.state_store is not None else {}
            )
        except (OSError, ValueError, json.JSONDecodeError, sqlite3.Error) as exc:
            print(f"admin client status failed: {exc}", flush=True)
            self.send_json(HTTPStatus.SERVICE_UNAVAILABLE,
                           {"error": "Server could not load clock status."})
            return

        clients: list[dict[str, object]] = []
        for device in sorted(devices, key=lambda item: item.description.casefold()):
            status = status_by_device.get(device.device_id, {})
            clients.append({
                "id": device.device_id,
                "description": device.description,
                "last_seen_at": status.get("last_seen_at"),
                "client_version": status.get("client_version"),
                "battery_mv": status.get("battery_mv"),
                "wifi_rssi": status.get("wifi_rssi"),
                "uptime_s": status.get("uptime_s"),
                "booted_at": status.get("booted_at"),
                "last_interaction_at": status.get("last_interaction_at"),
            })
        self.send_json(HTTPStatus.OK, {
            "server_time": int(time.time()),
            "clients": clients,
        })

    def handle_admin_pages(self) -> None:
        if not self.require_admin():
            return
        try:
            pages = parse_calendar(self.app.calendar_path)
        except (OSError, ValueError, TypeError, yaml.YAMLError) as exc:
            print(f"admin pages failed: {exc}", flush=True)
            self.send_json(HTTPStatus.SERVICE_UNAVAILABLE,
                           {"error": "calendar pages are unavailable"})
            return
        self.send_json(HTTPStatus.OK, {
            "pages": [
                {
                    "number": index,
                    "date": page.when.isoformat(),
                    "label": page.label,
                    "heading": page.heading,
                    "preview_url": f"{ADMIN_API_PATH}/pages/{index}.png",
                }
                for index, page in enumerate(pages, start=1)
            ]
        })

    def handle_admin_page_preview(self, index: int) -> None:
        if not self.require_admin():
            return
        try:
            pages = parse_calendar(self.app.calendar_path)
            if index > len(pages):
                self.send_error(HTTPStatus.NOT_FOUND, "page not found")
                return
            output = io.BytesIO()
            render_page_image(pages[index - 1]).save(output, format="PNG", optimize=True)
        except (OSError, ValueError, TypeError, yaml.YAMLError) as exc:
            print(f"admin page preview failed: {exc}", flush=True)
            self.send_error(HTTPStatus.SERVICE_UNAVAILABLE, "page preview unavailable")
            return
        self.send_bytes(HTTPStatus.OK, output.getvalue(), "image/png")

    def handle_admin_calendar(self) -> None:
        if not self.require_admin():
            return
        try:
            body = self.app.calendar_path.read_bytes()
        except OSError as exc:
            print(f"admin calendar source failed: {exc}", flush=True)
            self.send_error(HTTPStatus.SERVICE_UNAVAILABLE, "calendar unavailable")
            return
        if len(body) > MAX_CALENDAR_SOURCE_BYTES:
            self.send_error(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "calendar is too large")
            return
        self.send_bytes(HTTPStatus.OK, body, "text/yaml; charset=utf-8")

    def handle_image_request(self, path: str) -> None:
        if self.authenticated_device() is None:
            return

        name = path.rsplit("/", 1)[-1]
        bits = render_page_bits_by_name(self.app.calendar_path, name)
        if bits is None:
            self.send_error(HTTPStatus.NOT_FOUND, "image not found")
            return

        last_modified = self.app.effective_last_modified()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(bits)))
        self.send_header("Last-Modified", formatdate(last_modified, usegmt=True))
        self.end_headers()
        self.wfile.write(bits)

    def log_message(self, fmt: str, *args) -> None:
        print("%s - - [%s] %s" % (self.address_string(), self.log_date_time_string(), fmt % args),
              flush=True)


class ClockServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, server_address: tuple[str, int], calendar_path: Path,
                 devices_path: Path, state_path: Path,
                 admin_token_hash: str | None,
                 admin_assets_path: Path = DEFAULT_ADMIN_ASSETS_PATH,
                 secure_admin_cookie: bool = True) -> None:
        super().__init__(server_address, ClockRequestHandler)
        self.calendar_path = calendar_path
        self.devices_path = devices_path
        self.admin_assets_path = admin_assets_path
        self.secure_admin_cookie = secure_admin_cookie
        self.admin_sessions = AdminSessions(admin_token_hash)
        try:
            self.state_store: ClientStateStore | None = ClientStateStore(state_path)
        except (OSError, sqlite3.Error) as exc:
            self.state_store = None
            print(f"Warning: client state is unavailable: {exc}", flush=True)
        self.calendar_lock = threading.Lock()
        self.calendar_fingerprint = path_fingerprint(calendar_path)
        self.calendar_changed_at = int(datetime.now(timezone.utc).timestamp())

    def effective_last_modified(self) -> int:
        with self.calendar_lock:
            fingerprint = path_fingerprint(self.calendar_path)
            if fingerprint != self.calendar_fingerprint:
                self.calendar_fingerprint = fingerprint
                now = int(datetime.now(timezone.utc).timestamp())
                self.calendar_changed_at = max(now, self.calendar_changed_at + 1)
            return effective_last_modified(self.calendar_changed_at)


def validate_sha256(value: str, source: str) -> str:
    value = value.strip().lower()
    if not re.fullmatch(r"[0-9a-f]{64}", value):
        raise ValueError(f"{source} must contain one SHA-256 hexadecimal digest")
    return value


def resolve_admin_token_hash(args: argparse.Namespace) -> str | None:
    if args.admin_token_hash_file is not None:
        value = args.admin_token_hash_file.read_text(encoding="ascii")
        return validate_sha256(value, str(args.admin_token_hash_file))
    environment_path = os.environ.get("MEMORY_CLOCK_ADMIN_TOKEN_HASH_FILE")
    if environment_path:
        path = Path(environment_path)
        value = path.read_text(encoding="ascii")
        return validate_sha256(value, str(path))
    value = os.environ.get("MEMORY_CLOCK_ADMIN_TOKEN_HASH")
    if value:
        return validate_sha256(value, "MEMORY_CLOCK_ADMIN_TOKEN_HASH")
    return None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Serve calendar pages for Memory Clock.")
    parser.add_argument("--host", default="127.0.0.1", help="bind host, default: 127.0.0.1")
    parser.add_argument("--port", default=8000, type=int, help="bind port, default: 8000")
    parser.add_argument("--calendar", type=Path, default=DEFAULT_CALENDAR_PATH,
                        help=f"calendar YAML path, default: {DEFAULT_CALENDAR_PATH}")
    parser.add_argument("--devices", type=Path, default=DEFAULT_DEVICES_PATH,
                        help=f"device token file, default: {DEFAULT_DEVICES_PATH}")
    parser.add_argument("--state", type=Path, default=DEFAULT_STATE_PATH,
                        help=f"SQLite state path, default: {DEFAULT_STATE_PATH}")
    parser.add_argument("--admin-token-hash-file", type=Path,
                        help="file containing the SHA-256 hash of the admin token")
    parser.add_argument("--admin-assets", type=Path, default=DEFAULT_ADMIN_ASSETS_PATH,
                        help=argparse.SUPPRESS)
    parser.add_argument("--allow-insecure-admin-cookie", action="store_true",
                        help="allow the admin session cookie over HTTP; local testing only")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        admin_token_hash = resolve_admin_token_hash(args)
    except (OSError, UnicodeError, ValueError) as exc:
        raise SystemExit(f"invalid admin authentication configuration: {exc}") from exc
    server = ClockServer(
        (args.host, args.port),
        args.calendar.resolve(),
        args.devices.resolve(),
        args.state.resolve(),
        admin_token_hash,
        args.admin_assets.resolve(),
        secure_admin_cookie=not args.allow_insecure_admin_cookie,
    )
    print(f"Serving {BASE_PATH} on http://{args.host}:{args.port}{BASE_PATH}", flush=True)
    print(f"Calendar: {server.calendar_path}", flush=True)
    print(f"Devices: {server.devices_path}", flush=True)
    print(f"State: {args.state.resolve()}", flush=True)
    if server.admin_sessions.enabled:
        print(f"Admin: http://{args.host}:{args.port}{ADMIN_PATH}/", flush=True)
    else:
        print("Admin: disabled (configure an admin token hash)", flush=True)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
