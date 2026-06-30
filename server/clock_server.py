#!/usr/bin/python3

from __future__ import annotations

import argparse
import hashlib
import json
import threading
from dataclasses import dataclass
from datetime import date, datetime, time, timezone
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
CLIENT_VERSION_HEADER = "X-Memory-Clock-Version"

BASE_DIR = Path(__file__).resolve().parent
DEFAULT_CALENDAR_PATH = BASE_DIR / "calendar.yaml"
DEFAULT_DEVICES_PATH = BASE_DIR / "devices.jsonl"

FONT_CANDIDATES = {
    "regular": (
        "/usr/share/fonts/truetype/lato/Lato-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    ),
    "medium": (
        "/usr/share/fonts/truetype/lato/Lato-Medium.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Medium.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    ),
    "semibold": (
        "/usr/share/fonts/truetype/lato/Lato-Semibold.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-SemiBold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
    ),
    "bold": (
        "/usr/share/fonts/truetype/lato/Lato-Bold.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
    ),
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


def load_font(kind: str, size: int) -> ImageFont.ImageFont | ImageFont.FreeTypeFont:
    for candidate in FONT_CANDIDATES[kind]:
        path = Path(candidate)
        if path.exists():
            return ImageFont.truetype(str(path), size)
    return ImageFont.load_default()


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


def load_devices(path: Path) -> dict[str, str]:
    devices: dict[str, str] = {}
    if not path.exists():
        return devices

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            if not line.strip():
                continue
            record = json.loads(line)
            description = str(record["description"])
            token_hash = str(record["token_hash"])
            devices[token_hash] = description
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
    start_of_today = datetime.combine(now.date(), time.min, DISPLAY_TIMEZONE)
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

    plan_date_text = page.label

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


class ClockRequestHandler(BaseHTTPRequestHandler):
    server_version = "MemoryClockHTTP/1.0"

    @property
    def app(self) -> "ClockServer":
        return self.server  # type: ignore[return-value]

    def authenticated_device(self) -> str | None:
        token = bearer_token(self.headers)
        if token is None:
            self.send_response(HTTPStatus.UNAUTHORIZED)
            self.send_header("WWW-Authenticate", "Bearer")
            self.end_headers()
            return None

        token_hash = hash_token(token)
        devices = load_devices(self.app.devices_path)
        device_description = devices.get(token_hash)
        if device_description is None:
            self.send_error(HTTPStatus.FORBIDDEN, "unknown device token")
            return None
        return device_description

    def client_version(self) -> str:
        value = self.headers.get(CLIENT_VERSION_HEADER, "").strip()
        return value or "unknown"

    def do_GET(self) -> None:
        parsed_url = urlparse(self.path)
        if parsed_url.path.startswith(IMAGE_PATH_PREFIX):
            self.handle_image_request(parsed_url.path)
            return

        if parsed_url.path != BASE_PATH:
            self.send_error(HTTPStatus.NOT_FOUND, "not found")
            return

        device_description = self.authenticated_device()
        if device_description is None:
            return

        last_modified = self.app.effective_last_modified()
        if_modified_since = httpdate_to_timestamp(self.headers.get("If-Modified-Since", ""))
        if if_modified_since is not None and last_modified <= if_modified_since:
            self.send_response(HTTPStatus.NOT_MODIFIED)
            self.send_header("Last-Modified", formatdate(last_modified, usegmt=True))
            self.end_headers()
            print(f"device={device_description} client={self.client_version()} GET {BASE_PATH} 304",
                  flush=True)
            return

        payload = build_payload(self.app.calendar_path)
        payload["device"] = device_description
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")

        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Last-Modified", formatdate(last_modified, usegmt=True))
        self.end_headers()
        self.wfile.write(body)
        print(f"device={device_description} client={self.client_version()} GET {BASE_PATH} 200",
              flush=True)

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
    def __init__(self, server_address: tuple[str, int], calendar_path: Path,
                 devices_path: Path) -> None:
        super().__init__(server_address, ClockRequestHandler)
        self.calendar_path = calendar_path
        self.devices_path = devices_path
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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Serve calendar pages for Memory Clock.")
    parser.add_argument("--host", default="127.0.0.1", help="bind host, default: 127.0.0.1")
    parser.add_argument("--port", default=8000, type=int, help="bind port, default: 8000")
    parser.add_argument("--calendar", type=Path, default=DEFAULT_CALENDAR_PATH,
                        help=f"calendar YAML path, default: {DEFAULT_CALENDAR_PATH}")
    parser.add_argument("--devices", type=Path, default=DEFAULT_DEVICES_PATH,
                        help=f"device token file, default: {DEFAULT_DEVICES_PATH}")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    server = ClockServer((args.host, args.port), args.calendar.resolve(), args.devices.resolve())
    print(f"Serving {BASE_PATH} on http://{args.host}:{args.port}{BASE_PATH}", flush=True)
    print(f"Calendar: {server.calendar_path}", flush=True)
    print(f"Devices: {server.devices_path}", flush=True)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
