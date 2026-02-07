#!/usr/bin/env python3
"""Mock Telegram Bot API server for integration testing.

Serves common Bot API endpoints and supports configurable failure scenarios.
Uses only Python 3 stdlib — no pip dependencies.

Usage:
    python3 mock_tg_server.py [OPTIONS]

The server prints the listening port to stdout on startup.
"""

import argparse
import json
import sys
import time
import random
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler

# Default bot token for matching URL paths
TOKEN = "TESTTOKEN123"


class MockHandler(BaseHTTPRequestHandler):
    """Handle incoming requests mimicking Telegram Bot API."""

    server_version = "MockTgServer/1.0"

    def log_message(self, fmt, *args):
        """Suppress default logging to stderr unless verbose."""
        if self.server.verbose:
            super().log_message(fmt, *args)

    def _should_fail(self):
        """Check if this request should be a simulated failure."""
        if self.server.fail_rate > 0 and random.random() < self.server.fail_rate:
            return True
        return False

    def _apply_delay(self):
        """Apply configured delay before responding."""
        if self.server.delay_ms > 0:
            time.sleep(self.server.delay_ms / 1000.0)

    def _send_json(self, status_code, data, headers=None):
        """Send a JSON response."""
        body = json.dumps(data).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if headers:
            for k, v in headers.items():
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _ok_response(self, result):
        return {"ok": True, "result": result}

    def _error_response(self, code, desc):
        return {"ok": False, "error_code": code, "description": desc}

    def _extract_method(self):
        """Extract the API method from the URL path."""
        # Expected: /bot<TOKEN>/<method>
        prefix = f"/bot{TOKEN}/"
        if self.path.startswith(prefix):
            method = self.path[len(prefix):]
            # Strip query string
            if "?" in method:
                method = method.split("?", 1)[0]
            return method
        return None

    def do_GET(self):
        self._handle_request()

    def do_POST(self):
        self._handle_request()

    def _read_body(self):
        length = int(self.headers.get("Content-Length", 0))
        if length > 0:
            return self.rfile.read(length)
        return b""

    def _handle_request(self):
        self._apply_delay()

        method = self._extract_method()
        if method is None:
            self._send_json(404, self._error_response(404, "Not Found"))
            return

        # Check scenario-specific overrides
        scenario = self.server.scenario

        if scenario == "429-retry":
            if not hasattr(self.server, "_retry_sent"):
                self.server._retry_sent = True
                self._send_json(
                    429,
                    self._error_response(429, "Too Many Requests: retry after 1"),
                    headers={"Retry-After": "1"},
                )
                return

        if scenario == "401-unauthorized" and method == "getUpdates":
            self._send_json(401, self._error_response(401, "Unauthorized"))
            return

        if self.server.status_code_override:
            self._send_json(
                self.server.status_code_override,
                self._error_response(
                    self.server.status_code_override, "Forced error"
                ),
            )
            return

        if self._should_fail():
            self._send_json(500, self._error_response(500, "Internal Server Error"))
            return

        # Route to method handlers
        body = self._read_body()

        if method == "getMe":
            self._handle_get_me()
        elif method == "getUpdates":
            self._handle_get_updates()
        elif method == "sendMessage":
            self._handle_send_message(body)
        elif method == "setWebhook":
            self._handle_set_webhook(body)
        elif method == "deleteWebhook":
            self._handle_delete_webhook()
        else:
            self._send_json(404, self._error_response(404, f"Method not found: {method}"))

    def _handle_get_me(self):
        result = {
            "id": 123456789,
            "is_bot": True,
            "first_name": "TestBot",
            "username": "test_bot",
        }
        self._send_json(200, self._ok_response(result))

    def _handle_get_updates(self):
        scenario = self.server.scenario

        if scenario == "duplicate-updates":
            updates = [
                {
                    "update_id": 100,
                    "message": {
                        "message_id": 1,
                        "from": {"id": 42, "is_bot": False, "first_name": "Test"},
                        "chat": {"id": 42, "type": "private"},
                        "text": "dup1",
                    },
                },
                {
                    "update_id": 100,
                    "message": {
                        "message_id": 2,
                        "from": {"id": 42, "is_bot": False, "first_name": "Test"},
                        "chat": {"id": 42, "type": "private"},
                        "text": "dup2",
                    },
                },
            ]
            self._send_json(200, self._ok_response(updates))
            return

        if scenario == "out-of-order":
            updates = [
                {
                    "update_id": 5,
                    "message": {
                        "message_id": 5,
                        "from": {"id": 42, "is_bot": False, "first_name": "Test"},
                        "chat": {"id": 42, "type": "private"},
                        "text": "five",
                    },
                },
                {
                    "update_id": 3,
                    "message": {
                        "message_id": 3,
                        "from": {"id": 42, "is_bot": False, "first_name": "Test"},
                        "chat": {"id": 42, "type": "private"},
                        "text": "three",
                    },
                },
                {
                    "update_id": 7,
                    "message": {
                        "message_id": 7,
                        "from": {"id": 42, "is_bot": False, "first_name": "Test"},
                        "chat": {"id": 42, "type": "private"},
                        "text": "seven",
                    },
                },
            ]
            self._send_json(200, self._ok_response(updates))
            return

        if scenario == "partial-read":
            # Send partial JSON and close connection
            partial = b'{"ok": true, "res'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", "100")
            self.end_headers()
            self.wfile.write(partial)
            self.wfile.flush()
            return

        if scenario == "slow-response":
            time.sleep(35)  # exceed typical timeout

        # Default: empty update list
        self._send_json(200, self._ok_response([]))

    def _handle_send_message(self, body):
        try:
            data = json.loads(body) if body else {}
        except json.JSONDecodeError:
            data = {}

        result = {
            "message_id": random.randint(1, 999999),
            "from": {
                "id": 123456789,
                "is_bot": True,
                "first_name": "TestBot",
                "username": "test_bot",
            },
            "chat": {
                "id": data.get("chat_id", 0),
                "type": "private",
            },
            "text": data.get("text", ""),
        }
        self._send_json(200, self._ok_response(result))

    def _handle_set_webhook(self, body):
        self._send_json(
            200,
            self._ok_response(True),
        )

    def _handle_delete_webhook(self):
        self._send_json(
            200,
            self._ok_response(True),
        )


class MockTgServer(HTTPServer):
    """HTTPServer with configurable test parameters."""

    allow_reuse_address = True

    def __init__(self, port=0, **kwargs):
        self.fail_rate = kwargs.get("fail_rate", 0.0)
        self.delay_ms = kwargs.get("delay_ms", 0)
        self.status_code_override = kwargs.get("status_code", None)
        self.scenario = kwargs.get("scenario", None)
        self.verbose = kwargs.get("verbose", False)
        super().__init__(("127.0.0.1", port), MockHandler)


def main():
    parser = argparse.ArgumentParser(description="Mock Telegram Bot API server")
    parser.add_argument("--port", type=int, default=0, help="Port to listen on (0 = random)")
    parser.add_argument(
        "--fail-rate",
        type=float,
        default=0.0,
        help="Probability of returning 500 (0.0-1.0)",
    )
    parser.add_argument("--delay-ms", type=int, default=0, help="Response delay in ms")
    parser.add_argument("--status-code", type=int, default=None, help="Force this HTTP status code")
    parser.add_argument(
        "--scenario",
        type=str,
        default=None,
        choices=[
            "duplicate-updates",
            "out-of-order",
            "429-retry",
            "partial-read",
            "slow-response",
            "401-unauthorized",
        ],
        help="Test scenario to activate",
    )
    parser.add_argument("--verbose", action="store_true", help="Enable request logging")
    args = parser.parse_args()

    server = MockTgServer(
        port=args.port,
        fail_rate=args.fail_rate,
        delay_ms=args.delay_ms,
        status_code=args.status_code,
        scenario=args.scenario,
        verbose=args.verbose,
    )

    port = server.server_address[1]
    # Print port to stdout for test harness consumption
    print(port, flush=True)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
