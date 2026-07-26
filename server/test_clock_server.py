from __future__ import annotations

import http.client
import json
import sqlite3
import stat
import subprocess
import sys
import tempfile
import threading
import unittest
from datetime import date, timedelta
from pathlib import Path

import clock_server


class ClockServerIntegrationTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.data_path = Path(self.temporary_directory.name)
        self.device_token = "mc_test-device-token"
        self.admin_token = "ma_test-admin-token"
        self.device_id = "test-clock"

        self.calendar_path = self.data_path / "calendar.yaml"
        tomorrow = date.today() + timedelta(days=1)
        self.calendar_path.write_text(
            f"- date: {tomorrow.isoformat()}\n"
            "  plan: Test plan\n"
            "  appointments:\n"
            "    - time: '09:30'\n"
            "      title: Test appointment\n"
            "      location: Test room\n",
            encoding="utf-8",
        )
        self.devices_path = self.data_path / "devices.jsonl"
        self.devices_path.write_text(
            json.dumps({
                "id": self.device_id,
                "description": "Test clock",
                "token_hash": clock_server.hash_token(self.device_token),
            }) + "\n",
            encoding="utf-8",
        )
        self.alerts_path = self.data_path / "alerts.yaml"
        self.alerts_path.write_text(
            clock_server.EXAMPLE_ALERTS_PATH.read_text(encoding="utf-8"),
            encoding="utf-8",
        )
        self.state_path = self.data_path / "state.sqlite3"
        self.server = clock_server.ClockServer(
            ("127.0.0.1", 0),
            self.calendar_path,
            self.devices_path,
            self.alerts_path,
            self.state_path,
            clock_server.hash_token(self.admin_token),
            clock_server.DEFAULT_ADMIN_ASSETS_PATH,
            secure_admin_cookie=False,
        )
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_address[1]

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.temporary_directory.cleanup()

    def request(self, method: str, path: str, *, headers: dict[str, str] | None = None,
                body: bytes | None = None) -> tuple[int, dict[str, str], bytes]:
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        result = (
            response.status,
            {name.lower(): value for name, value in response.getheaders()},
            response.read(),
        )
        connection.close()
        return result

    def test_clock_status_and_admin_views(self) -> None:
        status, _, _ = self.request("GET", "/memory-clock/admin/api/clients")
        self.assertEqual(status, 401)

        device_headers = {
            "Authorization": f"Bearer {self.device_token}",
            "X-Memory-Clock-Version": "test-version",
            "X-Memory-Clock-Battery-Mv": "4128",
            "X-Memory-Clock-Wifi-Rssi": "-49",
            "X-Memory-Clock-Uptime-S": "9281",
            "X-Memory-Clock-Last-Interaction-S": "184",
        }
        status, headers, _ = self.request("GET", "/memory-clock", headers=device_headers)
        self.assertEqual(status, 200)

        device_headers["If-Modified-Since"] = headers["last-modified"]
        device_headers["X-Memory-Clock-Battery-Mv"] = "4099"
        status, _, body = self.request("GET", "/memory-clock", headers=device_headers)
        self.assertEqual(status, 304)
        self.assertEqual(body, b"")

        admin_headers = {"Authorization": f"Bearer {self.admin_token}"}
        status, _, body = self.request(
            "GET", "/memory-clock/admin/api/clients", headers=admin_headers
        )
        self.assertEqual(status, 200)
        payload = json.loads(body)
        self.assertEqual(
            payload["alerts"],
            ["Beep", "Shave and a Haircut", "La Cucaracha"],
        )
        self.assertEqual(len(payload["clients"]), 1)
        client = payload["clients"][0]
        self.assertEqual(client["id"], self.device_id)
        self.assertEqual(client["battery_mv"], 4099)
        self.assertEqual(client["wifi_rssi"], -49)
        self.assertEqual(client["client_version"], "test-version")

        status, _, body = self.request(
            "POST",
            "/memory-clock/admin/api/login",
            headers={"Content-Type": "application/json"},
            body=json.dumps({"token": self.admin_token}).encode("utf-8"),
        )
        self.assertEqual(status, 200)

    def test_authenticated_pages_calendar_and_login_cookie(self) -> None:
        login_body = json.dumps({"token": self.admin_token}).encode("utf-8")
        status, headers, _ = self.request(
            "POST",
            "/memory-clock/admin/api/login",
            headers={"Content-Type": "application/json", "Content-Length": str(len(login_body))},
            body=login_body,
        )
        self.assertEqual(status, 200)
        cookie = headers["set-cookie"].split(";", 1)[0]
        cookie_headers = {"Cookie": cookie}

        status, _, body = self.request(
            "GET", "/memory-clock/admin/api/pages", headers=cookie_headers
        )
        self.assertEqual(status, 200)
        self.assertEqual(len(json.loads(body)["pages"]), 1)

        status, headers, body = self.request(
            "GET", "/memory-clock/admin/api/pages/1.png", headers=cookie_headers
        )
        self.assertEqual(status, 200)
        self.assertEqual(headers["content-type"], "image/png")
        self.assertTrue(body.startswith(b"\x89PNG\r\n\x1a\n"))

        status, headers, body = self.request(
            "GET", "/memory-clock/admin/api/calendar", headers=cookie_headers
        )
        self.assertEqual(status, 200)
        self.assertEqual(headers["content-type"], "text/yaml; charset=utf-8")
        self.assertIn(b"Test appointment", body)

        status, headers, body = self.request("GET", "/memory-clock/admin/")
        self.assertEqual(status, 200)
        self.assertIn("default-src 'none'", headers["content-security-policy"])
        self.assertIn(b"Choose auth file", body)

    def test_message_lifecycle(self) -> None:
        device_headers = {
            "Authorization": f"Bearer {self.device_token}",
            "X-Memory-Clock-Message-Capable": "1",
        }
        status, headers, body = self.request(
            "GET", "/memory-clock", headers=device_headers
        )
        self.assertEqual(status, 200)
        self.assertIsNone(json.loads(body)["message"])
        last_modified = headers["last-modified"]

        admin_headers = {
            "Authorization": f"Bearer {self.admin_token}",
            "Content-Type": "application/json",
            "X-Memory-Clock-CSRF": "1",
        }
        message_body = json.dumps({
            "device_id": self.device_id,
            "text": "Dinner is ready!",
            "alert": "Shave and a Haircut",
        }).encode("utf-8")
        status, _, body = self.request(
            "POST",
            "/memory-clock/admin/api/messages",
            headers=admin_headers,
            body=message_body,
        )
        self.assertEqual(status, 201)
        queued = json.loads(body)["message"]
        self.assertEqual(queued["state"], "queued")
        self.assertEqual(queued["text"], "Dinner is ready!")
        self.assertEqual(queued["alert"], "Shave and a Haircut")
        message_id = queued["id"]

        delivery_headers = {
            **device_headers,
            "If-Modified-Since": last_modified,
        }
        status, headers, body = self.request(
            "GET", "/memory-clock", headers=delivery_headers
        )
        self.assertEqual(status, 200)
        delivered = json.loads(body)["message"]
        self.assertEqual(delivered["id"], message_id)
        self.assertEqual(delivered["text"], "Dinner is ready!")
        self.assertEqual(delivered["alert"]["tones"], [
            {"frequency_hz": 1047, "duration_ms": 283, "gap_ms": 50},
            {"frequency_hz": 784, "duration_ms": 142, "gap_ms": 25},
            {"frequency_hz": 784, "duration_ms": 142, "gap_ms": 25},
            {"frequency_hz": 880, "duration_ms": 283, "gap_ms": 50},
            {"frequency_hz": 784, "duration_ms": 333, "gap_ms": 0},
        ])

        displayed_headers = {
            **device_headers,
            "If-Modified-Since": headers["last-modified"],
            "X-Memory-Clock-Message-Active": message_id,
            "X-Memory-Clock-Message-Displayed": message_id,
        }
        status, _, body = self.request(
            "GET", "/memory-clock", headers=displayed_headers
        )
        self.assertEqual(status, 304)
        self.assertEqual(body, b"")

        status, _, body = self.request(
            "GET", "/memory-clock/admin/api/clients",
            headers={"Authorization": f"Bearer {self.admin_token}"},
        )
        self.assertEqual(status, 200)
        message = json.loads(body)["clients"][0]["message"]
        self.assertEqual(message["state"], "displayed")
        self.assertIsNotNone(message["displayed_at"])

        dismissed_headers = {
            **device_headers,
            "If-Modified-Since": last_modified,
            "X-Memory-Clock-Message-Dismissed": message_id,
        }
        status, _, body = self.request(
            "GET", "/memory-clock", headers=dismissed_headers
        )
        self.assertEqual(status, 304)
        self.assertEqual(body, b"")

        status, _, body = self.request(
            "GET", "/memory-clock/admin/api/clients",
            headers={"Authorization": f"Bearer {self.admin_token}"},
        )
        dismissed = json.loads(body)["clients"][0]["message"]
        self.assertEqual(dismissed["state"], "dismissed")
        self.assertIsNone(dismissed["text"])
        self.assertIsNone(dismissed["alert"])
        self.assertIsNotNone(dismissed["dismissed_at"])

    def test_queued_message_does_not_refresh_older_firmware(self) -> None:
        device_headers = {"Authorization": f"Bearer {self.device_token}"}
        status, headers, body = self.request(
            "GET", "/memory-clock", headers=device_headers
        )
        self.assertEqual(status, 200)
        self.assertIsNone(json.loads(body)["message"])

        message_body = json.dumps({
            "device_id": self.device_id,
            "text": "This clock needs newer firmware.",
        }).encode("utf-8")
        status, _, _ = self.request(
            "POST",
            "/memory-clock/admin/api/messages",
            headers={
                "Authorization": f"Bearer {self.admin_token}",
                "Content-Type": "application/json",
                "X-Memory-Clock-CSRF": "1",
            },
            body=message_body,
        )
        self.assertEqual(status, 201)

        device_headers["If-Modified-Since"] = headers["last-modified"]
        for _ in range(2):
            status, _, body = self.request(
                "GET", "/memory-clock", headers=device_headers
            )
            self.assertEqual(status, 304)
            self.assertEqual(body, b"")

    def test_admin_removal_cancels_displayed_message(self) -> None:
        device_headers = {
            "Authorization": f"Bearer {self.device_token}",
            "X-Memory-Clock-Message-Capable": "1",
        }
        status, headers, _ = self.request(
            "GET", "/memory-clock", headers=device_headers
        )
        self.assertEqual(status, 200)
        last_modified = headers["last-modified"]

        message_body = json.dumps({
            "device_id": self.device_id,
            "text": "Temporary message",
        }).encode("utf-8")
        admin_headers = {
            "Authorization": f"Bearer {self.admin_token}",
            "Content-Type": "application/json",
            "X-Memory-Clock-CSRF": "1",
        }
        status, _, body = self.request(
            "POST",
            "/memory-clock/admin/api/messages",
            headers=admin_headers,
            body=message_body,
        )
        self.assertEqual(status, 201)
        queued = json.loads(body)["message"]
        self.assertIsNone(queued["alert"])
        message_id = queued["id"]

        status, _, body = self.request(
            "GET",
            "/memory-clock",
            headers={
                **device_headers,
                "If-Modified-Since": last_modified,
            },
        )
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["message"]["id"], message_id)

        status, _, body = self.request(
            "DELETE",
            f"/memory-clock/admin/api/messages/{self.device_id}",
            headers={
                "Authorization": f"Bearer {self.admin_token}",
                "X-Memory-Clock-CSRF": "1",
            },
        )
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(body)["removed"])

        status, _, body = self.request(
            "GET",
            "/memory-clock",
            headers={
                **device_headers,
                "If-Modified-Since": last_modified,
                "X-Memory-Clock-Message-Active": message_id,
                "X-Memory-Clock-Message-Displayed": message_id,
            },
        )
        self.assertEqual(status, 200)
        self.assertIsNone(json.loads(body)["message"])

        status, _, body = self.request(
            "GET",
            "/memory-clock",
            headers={
                **device_headers,
                "If-Modified-Since": last_modified,
            },
        )
        self.assertEqual(status, 304)
        self.assertEqual(body, b"")

    def test_message_validation_and_csrf(self) -> None:
        body = json.dumps({
            "device_id": self.device_id,
            "text": "Test message",
        }).encode("utf-8")
        status, _, _ = self.request(
            "POST",
            "/memory-clock/admin/api/messages",
            headers={
                "Authorization": f"Bearer {self.admin_token}",
                "Content-Type": "application/json",
            },
            body=body,
        )
        self.assertEqual(status, 403)

        body = json.dumps({
            "device_id": self.device_id,
            "text": "Test message",
            "alert": "Not configured",
        }).encode("utf-8")
        status, _, response = self.request(
            "POST",
            "/memory-clock/admin/api/messages",
            headers={
                "Authorization": f"Bearer {self.admin_token}",
                "Content-Type": "application/json",
                "X-Memory-Clock-CSRF": "1",
            },
            body=body,
        )
        self.assertEqual(status, 400)
        self.assertIn(b"unknown alert", response)

        body = json.dumps({
            "device_id": self.device_id,
            "text": "Unsupported: \N{RIGHT SINGLE QUOTATION MARK}",
        }).encode("utf-8")
        status, _, response = self.request(
            "POST",
            "/memory-clock/admin/api/messages",
            headers={
                "Authorization": f"Bearer {self.admin_token}",
                "Content-Type": "application/json",
                "X-Memory-Clock-CSRF": "1",
            },
            body=body,
        )
        self.assertEqual(status, 400)
        self.assertIn(b"cannot display", response)

    def test_invalid_alert_catalog_does_not_hide_clocks_or_block_silent_message(
            self) -> None:
        self.alerts_path.write_text("alerts: invalid\n", encoding="utf-8")
        admin_headers = {"Authorization": f"Bearer {self.admin_token}"}

        status, _, body = self.request(
            "GET", "/memory-clock/admin/api/clients", headers=admin_headers
        )
        self.assertEqual(status, 200)
        payload = json.loads(body)
        self.assertEqual(len(payload["clients"]), 1)
        self.assertEqual(payload["alerts"], [])
        self.assertEqual(payload["alerts_error"], "Alert sounds are unavailable.")

        message_body = json.dumps({
            "device_id": self.device_id,
            "text": "This silent message still works.",
            "alert": None,
        }).encode("utf-8")
        status, _, body = self.request(
            "POST",
            "/memory-clock/admin/api/messages",
            headers={
                **admin_headers,
                "Content-Type": "application/json",
                "X-Memory-Clock-CSRF": "1",
            },
            body=message_body,
        )
        self.assertEqual(status, 201)
        self.assertEqual(
            json.loads(body)["message"]["text"],
            "This silent message still works.",
        )

        requested_alert_body = json.dumps({
            "device_id": self.device_id,
            "text": "This requests unavailable sound.",
            "alert": "Beep",
        }).encode("utf-8")
        status, _, body = self.request(
            "POST",
            "/memory-clock/admin/api/messages",
            headers={
                **admin_headers,
                "Content-Type": "application/json",
                "X-Memory-Clock-CSRF": "1",
            },
            body=requested_alert_body,
        )
        self.assertEqual(status, 503)
        self.assertIn(b"alert sounds are unavailable", body)

    def test_invalid_stored_alert_does_not_hide_message(self) -> None:
        alert = clock_server.load_alerts(self.alerts_path)[0]
        queued = self.server.state_store.queue_message(
            self.device_id, "The message is still shown.", alert
        )
        with sqlite3.connect(self.state_path) as connection:
            connection.execute(
                """
                UPDATE device_messages
                SET alert_tones = ?
                WHERE device_id = ?
                """,
                ("not-json", self.device_id),
            )

        status, _, body = self.request(
            "GET",
            "/memory-clock",
            headers={
                "Authorization": f"Bearer {self.device_token}",
                "X-Memory-Clock-Message-Capable": "1",
            },
        )
        self.assertEqual(status, 200)
        message = json.loads(body)["message"]
        self.assertEqual(message["id"], queued["message_id"])
        self.assertEqual(message["text"], "The message is still shown.")
        self.assertIsNone(message["alert"])

    def test_invalid_login_limit_does_not_lock_out_valid_token(self) -> None:
        for _ in range(clock_server.ADMIN_LOGIN_ATTEMPTS + 1):
            body = json.dumps({"token": "ma_wrong-admin-token"}).encode("utf-8")
            status, _, _ = self.request(
                "POST",
                "/memory-clock/admin/api/login",
                headers={"Content-Type": "application/json"},
                body=body,
            )
        self.assertEqual(status, 429)

        body = json.dumps({"token": self.admin_token}).encode("utf-8")
        status, _, _ = self.request(
            "POST",
            "/memory-clock/admin/api/login",
            headers={"Content-Type": "application/json"},
            body=body,
        )
        self.assertEqual(status, 200)


class AdminAuthToolTest(unittest.TestCase):
    def test_creates_distinct_private_token_and_hash_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary_path = Path(temporary_directory)
            token_path = temporary_path / "admin.token"
            hash_path = temporary_path / "admin-token.sha256"
            command = [
                sys.executable,
                str(Path(__file__).with_name("create-admin-auth.py")),
                "--token-file",
                str(token_path),
                "--hash-file",
                str(hash_path),
            ]
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            token = token_path.read_text(encoding="ascii").strip()
            token_hash = hash_path.read_text(encoding="ascii").strip()
            self.assertTrue(token.startswith("ma_"))
            self.assertEqual(clock_server.hash_token(token), token_hash)
            self.assertEqual(stat.S_IMODE(token_path.stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(hash_path.stat().st_mode), 0o600)

            second_result = subprocess.run(
                command, capture_output=True, text=True, check=False
            )
            self.assertEqual(second_result.returncode, 1)


class DeviceConfigurationTest(unittest.TestCase):
    def test_legacy_record_gets_stable_fallback_id(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "devices.jsonl"
            token_hash = clock_server.hash_token("mc_legacy-token")
            path.write_text(
                json.dumps({
                    "description": "Legacy clock",
                    "token_hash": token_hash,
                }) + "\n",
                encoding="utf-8",
            )
            first = clock_server.load_devices(path)[token_hash]
            second = clock_server.load_devices(path)[token_hash]
            self.assertEqual(first.device_id, second.device_id)
            self.assertTrue(first.device_id.startswith("legacy-"))


class AlertConfigurationTest(unittest.TestCase):
    def test_default_alerts_are_valid(self) -> None:
        alerts = clock_server.load_alerts(clock_server.EXAMPLE_ALERTS_PATH)
        self.assertEqual(
            [alert.name for alert in alerts],
            ["Beep", "Shave and a Haircut", "La Cucaracha"],
        )
        self.assertEqual(
            sum(tone.duration_ms + tone.gap_ms for tone in alerts[1].tones),
            1333,
        )
        self.assertEqual(
            [tone.frequency_hz for tone in alerts[2].tones],
            [523, 523, 523, 698, 880, 523, 523, 523, 698, 880],
        )
        self.assertEqual(
            sum(tone.duration_ms + tone.gap_ms for tone in alerts[2].tones),
            3600,
        )

    def test_rejects_out_of_range_frequency(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "alerts.yaml"
            path.write_text(
                "alerts:\n"
                "  - name: Bad\n"
                "    tones:\n"
                "      - frequency_hz: 499\n"
                "        duration_ms: 100\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "frequency_hz"):
                clock_server.load_alerts(path)


if __name__ == "__main__":
    unittest.main()
