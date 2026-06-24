from datetime import datetime
from http.server import BaseHTTPRequestHandler, HTTPServer
from io import BytesIO
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


HOST = "127.0.0.1"
PORT = 8000
WIDTH = 800
HEIGHT = 480


def load_font(size: int) -> ImageFont.ImageFont:
    candidates = [
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    ]
    for candidate in candidates:
        if Path(candidate).exists():
            return ImageFont.truetype(candidate, size=size)
    return ImageFont.load_default()


def centered_text(draw: ImageDraw.ImageDraw, text: str, font: ImageFont.ImageFont, y: int) -> None:
    left, top, right, bottom = draw.textbbox((0, 0), text, font=font)
    width = right - left
    height = bottom - top
    x = (WIDTH - width) // 2
    draw.text((x, y - (height // 2)), text, fill=0, font=font)


def text_size(draw: ImageDraw.ImageDraw, text: str, font: ImageFont.ImageFont) -> tuple[int, int]:
    left, top, right, bottom = draw.textbbox((0, 0), text, font=font)
    return right - left, bottom - top


def draw_centered_at(
    draw: ImageDraw.ImageDraw,
    text: str,
    font: ImageFont.ImageFont,
    center_x: int,
    center_y: int,
) -> None:
    width, height = text_size(draw, text, font)
    draw.text((center_x - (width // 2), center_y - (height // 2)), text, fill=0, font=font)


def part_of_day(hour: int) -> str:
    if hour < 5:
        return "Night"
    if hour < 12:
        return "Morning"
    if hour < 17:
        return "Afternoon"
    if hour < 21:
        return "Evening"
    return "Night"


def render_screen() -> bytes:
    image = Image.new("1", (WIDTH, HEIGHT), 1)
    draw = ImageDraw.Draw(image)
    now = datetime.now()

    weekday_text = now.strftime("%A")
    period_text = part_of_day(now.hour)
    time_main = now.strftime("%I:%M").lstrip("0")
    am_pm_text = "A.M." if now.strftime("%p") == "AM" else "P.M."
    month_text = now.strftime("%B")
    day_text = f"{now.day},"
    year_text = str(now.year)

    weekday_font = load_font(54)
    period_font = load_font(38)
    time_font = load_font(145)
    am_pm_font = load_font(50)
    date_font = load_font(34)

    centered_text(draw, weekday_text, weekday_font, 72)
    centered_text(draw, period_text, period_font, 132)

    time_width, time_height = text_size(draw, time_main, time_font)
    am_pm_width, am_pm_height = text_size(draw, am_pm_text, am_pm_font)
    time_block_width = time_width + 22 + am_pm_width
    time_left = (WIDTH - time_block_width) // 2
    time_top = 170

    draw.text((time_left, time_top), time_main, fill=0, font=time_font)
    draw.text(
        (time_left + time_width + 22, time_top + time_height - am_pm_height - 14),
        am_pm_text,
        fill=0,
        font=am_pm_font,
    )

    draw_centered_at(draw, month_text, date_font, 180, 400)
    draw_centered_at(draw, day_text, date_font, 400, 400)
    draw_centered_at(draw, year_text, date_font, 630, 400)

    payload = BytesIO()
    image.save(payload, format="BMP")
    return payload.getvalue()


class ScreenHandler(BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        if self.path != "/screen.bmp":
            self.send_error(404)
            return

        payload = render_screen()
        self.send_response(200)
        self.send_header("Content-Type", "image/bmp")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, format: str, *args) -> None:  # noqa: A003
        return


def main() -> None:
    server = HTTPServer((HOST, PORT), ScreenHandler)
    print(f"Serving local mode BMP on http://{HOST}:{PORT}/screen.bmp")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
