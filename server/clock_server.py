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
from datetime import date, datetime, time as datetime_time, timedelta, timezone
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
APPOINTMENT_DISPLAY_GRACE = timedelta(hours=1)

STATIC_TZ = "EST5EDT,M3.2.0/2,M11.1.0/2"
STATIC_NTP = "time.cloudflare.com"
DISPLAY_TIMEZONE = ZoneInfo("America/New_York")
BASE_PATH = "/memory-clock"
IMAGE_PATH_PREFIX = f"{BASE_PATH}/images/"
ADMIN_PATH = f"{BASE_PATH}/admin"
ADMIN_API_PATH = f"{ADMIN_PATH}/api"
ADMIN_COOKIE_NAME = "memory_clock_admin"
CLIENT_VERSION_HEADER = "X-Memory-Clock-Version"
MESSAGE_CAPABLE_HEADER = "X-Memory-Clock-Message-Capable"
MESSAGE_ACTIVE_HEADER = "X-Memory-Clock-Message-Active"
MESSAGE_DISPLAYED_HEADER = "X-Memory-Clock-Message-Displayed"
MESSAGE_DISMISSED_HEADER = "X-Memory-Clock-Message-Dismissed"

ADMIN_SESSION_TTL_SECONDS = 12 * 60 * 60
ADMIN_LOGIN_WINDOW_SECONDS = 5 * 60
ADMIN_LOGIN_ATTEMPTS = 10
MAX_ADMIN_LOGIN_BYTES = 4096
MAX_ADMIN_MESSAGE_BYTES = 4096
MAX_CALENDAR_SOURCE_BYTES = 1024 * 1024
MAX_CLIENT_VERSION_LENGTH = 128
MAX_MESSAGE_TEXT_LENGTH = 240
MESSAGE_ID_PATTERN = re.compile(r"[0-9a-f]{24}")
MESSAGE_CHARACTERS = frozenset(
    " !\"#$%&'()*+,-./0123456789:;?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]"
    "abcdefghijklmnopqrstuvwxyz\n"
)

TELEMETRY_HEADERS = {
    "x-memory-clock-battery-mv": ("battery", 2500, 5000),
    "x-memory-clock-last-interaction-s": ("last_input", 0, 3155760000),
    "x-memory-clock-wifi-rssi": ("rssi", -127, 0),
    "x-memory-clock-uptime-s": ("uptime", 0, 4294967295),
}

BASE_DIR = Path(__file__).resolve().parent
DEFAULT_CALENDAR_PATH = BASE_DIR / "calendar.yaml"
DEFAULT_DEVICES_PATH = BASE_DIR / "devices.jsonl"
DEFAULT_ALERTS_PATH = BASE_DIR / "local-data" / "alerts.yaml"
EXAMPLE_ALERTS_PATH = BASE_DIR / "alerts.example.yaml"
DEFAULT_STATE_PATH = BASE_DIR / "local-data" / "memory-clock.sqlite3"
DEFAULT_ADMIN_ASSETS_PATH = BASE_DIR / "admin"

MAX_ALERT_COUNT = 16
MAX_ALERT_NAME_LENGTH = 48
MAX_ALERT_TONES = 16
MIN_ALERT_FREQUENCY_HZ = 500
MAX_ALERT_FREQUENCY_HZ = 3000
MIN_ALERT_DURATION_MS = 20
MAX_ALERT_DURATION_MS = 1000
MAX_ALERT_GAP_MS = 1000
MAX_ALERT_TOTAL_MS = 5000

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


@dataclass(frozen=True)
class AlertTone:
    frequency_hz: int
    duration_ms: int
    gap_ms: int


@dataclass(frozen=True)
class AlertDefinition:
    name: str
    tones: tuple[AlertTone, ...]


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


def parse_appointment_time(value: object) -> tuple[str, datetime_time | None]:
    text = str(value).strip()
    match = re.match(
        r"^(?P<hour>[0-9]{1,2})(?::(?P<minute>[0-9]{2}))?"
        r"\s*(?P<meridiem>[AaPp][Mm])?(?:\b|(?=\s|[-–—]))",
        text,
    )
    if match is None:
        return text, None

    hour = int(match.group("hour"))
    minute_text = match.group("minute")
    meridiem = match.group("meridiem")
    if minute_text is None and meridiem is None:
        return text, None
    minute = int(minute_text or "0")
    if minute > 59:
        return text, None

    if meridiem is not None:
        if not 1 <= hour <= 12:
            return text, None
        hour %= 12
        if meridiem.lower() == "pm":
            hour += 12
    elif hour > 23:
        return text, None

    return text, datetime_time(hour, minute)


def calendar_pages_at(path: Path, now: datetime) -> tuple[list[CalendarPage], int]:
    if now.tzinfo is None:
        raise ValueError("calendar time must include a timezone")
    now = now.astimezone(DISPLAY_TIMEZONE)
    raw = yaml.safe_load(path.read_text(encoding="utf-8")) or []
    today = now.date()
    pages: list[CalendarPage] = []
    today_starts: list[datetime_time] = []
    today_has_unknown_start = False
    for entry in raw:
        when = date.fromisoformat(str(entry["date"]))
        if when < today:
            continue

        plan = str(entry.get("plan", "")).strip()
        appointments_with_starts: list[tuple[datetime_time | None, Appointment]] = []
        for item in entry.get("appointments", []):
            time_text, start = parse_appointment_time(item["time"])
            appointments_with_starts.append((
                start,
                Appointment(
                    time=time_text,
                    title=str(item["title"]).strip(),
                    location=str(item["location"]).strip(),
                ),
            ))
        if all(item[0] is not None for item in appointments_with_starts):
            appointments_with_starts.sort(key=lambda item: item[0] or datetime_time.min)
        appointments = tuple(item[1] for item in appointments_with_starts)
        if not appointments:
            continue
        if when == today:
            today_starts.extend(
                item[0] for item in appointments_with_starts if item[0] is not None
            )
            today_has_unknown_start = today_has_unknown_start or any(
                item[0] is None for item in appointments_with_starts
            )

        label = when.strftime("%B ").replace(" 0", " ") + str(when.day)
        heading = "Today" if when == today else ""
        pages.append(
            CalendarPage(when=when, label=label, plan=plan,
                         appointments=appointments, heading=heading)
        )
    pages.sort(key=lambda page: page.when)

    start_of_today = datetime.combine(today, datetime_time.min, DISPLAY_TIMEZONE)
    display_changed_at = start_of_today
    if today_starts and not today_has_unknown_start:
        last_start = datetime.combine(today, max(today_starts), DISPLAY_TIMEZONE)
        start_of_tomorrow = start_of_today + timedelta(days=1)
        cutoff = min(last_start + APPOINTMENT_DISPLAY_GRACE, start_of_tomorrow)
        if now >= cutoff:
            pages = [page for page in pages if page.when != today]
            display_changed_at = cutoff

    if pages and pages[0].heading == "":
        first_page = pages[0]
        heading = (
            "Tomorrow"
            if first_page.when == today + timedelta(days=1)
            else "Next Appointment"
        )
        pages[0] = CalendarPage(
            when=first_page.when,
            label=first_page.label,
            plan=first_page.plan,
            appointments=first_page.appointments,
            heading=heading,
        )
    return pages, int(display_changed_at.astimezone(timezone.utc).timestamp())


def parse_calendar(path: Path, now: datetime | None = None) -> list[CalendarPage]:
    pages, _ = calendar_pages_at(path, now or datetime.now(DISPLAY_TIMEZONE))
    return pages


def calendar_display_timestamp(path: Path, now: datetime | None = None) -> int:
    _, display_changed_at = calendar_pages_at(
        path,
        now or datetime.now(DISPLAY_TIMEZONE),
    )
    return display_changed_at


def legacy_device_id(token_hash: str) -> str:
    digest = hashlib.sha256(f"memory-clock-device-id:{token_hash}".encode("ascii")).hexdigest()
    return f"legacy-{digest[:20]}"


def valid_device_id(value: str) -> bool:
    return re.fullmatch(r"[A-Za-z0-9_-]{1,64}", value) is not None


def alert_integer(value: object, name: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{name} must be an integer")
    if value < minimum or value > maximum:
        raise ValueError(f"{name} must be between {minimum} and {maximum}")
    return value


def parse_alert_tones(value: object, source: str) -> tuple[AlertTone, ...]:
    if not isinstance(value, list) or not 1 <= len(value) <= MAX_ALERT_TONES:
        raise ValueError(f"{source} must define 1 to {MAX_ALERT_TONES} tones")
    tones: list[AlertTone] = []
    total_ms = 0
    for index, raw_tone in enumerate(value, start=1):
        if not isinstance(raw_tone, dict):
            raise ValueError(f"{source} tone {index} must be an object")
        frequency_hz = alert_integer(
            raw_tone.get("frequency_hz"),
            f"{source} tone {index} frequency_hz",
            MIN_ALERT_FREQUENCY_HZ,
            MAX_ALERT_FREQUENCY_HZ,
        )
        duration_ms = alert_integer(
            raw_tone.get("duration_ms"),
            f"{source} tone {index} duration_ms",
            MIN_ALERT_DURATION_MS,
            MAX_ALERT_DURATION_MS,
        )
        gap_ms = alert_integer(
            raw_tone.get("gap_ms", 0),
            f"{source} tone {index} gap_ms",
            0,
            MAX_ALERT_GAP_MS,
        )
        total_ms += duration_ms + gap_ms
        if total_ms > MAX_ALERT_TOTAL_MS:
            raise ValueError(
                f"{source} must last at most {MAX_ALERT_TOTAL_MS} milliseconds"
            )
        tones.append(AlertTone(frequency_hz, duration_ms, gap_ms))
    return tuple(tones)


def load_alerts(path: Path) -> list[AlertDefinition]:
    raw = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    if not isinstance(raw, dict) or not isinstance(raw.get("alerts"), list):
        raise ValueError("alert file must contain an alerts list")
    raw_alerts = raw["alerts"]
    if len(raw_alerts) > MAX_ALERT_COUNT:
        raise ValueError(f"alert file may define at most {MAX_ALERT_COUNT} alerts")

    alerts: list[AlertDefinition] = []
    seen_names: set[str] = set()
    for index, raw_alert in enumerate(raw_alerts, start=1):
        source = f"alert {index}"
        if not isinstance(raw_alert, dict):
            raise ValueError(f"{source} must be an object")
        name = raw_alert.get("name")
        if (not isinstance(name, str) or not name.strip()
                or len(name.strip()) > MAX_ALERT_NAME_LENGTH
                or not all(character.isprintable() for character in name.strip())):
            raise ValueError(f"{source} has an invalid name")
        name = name.strip()
        normalized_name = name.casefold()
        if normalized_name in seen_names:
            raise ValueError(f"duplicate alert name: {name}")
        tones = parse_alert_tones(raw_alert.get("tones"), source)
        alerts.append(AlertDefinition(name, tones))
        seen_names.add(normalized_name)
    return alerts


def alert_tones_payload(tones: tuple[AlertTone, ...]) -> list[dict[str, int]]:
    return [
        {
            "frequency_hz": tone.frequency_hz,
            "duration_ms": tone.duration_ms,
            "gap_ms": tone.gap_ms,
        }
        for tone in tones
    ]


def stored_alert_payload(message: dict[str, object]) -> dict[str, object] | None:
    tones_json = message.get("alert_tones")
    if not isinstance(tones_json, str):
        return None
    alert_name = message.get("alert_name")
    source = (
        f"stored alert {alert_name}"
        if isinstance(alert_name, str) else "stored alert"
    )
    tones = parse_alert_tones(json.loads(tones_json), source)
    return {
        "tones": alert_tones_payload(tones),
    }


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


def effective_last_modified(calendar_changed_at: int,
                            display_changed_at: int | None = None) -> int:
    if display_changed_at is None:
        display_changed_at = start_of_today_timestamp()
    return max(calendar_changed_at, display_changed_at)


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
        if name.lower() in TELEMETRY_HEADERS
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


def message_id_header(headers, name: str) -> str | None:
    value = headers.get(name, "").strip().lower()
    return value if MESSAGE_ID_PATTERN.fullmatch(value) else None


def validate_message_text(value: object) -> str:
    if not isinstance(value, str):
        raise ValueError("message must be text")
    text = value.replace("\r\n", "\n").replace("\r", "\n").strip()
    if not text:
        raise ValueError("message must not be empty")
    if len(text) > MAX_MESSAGE_TEXT_LENGTH:
        raise ValueError(f"message must be at most {MAX_MESSAGE_TEXT_LENGTH} characters")
    unsupported = sorted({character for character in text
                          if character not in MESSAGE_CHARACTERS})
    if unsupported:
        raise ValueError("message contains characters the clock cannot display")
    return text


def admin_message_payload(message: dict[str, object] | None) -> dict[str, object] | None:
    if message is None:
        return None
    dismissed_at = message.get("dismissed_at")
    displayed_at = message.get("displayed_at")
    state = "dismissed" if dismissed_at is not None else (
        "displayed" if displayed_at is not None else "queued"
    )
    return {
        "id": message["message_id"],
        "text": message.get("message_text"),
        "alert": message.get("alert_name"),
        "state": state,
        "queued_at": message["queued_at"],
        "displayed_at": displayed_at,
        "dismissed_at": dismissed_at,
    }


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
            connection.execute(
                """
                CREATE TABLE IF NOT EXISTS device_messages (
                    device_id TEXT PRIMARY KEY,
                    message_id TEXT NOT NULL,
                    message_text TEXT,
                    alert_name TEXT,
                    alert_tones TEXT,
                    queued_at INTEGER NOT NULL,
                    displayed_at INTEGER,
                    dismissed_at INTEGER
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

    def record_message_state(self, device_id: str, headers) -> None:
        displayed_id = message_id_header(headers, MESSAGE_DISPLAYED_HEADER)
        dismissed_id = message_id_header(headers, MESSAGE_DISMISSED_HEADER)
        if displayed_id is None and dismissed_id is None:
            return

        observed_at = int(time.time())
        with self.connect() as connection:
            if displayed_id is not None:
                connection.execute(
                    """
                    UPDATE device_messages
                    SET displayed_at = COALESCE(displayed_at, ?)
                    WHERE device_id = ? AND message_id = ? AND dismissed_at IS NULL
                    """,
                    (observed_at, device_id, displayed_id),
                )
            if dismissed_id is not None:
                connection.execute(
                    """
                    UPDATE device_messages
                    SET displayed_at = COALESCE(displayed_at, ?),
                        dismissed_at = COALESCE(dismissed_at, ?),
                        message_text = NULL,
                        alert_name = NULL,
                        alert_tones = NULL
                    WHERE device_id = ? AND message_id = ?
                    """,
                    (observed_at, observed_at, device_id, dismissed_id),
                )

    def queue_message(self, device_id: str, text: str,
                      alert: AlertDefinition | None) -> dict[str, object]:
        message_id = secrets.token_hex(12)
        queued_at = int(time.time())
        alert_name = alert.name if alert is not None else None
        alert_tones = (
            json.dumps(alert_tones_payload(alert.tones), separators=(",", ":"))
            if alert is not None else None
        )
        with self.connect() as connection:
            connection.execute(
                """
                INSERT INTO device_messages (
                    device_id, message_id, message_text, alert_name,
                    alert_tones, queued_at,
                    displayed_at, dismissed_at
                ) VALUES (?, ?, ?, ?, ?, ?, NULL, NULL)
                ON CONFLICT(device_id) DO UPDATE SET
                    message_id = excluded.message_id,
                    message_text = excluded.message_text,
                    alert_name = excluded.alert_name,
                    alert_tones = excluded.alert_tones,
                    queued_at = excluded.queued_at,
                    displayed_at = NULL,
                    dismissed_at = NULL
                """,
                (
                    device_id, message_id, text, alert_name,
                    alert_tones, queued_at,
                ),
            )
        return {
            "device_id": device_id,
            "message_id": message_id,
            "message_text": text,
            "alert_name": alert_name,
            "alert_tones": alert_tones,
            "queued_at": queued_at,
            "displayed_at": None,
            "dismissed_at": None,
        }

    def active_message(self, device_id: str) -> dict[str, object] | None:
        with self.connect() as connection:
            row = connection.execute(
                """
                SELECT device_id, message_id, message_text, alert_name,
                       alert_tones, queued_at,
                       displayed_at, dismissed_at
                FROM device_messages
                WHERE device_id = ? AND dismissed_at IS NULL
                      AND message_text IS NOT NULL
                """,
                (device_id,),
            ).fetchone()
        return dict(row) if row is not None else None

    def remove_message(self, device_id: str) -> bool:
        with self.connect() as connection:
            cursor = connection.execute(
                "DELETE FROM device_messages WHERE device_id = ?",
                (device_id,),
            )
        return cursor.rowcount > 0

    def messages_by_device(self) -> dict[str, dict[str, object]]:
        with self.connect() as connection:
            rows = connection.execute(
                """
                SELECT device_id, message_id, message_text, alert_name,
                       alert_tones, queued_at,
                       displayed_at, dismissed_at
                FROM device_messages
                """
            ).fetchall()
        return {str(row["device_id"]): dict(row) for row in rows}

    def status_by_device(self, active_device_ids: set[str]) -> dict[str, dict[str, object]]:
        with self.connect() as connection:
            if active_device_ids:
                placeholders = ",".join("?" for _ in active_device_ids)
                connection.execute(
                    f"DELETE FROM client_status WHERE device_id NOT IN ({placeholders})",
                    tuple(sorted(active_device_ids)),
                )
                connection.execute(
                    f"DELETE FROM device_messages WHERE device_id NOT IN ({placeholders})",
                    tuple(sorted(active_device_ids)),
                )
            else:
                connection.execute("DELETE FROM client_status")
                connection.execute("DELETE FROM device_messages")
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

    def record_client_message_state(self, device: Device) -> None:
        if self.app.state_store is None:
            return
        try:
            self.app.state_store.record_message_state(device.device_id, self.headers)
        except (OSError, sqlite3.Error) as exc:
            print(f"message state update failed for device={log_value(device.description)}: "
                  f"{exc}", flush=True)

    def active_message(self, device: Device) -> dict[str, object] | None:
        if self.app.state_store is None:
            return None
        try:
            return self.app.state_store.active_message(device.device_id)
        except (OSError, sqlite3.Error) as exc:
            print(f"message lookup failed for device={log_value(device.description)}: {exc}",
                  flush=True)
            return None

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
        self.record_client_message_state(device)
        active_message = self.active_message(device)
        active_message_id = (
            str(active_message["message_id"]) if active_message is not None else None
        )
        active_alert = None
        if active_message is not None:
            try:
                active_alert = stored_alert_payload(active_message)
            except (TypeError, ValueError, json.JSONDecodeError) as exc:
                print(f"stored alert is invalid for device={log_value(device.description)}: "
                      f"{exc}", flush=True)
        reported_active_id = message_id_header(self.headers, MESSAGE_ACTIVE_HEADER)
        message_capable = self.headers.get(MESSAGE_CAPABLE_HEADER, "").strip() == "1"
        message_needs_sync = (
            message_capable and active_message_id != reported_active_id
        )

        last_modified = self.app.effective_last_modified()
        if_modified_since = httpdate_to_timestamp(self.headers.get("If-Modified-Since", ""))
        if (not message_needs_sync and if_modified_since is not None
                and last_modified <= if_modified_since):
            self.send_response(HTTPStatus.NOT_MODIFIED)
            self.send_header("Last-Modified", formatdate(last_modified, usegmt=True))
            self.end_headers()
            self.log_page_request(device, HTTPStatus.NOT_MODIFIED)
            return

        payload = build_payload(self.app.calendar_path)
        payload["device"] = device.description
        payload["message"] = (
            {
                "id": active_message["message_id"],
                "text": active_message["message_text"],
                "alert": active_alert,
            }
            if message_capable and active_message is not None else None
        )
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
        if path == f"{ADMIN_API_PATH}/messages":
            self.handle_admin_message()
            return
        self.send_error(HTTPStatus.NOT_FOUND, "not found")

    def do_DELETE(self) -> None:
        path = urlparse(self.path).path
        match = re.fullmatch(
            re.escape(f"{ADMIN_API_PATH}/messages/") + r"([A-Za-z0-9_-]{1,64})",
            path,
        )
        if match:
            self.handle_admin_message_removal(match.group(1))
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

    def handle_admin_message(self) -> None:
        if not self.require_admin():
            return
        if self.headers.get("X-Memory-Clock-CSRF") != "1":
            self.send_json(HTTPStatus.FORBIDDEN, {"error": "missing CSRF header"})
            return
        if self.app.state_store is None:
            self.send_json(HTTPStatus.SERVICE_UNAVAILABLE,
                           {"error": "clock message storage is unavailable"})
            return

        payload = self.read_json_body(MAX_ADMIN_MESSAGE_BYTES)
        if payload is None:
            return
        device_id = payload.get("device_id")
        if not isinstance(device_id, str) or not valid_device_id(device_id):
            self.send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid clock id"})
            return
        alert_name = payload.get("alert")
        if alert_name is not None and (
                not isinstance(alert_name, str) or not alert_name.strip()
                or len(alert_name.strip()) > MAX_ALERT_NAME_LENGTH
                or not all(character.isprintable()
                           for character in alert_name.strip())):
            self.send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid alert name"})
            return
        if isinstance(alert_name, str):
            alert_name = alert_name.strip()
        try:
            text = validate_message_text(payload.get("text"))
        except ValueError as exc:
            self.send_json(HTTPStatus.BAD_REQUEST, {"error": str(exc)})
            return
        try:
            devices = load_devices(self.app.devices_path)
        except (OSError, KeyError, ValueError, json.JSONDecodeError) as exc:
            print(f"admin message clock configuration failed: {exc}", flush=True)
            self.send_json(HTTPStatus.SERVICE_UNAVAILABLE,
                           {"error": "clock configuration is unavailable"})
            return
        if all(device.device_id != device_id for device in devices.values()):
            self.send_json(HTTPStatus.NOT_FOUND, {"error": "clock not found"})
            return

        alert = None
        if alert_name is not None:
            try:
                alerts = load_alerts(self.app.alerts_path)
            except (OSError, ValueError, yaml.YAMLError) as exc:
                print(f"admin alert configuration failed: {exc}", flush=True)
                self.send_json(HTTPStatus.SERVICE_UNAVAILABLE,
                               {"error": "alert sounds are unavailable"})
                return
            alert = next(
                (candidate for candidate in alerts if candidate.name == alert_name),
                None,
            )
            if alert is None:
                self.send_json(HTTPStatus.BAD_REQUEST, {"error": "unknown alert"})
                return

        try:
            message = self.app.state_store.queue_message(device_id, text, alert)
        except (OSError, sqlite3.Error) as exc:
            print(f"admin message queue failed for device={log_value(device_id)}: {exc}",
                  flush=True)
            self.send_json(HTTPStatus.SERVICE_UNAVAILABLE,
                           {"error": "clock message storage is unavailable"})
            return
        self.send_json(HTTPStatus.CREATED, {"message": admin_message_payload(message)})

    def handle_admin_message_removal(self, device_id: str) -> None:
        if not self.require_admin():
            return
        if self.headers.get("X-Memory-Clock-CSRF") != "1":
            self.send_json(HTTPStatus.FORBIDDEN, {"error": "missing CSRF header"})
            return
        if self.app.state_store is None:
            self.send_json(HTTPStatus.SERVICE_UNAVAILABLE,
                           {"error": "clock message storage is unavailable"})
            return
        try:
            devices = load_devices(self.app.devices_path)
        except (OSError, KeyError, ValueError, json.JSONDecodeError) as exc:
            print(f"admin message removal validation failed: {exc}", flush=True)
            self.send_json(HTTPStatus.SERVICE_UNAVAILABLE,
                           {"error": "clock configuration is unavailable"})
            return
        if all(device.device_id != device_id for device in devices.values()):
            self.send_json(HTTPStatus.NOT_FOUND, {"error": "clock not found"})
            return
        try:
            removed = self.app.state_store.remove_message(device_id)
        except (OSError, sqlite3.Error) as exc:
            print(f"admin message removal failed for device={log_value(device_id)}: {exc}",
                  flush=True)
            self.send_json(HTTPStatus.SERVICE_UNAVAILABLE,
                           {"error": "clock message storage is unavailable"})
            return
        self.send_json(HTTPStatus.OK, {"removed": removed})

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
            message_by_device = (
                self.app.state_store.messages_by_device()
                if self.app.state_store is not None else {}
            )
        except (OSError, ValueError, json.JSONDecodeError, sqlite3.Error) as exc:
            print(f"admin client status failed: {exc}", flush=True)
            self.send_json(HTTPStatus.SERVICE_UNAVAILABLE,
                           {"error": "Server could not load clock status."})
            return

        alerts: list[AlertDefinition] = []
        alerts_error = None
        try:
            alerts = load_alerts(self.app.alerts_path)
        except (OSError, ValueError, yaml.YAMLError) as exc:
            print(f"admin alert configuration failed: {exc}", flush=True)
            alerts_error = "Alert sounds are unavailable."

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
                "message": admin_message_payload(
                    message_by_device.get(device.device_id)
                ),
            })
        self.send_json(HTTPStatus.OK, {
            "server_time": int(time.time()),
            "alerts": [alert.name for alert in alerts],
            "alerts_error": alerts_error,
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
                 devices_path: Path, alerts_path: Path, state_path: Path,
                 admin_token_hash: str | None,
                 admin_assets_path: Path = DEFAULT_ADMIN_ASSETS_PATH,
                 secure_admin_cookie: bool = True) -> None:
        super().__init__(server_address, ClockRequestHandler)
        self.calendar_path = calendar_path
        self.devices_path = devices_path
        self.alerts_path = alerts_path
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
            display_changed_at = calendar_display_timestamp(self.calendar_path)
            return effective_last_modified(self.calendar_changed_at, display_changed_at)


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
    alerts_default = Path(
        os.environ.get("MEMORY_CLOCK_ALERTS_PATH", str(DEFAULT_ALERTS_PATH))
    )
    parser.add_argument("--host", default="127.0.0.1", help="bind host, default: 127.0.0.1")
    parser.add_argument("--port", default=8000, type=int, help="bind port, default: 8000")
    parser.add_argument("--calendar", type=Path, default=DEFAULT_CALENDAR_PATH,
                        help=f"calendar YAML path, default: {DEFAULT_CALENDAR_PATH}")
    parser.add_argument("--devices", type=Path, default=DEFAULT_DEVICES_PATH,
                        help=f"device token file, default: {DEFAULT_DEVICES_PATH}")
    parser.add_argument("--alerts", type=Path, default=alerts_default,
                        help=f"alert tone YAML path, default: {alerts_default}")
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
        args.alerts.resolve(),
        args.state.resolve(),
        admin_token_hash,
        args.admin_assets.resolve(),
        secure_admin_cookie=not args.allow_insecure_admin_cookie,
    )
    print(f"Serving {BASE_PATH} on http://{args.host}:{args.port}{BASE_PATH}", flush=True)
    print(f"Calendar: {server.calendar_path}", flush=True)
    print(f"Devices: {server.devices_path}", flush=True)
    print(f"Alerts: {server.alerts_path}", flush=True)
    print(f"State: {args.state.resolve()}", flush=True)
    if server.admin_sessions.enabled:
        print(f"Admin: http://{args.host}:{args.port}{ADMIN_PATH}/", flush=True)
    else:
        print("Admin: disabled (configure an admin token hash)", flush=True)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
