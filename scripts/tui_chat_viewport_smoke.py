#!/usr/bin/env python3
"""Repeatable pseudo-terminal smoke checks for ACECode ChatViewport.

This script is intentionally a smoke harness, not a replacement for the final
manual Windows Terminal/ConHost checklist. It runs acecode in an isolated
USERPROFILE with a generated resume session, drives a few terminal events via
pywinpty, and verifies the ChatViewport trace contains sane row/box updates.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
import tempfile
import threading
import time
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Callable, Any

try:
    from winpty import PtyProcess
except Exception as exc:  # pragma: no cover - environment guard
    PtyProcess = None  # type: ignore[assignment]
    WINPTY_IMPORT_ERROR = exc
else:
    WINPTY_IMPORT_ERROR = None


ANSI_RE = re.compile(
    r"\x1b\][^\x1b]*(?:\x07|\x1b\\)|\x1b\[[0-?]*[ -/]*[@-~]|\x1b[()][A-Za-z0-9]"
)
TRACE_RE = re.compile(
    r"\[chat-viewport\].*?top=(?P<top>\d+).*?"
    r"viewport_rows=(?P<viewport>\d+).*?"
    r"total_rows=(?P<total>\d+).*?"
    r"chat_box=(?P<chat>[-\d,]+).*?"
    r"scrollbar_box=(?P<scrollbar>[-\d,]+)"
)
FTXUI_MODE_RE = re.compile(
    r"alt_screen=(?P<alt>[01]) terminal_output=(?P<terminal>[01])"
)
TUI_MODE_RE = re.compile(
    r"\[tui\] render_mode=(?P<mode>\w+) "
    r"conhost_compat_layout=(?P<compat>[01]) "
    r"terminal_source=(?P<source>.*)"
)
SCROLL_TRACE_RE = re.compile(
    r"\[chat-scroll\].*?path=(?P<path>\w+).*?"
    r"button=(?P<button>\w+).*?"
    r"actual=(?P<actual>-?\d+).*?"
    r"top=(?P<before>\d+)->(?P<after>\d+)"
)
ASK_TOOL_CALL_ID = "call_ask_smoke"
ASK_FINAL_TEXT = "Ask smoke complete."
ASK_TOOL_ARGUMENTS = {
    "questions": [
        {
            "question": "Choose the smoke path?",
            "header": "Smoke",
            "options": [
                {
                    "label": "Proceed",
                    "description": "Continue the smoke test.",
                },
                {
                    "label": "Stop",
                    "description": "Stop the smoke test.",
                },
            ],
        }
    ]
}


@dataclass(frozen=True)
class TraceEntry:
    top: int
    viewport_rows: int
    total_rows: int
    chat_box: tuple[int, int, int, int]
    scrollbar_box: tuple[int, int, int, int]


@dataclass(frozen=True)
class ScrollTraceEntry:
    path: str
    button: str
    actual: int
    before: int
    after: int


class PtyRunner:
    def __init__(
        self,
        argv: list[str],
        cwd: Path,
        env: dict[str, str],
        dimensions: tuple[int, int],
        verbose: bool = False,
    ) -> None:
        if PtyProcess is None:
            raise RuntimeError(f"pywinpty is unavailable: {WINPTY_IMPORT_ERROR!r}")
        self.proc = PtyProcess.spawn(
            argv,
            cwd=str(cwd),
            env=env,
            dimensions=dimensions,
        )
        self.verbose = verbose
        self._buf: list[str] = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def _read_loop(self) -> None:
        while not self._stop.is_set():
            try:
                chunk = self.proc.read(4096)
            except Exception:
                break
            if not chunk:
                if not self.proc.isalive():
                    break
                time.sleep(0.02)
                continue
            if self.verbose:
                encoding = sys.stdout.encoding or "utf-8"
                sys.stdout.buffer.write(chunk.encode(encoding, errors="replace"))
                sys.stdout.buffer.flush()
            with self._lock:
                self._buf.append(chunk)

    def write(self, text: str) -> None:
        self.proc.write(text)

    def resize(self, rows: int, cols: int) -> None:
        self.proc.setwinsize(rows, cols)

    def output(self) -> str:
        with self._lock:
            return "".join(self._buf)

    def clean_output(self) -> str:
        return strip_ansi(self.output())

    def wait_until(self, pred: Callable[[], bool], timeout: float, label: str) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if pred():
                return
            if not self.proc.isalive():
                break
            time.sleep(0.05)
        raise RuntimeError(f"timed out waiting for {label}")

    def close(self) -> None:
        try:
            if self.proc.isalive():
                try:
                    self.write("\x03")
                except EOFError:
                    return
                time.sleep(0.2)
                try:
                    self.write("y")
                except EOFError:
                    return
                deadline = time.monotonic() + 3.0
                while time.monotonic() < deadline and self.proc.isalive():
                    time.sleep(0.05)
            if self.proc.isalive():
                self.proc.terminate()
        finally:
            self._stop.set()
            self._reader.join(timeout=1.0)


class FakeOpenAiServer:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self.requests: list[dict[str, Any]] = []
        self._server = ThreadingHTTPServer(("127.0.0.1", 0), self._handler())
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    @property
    def base_url(self) -> str:
        host, port = self._server.server_address
        return f"http://{host}:{port}/v1"

    @property
    def request_count(self) -> int:
        with self._lock:
            return len(self.requests)

    def start(self) -> None:
        self._thread.start()

    def close(self) -> None:
        self._server.shutdown()
        self._server.server_close()
        self._thread.join(timeout=2.0)

    def _handler(self) -> type[BaseHTTPRequestHandler]:
        owner = self

        class Handler(BaseHTTPRequestHandler):
            server_version = "AceCodeSmokeOpenAI/1.0"

            def log_message(self, format: str, *args: object) -> None:
                return

            def do_POST(self) -> None:
                length = int(self.headers.get("Content-Length", "0") or "0")
                raw_body = self.rfile.read(length).decode("utf-8", errors="replace")
                try:
                    body: dict[str, Any] = json.loads(raw_body)
                except json.JSONDecodeError:
                    body = {}

                with owner._lock:
                    owner.requests.append({"path": self.path, "body": body})

                if owner._has_tool_result(body):
                    events = owner._final_events()
                elif owner._wants_ask(body):
                    events = owner._ask_events()
                else:
                    events = owner._normal_events()
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Connection", "close")
                self.end_headers()
                try:
                    for event in events:
                        self.wfile.write(f"data: {event}\n\n".encode("utf-8"))
                        self.wfile.flush()
                        time.sleep(0.02)
                    self.wfile.write(b"data: [DONE]\n\n")
                    self.wfile.flush()
                except BrokenPipeError:
                    return

        return Handler

    def _has_tool_result(self, body: dict[str, Any]) -> bool:
        messages = body.get("messages")
        if not isinstance(messages, list):
            return False
        for message in messages:
            if not isinstance(message, dict):
                continue
            if message.get("role") == "tool" and message.get("tool_call_id") == ASK_TOOL_CALL_ID:
                return True
        return False

    def _wants_ask(self, body: dict[str, Any]) -> bool:
        messages = body.get("messages")
        if not isinstance(messages, list):
            return False
        for message in reversed(messages):
            if not isinstance(message, dict) or message.get("role") != "user":
                continue
            content = message.get("content")
            if isinstance(content, str) and "ask" in content.lower():
                return True
            if isinstance(content, list):
                text = json.dumps(content, ensure_ascii=False).lower()
                if "ask" in text:
                    return True
        return False

    def _ask_events(self) -> list[str]:
        arguments = json.dumps(ASK_TOOL_ARGUMENTS, ensure_ascii=False, separators=(",", ":"))
        split = max(1, len(arguments) // 2)
        return [
            json.dumps(
                {"choices": [{"index": 0, "delta": {"role": "assistant"}, "finish_reason": None}]},
                separators=(",", ":"),
            ),
            json.dumps(
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {
                                "tool_calls": [
                                    {
                                        "index": 0,
                                        "id": ASK_TOOL_CALL_ID,
                                        "type": "function",
                                        "function": {
                                            "name": "AskUserQuestion",
                                            "arguments": arguments[:split],
                                        },
                                    }
                                ]
                            },
                            "finish_reason": None,
                        }
                    ]
                },
                separators=(",", ":"),
            ),
            json.dumps(
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {
                                "tool_calls": [
                                    {
                                        "index": 0,
                                        "function": {"arguments": arguments[split:]},
                                    }
                                ]
                            },
                            "finish_reason": "tool_calls",
                        }
                    ]
                },
                separators=(",", ":"),
            ),
        ]

    def _final_events(self) -> list[str]:
        return [
            json.dumps(
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {"content": ASK_FINAL_TEXT},
                            "finish_reason": "stop",
                        }
                    ]
                },
                separators=(",", ":"),
            )
        ]

    def _normal_events(self) -> list[str]:
        chunks = [
            "Streaming smoke response line 1.\n",
            "Streaming smoke response line 2 wraps enough text to exercise tail-follow and row cache updates.\n",
            "Streaming smoke response done.",
        ]
        return [
            json.dumps(
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {"content": chunk},
                            "finish_reason": "stop" if index == len(chunks) - 1 else None,
                        }
                    ]
                },
                separators=(",", ":"),
            )
            for index, chunk in enumerate(chunks)
        ]


def strip_ansi(text: str) -> str:
    text = ANSI_RE.sub("", text)
    return "".join(ch for ch in text if ch == "\n" or ch == "\r" or ch == "\t" or ord(ch) >= 32)


def fnv1a_64(data: str) -> int:
    value = 14695981039346656037
    for byte in data.encode("utf-8"):
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def project_hash(cwd: Path) -> str:
    normalized = str(cwd.resolve()).replace("\\", "/").lower()
    while len(normalized) > 1 and normalized.endswith("/"):
        normalized = normalized[:-1]
    return f"{fnv1a_64(normalized):016x}"


def now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def write_jsonl(path: Path, rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        for row in rows:
            handle.write(json.dumps(row, ensure_ascii=False, separators=(",", ":")))
            handle.write("\n")


def configure_home(home: Path, alt_screen_mode: str, base_url: str = "http://127.0.0.1:9/v1") -> None:
    config = {
        "provider": "openai",
        "openai": {
            "base_url": base_url,
            "api_key": "sk-smoke",
            "model": "smoke-model",
        },
        "saved_models": [
            {
                "name": "smoke-openai",
                "provider": "openai",
                "model": "smoke-model",
                "base_url": base_url,
                "api_key": "sk-smoke",
            }
        ],
        "default_model_name": "smoke-openai",
        "tui": {"alt_screen_mode": alt_screen_mode},
        "mcp_servers": {},
    }
    write_json(home / ".acecode" / "config.json", config)


def long_session_messages() -> list[dict[str, object]]:
    messages: list[dict[str, object]] = []
    messages.append({"role": "user", "content": "Generate a long smoke transcript."})
    for i in range(34):
        body = "\n".join(
            [
                f"assistant block {i:02d} line {j:02d}: viewport smoke content wraps and scrolls"
                for j in range(3)
            ]
        )
        messages.append({"role": "assistant", "content": body})
        if i % 6 == 0:
            messages.append({"role": "user", "content": f"follow-up prompt {i:02d}"})
    messages.append(
        {
            "role": "assistant",
            "content": "Inspecting generated files.",
            "tool_calls": [
                {
                    "id": "call-smoke-long",
                    "type": "function",
                    "function": {
                        "name": "bash",
                        "arguments": "{\"command\":\"printf smoke\"}",
                    },
                }
            ],
        }
    )
    messages.append(
        {
            "role": "tool",
            "tool_call_id": "call-smoke-long",
            "content": "\n".join(f"tool output detail {i:02d}" for i in range(45)),
            "metadata": {
                "tool_summary": {
                    "verb": "Ran",
                    "object": "smoke command with forty five output lines",
                    "icon": "$",
                    "metrics": [["exit", "0"], ["lines", "45"], ["time", "12ms"]],
                }
            },
        }
    )
    return messages


def short_session_messages() -> list[dict[str, object]]:
    return [
        {
            "role": "user",
            "content": "Short ChatViewport smoke seed.",
        },
        {
            "role": "assistant",
            "content": (
                "Short transcript seed for streaming, tail-follow, and selection "
                "checks. The selectable smoke text starts on this line."
            ),
        },
    ]


def tool_session_messages() -> list[dict[str, object]]:
    return [
        {
            "role": "tool",
            "tool_call_id": "call-smoke-tool",
            "content": "\n".join(f"expanded smoke output line {i:02d}" for i in range(18)),
            "metadata": {
                "tool_summary": {
                    "verb": "Read",
                    "object": "smoke-tool-result.txt",
                    "icon": "file",
                    "metrics": [["lines", "18"], ["time", "3ms"]],
                }
            },
        }
    ]


def create_session(home: Path, workspace: Path, session_id: str, title: str, rows: list[dict[str, object]]) -> None:
    project_dir = home / ".acecode" / "projects" / project_hash(workspace)
    timestamp = now_iso()
    write_jsonl(project_dir / f"{session_id}.jsonl", rows)
    write_json(
        project_dir / f"{session_id}.meta.json",
        {
            "id": session_id,
            "cwd": str(workspace),
            "created_at": timestamp,
            "updated_at": timestamp,
            "message_count": len(rows),
            "summary": title,
            "provider": "openai",
            "model": "smoke-model",
            "model_preset": "smoke-openai",
            "title": title,
            "permission_mode": "default",
            "turn_count": sum(1 for row in rows if row.get("role") == "user"),
        },
    )


def read_log(log_path: Path) -> str:
    if not log_path.exists():
        return ""
    return log_path.read_text(encoding="utf-8", errors="replace")


def parse_box(raw: str) -> tuple[int, int, int, int]:
    match = re.fullmatch(r"(-?\d+),(-?\d+)-(-?\d+),(-?\d+)", raw)
    if not match:
        raise ValueError(f"invalid box: {raw!r}")
    return tuple(int(match.group(i)) for i in range(1, 5))  # type: ignore[return-value]


def parse_traces(log_text: str) -> list[TraceEntry]:
    entries: list[TraceEntry] = []
    for match in TRACE_RE.finditer(log_text):
        try:
            chat_box = parse_box(match.group("chat"))
            scrollbar_box = parse_box(match.group("scrollbar"))
        except ValueError:
            continue
        entries.append(
            TraceEntry(
                top=int(match.group("top")),
                viewport_rows=int(match.group("viewport")),
                total_rows=int(match.group("total")),
                chat_box=chat_box,
                scrollbar_box=scrollbar_box,
            )
        )
    return entries


def parse_scroll_traces(log_text: str) -> list[ScrollTraceEntry]:
    entries: list[ScrollTraceEntry] = []
    for match in SCROLL_TRACE_RE.finditer(log_text):
        entries.append(
            ScrollTraceEntry(
                path=match.group("path"),
                button=match.group("button"),
                actual=int(match.group("actual")),
                before=int(match.group("before")),
                after=int(match.group("after")),
            )
        )
    return entries


def assert_box_sane(name: str, box: tuple[int, int, int, int], rows: int, cols: int) -> None:
    x_min, y_min, x_max, y_max = box
    if x_min < 0 or y_min < 0 or x_max < x_min or y_max < y_min:
        raise AssertionError(f"{name} invalid: {box}")
    if x_max >= cols or y_max >= rows:
        raise AssertionError(f"{name} outside {cols}x{rows}: {box}")


def sgr_mouse(code: int, x: int, y: int, pressed: bool = True) -> str:
    suffix = "M" if pressed else "m"
    return f"\x1b[<{code};{x};{y}{suffix}"


def wait_for_trace(log_path: Path, min_count: int, timeout: float = 8.0) -> list[TraceEntry]:
    deadline = time.monotonic() + timeout
    last: list[TraceEntry] = []
    while time.monotonic() < deadline:
        last = parse_traces(read_log(log_path))
        if len(last) >= min_count:
            return last
        time.sleep(0.05)
    raise RuntimeError(f"expected at least {min_count} ChatViewport trace entries, got {len(last)}")


def wait_for_scroll_trace(
    log_path: Path,
    path: str,
    button: str,
    min_count: int = 1,
    timeout: float = 8.0,
) -> list[ScrollTraceEntry]:
    deadline = time.monotonic() + timeout
    last: list[ScrollTraceEntry] = []
    while time.monotonic() < deadline:
        last = [
            entry
            for entry in parse_scroll_traces(read_log(log_path))
            if entry.path == path and entry.button == button
        ]
        if len(last) >= min_count:
            return last
        time.sleep(0.05)
    raise RuntimeError(
        f"expected at least {min_count} {path} {button} scroll traces, got {len(last)}"
    )


def wait_for_resume_tail_trace(log_path: Path, timeout: float = 8.0) -> list[TraceEntry]:
    deadline = time.monotonic() + timeout
    last: list[TraceEntry] = []
    while time.monotonic() < deadline:
        last = parse_traces(read_log(log_path))
        for entry in reversed(last):
            if entry.total_rows <= entry.viewport_rows:
                continue
            max_top = entry.total_rows - entry.viewport_rows
            if entry.top != max_top:
                raise AssertionError(
                    f"resume did not start at tail: top={entry.top} max_top={max_top}"
                )
            return last
        time.sleep(0.05)
    raise RuntimeError("expected a scrollable ChatViewport trace after resume")


def run_long_mode(
    exe: Path,
    root: Path,
    mode: str,
    verbose: bool,
    force_compat: bool = False,
) -> dict[str, object]:
    label = "compat" if force_compat else mode
    home = root / f"home-{label}"
    workspace = root / f"workspace-{label}"
    home.mkdir(parents=True, exist_ok=True)
    workspace.mkdir(parents=True, exist_ok=True)
    configure_home(home, mode)
    session_id = f"20260523-000000-{label[:4]}"
    create_session(home, workspace, session_id, f"ChatViewport smoke {label}", long_session_messages())

    env = os.environ.copy()
    env["USERPROFILE"] = str(home)
    env["HOME"] = str(home)
    env["ACECODE_TUI_CHAT_VIEWPORT"] = "1"
    env["ACECODE_TUI_CHAT_VIEWPORT_TRACE"] = "1"
    if force_compat:
        env["ACECODE_TUI_FORCE_CONHOST_COMPAT_LAYOUT"] = "1"

    log_path = workspace / "acecode.log"
    if log_path.exists():
        log_path.unlink()

    runner = PtyRunner(
        [str(exe), "--resume", session_id],
        cwd=workspace,
        env=env,
        dimensions=(30, 110),
        verbose=verbose,
    )
    try:
        initial = wait_for_resume_tail_trace(log_path)
        runner.write("\x1b[5~")  # PageUp
        time.sleep(0.25)
        runner.write("\x1b[6~")  # PageDown
        time.sleep(0.25)
        runner.write(sgr_mouse(64, 10, 10))  # WheelUp in chat area.
        time.sleep(0.25)
        runner.write(sgr_mouse(65, 10, 10))  # WheelDown in chat area.
        time.sleep(0.25)
        traces_after_scroll = wait_for_trace(log_path, len(initial) + 1)

        last_box = traces_after_scroll[-1].scrollbar_box
        scrollbar_x = max(1, last_box[0])
        runner.write(sgr_mouse(0, scrollbar_x, last_box[1]))
        time.sleep(0.1)
        runner.write(sgr_mouse(32, scrollbar_x, (last_box[1] + last_box[3]) // 2))
        time.sleep(0.1)
        runner.write(sgr_mouse(0, scrollbar_x, (last_box[1] + last_box[3]) // 2, pressed=False))
        time.sleep(0.25)

        runner.resize(38, 132)
        time.sleep(0.6)
        runner.write("\x1b[5~")
        time.sleep(0.25)
        final_traces = parse_traces(read_log(log_path))
        if len(final_traces) <= len(initial):
            raise AssertionError("scroll/resize did not produce additional ChatViewport traces")
        latest = final_traces[-1]
        assert_box_sane("chat_box", latest.chat_box, 38, 132)
        assert_box_sane("scrollbar_box", latest.scrollbar_box, 38, 132)
        if latest.total_rows <= latest.viewport_rows:
            raise AssertionError(
                f"long session was not scrollable: total={latest.total_rows} viewport={latest.viewport_rows}"
            )

        log_text = read_log(log_path)
        modes = [m.groupdict() for m in FTXUI_MODE_RE.finditer(log_text)]
        expected = ("1", "0") if mode == "always" or force_compat else ("0", "1")
        if not any((m["alt"], m["terminal"]) == expected for m in modes):
            raise AssertionError(f"did not observe expected FTXUI mode {expected} in log")

        tui_modes = [m.groupdict() for m in TUI_MODE_RE.finditer(log_text)]
        if force_compat:
            if not any(m["mode"] == "AltScreen" and m["compat"] == "1" for m in tui_modes):
                raise AssertionError("forced compatibility run did not enter compat AltScreen mode")
        elif not any(m["compat"] == "0" for m in tui_modes):
            raise AssertionError("non-compat run did not log conhost_compat_layout=0")

        return {
            "mode": label,
            "trace_count": len(final_traces),
            "top_values": sorted({entry.top for entry in final_traces}),
            "latest_chat_box": latest.chat_box,
            "latest_scrollbar_box": latest.scrollbar_box,
            "total_rows": latest.total_rows,
            "viewport_rows": latest.viewport_rows,
            "conhost_compat_layout": force_compat,
            "log": str(log_path),
        }
    finally:
        runner.close()


def run_slash_resume_tail(
    exe: Path,
    root: Path,
    verbose: bool,
    use_chat_viewport: bool = True,
) -> dict[str, object]:
    suffix = "slash-resume" if use_chat_viewport else "slash-resume-legacy"
    home = root / f"home-{suffix}"
    workspace = root / f"workspace-{suffix}"
    home.mkdir(parents=True, exist_ok=True)
    workspace.mkdir(parents=True, exist_ok=True)
    configure_home(home, "always")
    session_id = "20260523-000004-a11c"
    create_session(
        home,
        workspace,
        session_id,
        "ChatViewport slash resume smoke",
        long_session_messages(),
    )

    env = os.environ.copy()
    env["USERPROFILE"] = str(home)
    env["HOME"] = str(home)
    if use_chat_viewport:
        env["ACECODE_TUI_CHAT_VIEWPORT_TRACE"] = "1"
    else:
        env["ACECODE_TUI_CHAT_VIEWPORT"] = "0"
        env["ACECODE_TUI_SCROLL_TRACE"] = "1"

    log_path = workspace / "acecode.log"
    if log_path.exists():
        log_path.unlink()

    runner = PtyRunner(
        [str(exe)],
        cwd=workspace,
        env=env,
        dimensions=(30, 110),
        verbose=verbose,
    )
    try:
        runner.wait_until(
            lambda: "Type your prompt here" in runner.clean_output(),
            10.0,
            "initial prompt",
        )
        runner.write("/resume \r")
        runner.wait_until(
            lambda: "Resume a session" in runner.clean_output(),
            8.0,
            "resume picker",
        )
        runner.write("\r")
        if not use_chat_viewport:
            runner.wait_until(
                lambda: f"Resumed session {session_id}" in runner.clean_output(),
                8.0,
                "legacy slash resume tail",
            )
            runner.write(sgr_mouse(64, 10, 10))
            legacy_wheel = wait_for_scroll_trace(log_path, "legacy", "WheelUp")[-1]
            if legacy_wheel.actual >= 0 or legacy_wheel.after >= legacy_wheel.before:
                raise AssertionError(
                    "legacy slash resume first WheelUp did not move upward: "
                    f"actual={legacy_wheel.actual} top={legacy_wheel.before}->{legacy_wheel.after}"
                )
            return {
                "mode": "slash-resume-legacy",
                "resumed_tail_seen": True,
                "first_wheel_actual": legacy_wheel.actual,
                "first_wheel_top": [legacy_wheel.before, legacy_wheel.after],
                "log": str(log_path),
            }
        traces = wait_for_resume_tail_trace(log_path)
        latest = traces[-1]
        assert_box_sane("chat_box", latest.chat_box, 30, 110)
        assert_box_sane("scrollbar_box", latest.scrollbar_box, 30, 110)
        runner.write(sgr_mouse(64, 10, 10))
        viewport_wheel = wait_for_scroll_trace(log_path, "viewport", "WheelUp")[-1]
        if viewport_wheel.actual >= 0 or viewport_wheel.after >= viewport_wheel.before:
            raise AssertionError(
                "slash resume first ChatViewport WheelUp did not move upward: "
                f"actual={viewport_wheel.actual} "
                f"top={viewport_wheel.before}->{viewport_wheel.after}"
            )
        return {
            "mode": "slash-resume",
            "trace_count": len(parse_traces(read_log(log_path))),
            "tail_top": latest.top,
            "first_wheel_top": [viewport_wheel.before, viewport_wheel.after],
            "max_top": max(0, latest.total_rows - latest.viewport_rows),
            "latest_chat_box": latest.chat_box,
            "latest_scrollbar_box": latest.scrollbar_box,
            "log": str(log_path),
        }
    finally:
        runner.close()


def run_tool_expand(exe: Path, root: Path, verbose: bool) -> dict[str, object]:
    home = root / "home-tool"
    workspace = root / "workspace-tool"
    home.mkdir(parents=True, exist_ok=True)
    workspace.mkdir(parents=True, exist_ok=True)
    configure_home(home, "always")
    session_id = "20260523-000001-tool"
    create_session(home, workspace, session_id, "ChatViewport tool expand smoke", tool_session_messages())

    env = os.environ.copy()
    env["USERPROFILE"] = str(home)
    env["HOME"] = str(home)
    env["ACECODE_TUI_CHAT_VIEWPORT"] = "1"
    env["ACECODE_TUI_CHAT_VIEWPORT_TRACE"] = "1"

    runner = PtyRunner(
        [str(exe), "--resume", session_id],
        cwd=workspace,
        env=env,
        dimensions=(24, 96),
        verbose=verbose,
    )
    try:
        runner.wait_until(lambda: "smoke-tool-result.txt" in runner.clean_output(), 8.0, "collapsed summary")
        before = runner.clean_output()
        if "expanded smoke output line 10" in before:
            raise AssertionError("tool result detail was visible before Ctrl+O")
        runner.write("\x0f")  # Ctrl+O toggles focused summarized tool_result.
        runner.wait_until(
            lambda: "expanded smoke output line 10" in runner.clean_output(),
            8.0,
            "expanded tool result detail",
        )
        return {
            "mode": "tool-expand",
            "collapsed_summary_seen": "smoke-tool-result.txt" in before,
            "expanded_detail_seen": True,
            "log": str(workspace / "acecode.log"),
        }
    finally:
        runner.close()


def run_short_streaming(exe: Path, root: Path, verbose: bool) -> dict[str, object]:
    home = root / "home-stream"
    workspace = root / "workspace-stream"
    home.mkdir(parents=True, exist_ok=True)
    workspace.mkdir(parents=True, exist_ok=True)

    fake_openai = FakeOpenAiServer()
    fake_openai.start()
    configure_home(home, "always", fake_openai.base_url)

    session_id = "20260523-000003-stream"
    create_session(home, workspace, session_id, "ChatViewport short streaming smoke", short_session_messages())

    env = os.environ.copy()
    env["USERPROFILE"] = str(home)
    env["HOME"] = str(home)
    env["ACECODE_TUI_CHAT_VIEWPORT"] = "1"
    env["ACECODE_TUI_CHAT_VIEWPORT_TRACE"] = "1"

    log_path = workspace / "acecode.log"
    if log_path.exists():
        log_path.unlink()

    runner = PtyRunner(
        [str(exe), "--resume", session_id],
        cwd=workspace,
        env=env,
        dimensions=(26, 100),
        verbose=verbose,
    )
    try:
        before_traces = wait_for_trace(log_path, 1)
        before_total_rows = before_traces[-1].total_rows
        runner.write("stream smoke\r")
        runner.wait_until(lambda: fake_openai.request_count >= 1, 12.0, "fake OpenAI streaming request")
        runner.wait_until(
            lambda: "Streaming smoke response done." in runner.clean_output(),
            12.0,
            "streamed assistant response",
        )
        final_traces = parse_traces(read_log(log_path))
        latest = final_traces[-1]
        if latest.total_rows <= before_total_rows:
            raise AssertionError(
                f"streaming did not add transcript rows: before={before_total_rows} after={latest.total_rows}"
            )
        max_top = max(0, latest.total_rows - latest.viewport_rows)
        if latest.top != max_top:
            raise AssertionError(
                f"tail-follow did not stay pinned: top={latest.top} max_top={max_top}"
            )
        return {
            "mode": "short-streaming",
            "request_count": fake_openai.request_count,
            "before_total_rows": before_total_rows,
            "after_total_rows": latest.total_rows,
            "tail_top": latest.top,
            "latest_chat_box": latest.chat_box,
            "latest_scrollbar_box": latest.scrollbar_box,
            "log": str(log_path),
        }
    finally:
        runner.close()
        fake_openai.close()


def run_selection_drag(exe: Path, root: Path, verbose: bool) -> dict[str, object]:
    home = root / "home-selection"
    workspace = root / "workspace-selection"
    home.mkdir(parents=True, exist_ok=True)
    workspace.mkdir(parents=True, exist_ok=True)
    configure_home(home, "always")
    session_id = "20260523-000004-selection"
    create_session(home, workspace, session_id, "ChatViewport selection copy smoke", short_session_messages())

    env = os.environ.copy()
    env["USERPROFILE"] = str(home)
    env["HOME"] = str(home)
    env["ACECODE_TUI_CHAT_VIEWPORT"] = "1"
    env["ACECODE_TUI_CHAT_VIEWPORT_TRACE"] = "1"
    log_path = workspace / "acecode.log"
    if log_path.exists():
        log_path.unlink()

    runner = PtyRunner(
        [str(exe), "--resume", session_id],
        cwd=workspace,
        env=env,
        dimensions=(24, 96),
        verbose=verbose,
    )
    try:
        latest = wait_for_trace(log_path, 1)[-1]
        content_top = latest.chat_box[3] - min(latest.total_rows, latest.viewport_rows) + 1
        y = max(latest.chat_box[1], min(latest.chat_box[3], content_top + 4))
        x_start = min(latest.chat_box[2] - 10, latest.chat_box[0] + 6)
        x_end = min(latest.chat_box[2] - 4, x_start + 48)
        if x_start >= x_end:
            raise AssertionError(f"selection coordinates invalid: x_start={x_start} x_end={x_end}")

        # SGR mouse coordinates are one-based; FTXUI boxes are zero-based.
        sx_start = x_start + 1
        sx_end = x_end + 1
        sy = y + 1
        runner.write(sgr_mouse(0, sx_start, sy))
        time.sleep(0.15)
        runner.write(sgr_mouse(32, sx_end, sy))
        time.sleep(0.15)
        runner.write(sgr_mouse(0, sx_end, sy, pressed=False))
        deadline = time.monotonic() + 8.0
        selection_log_seen = False
        while time.monotonic() < deadline:
            log_text = read_log(log_path)
            if "HandleSelection moved" in log_text and "empty=1->0" in log_text:
                selection_log_seen = True
                break
            time.sleep(0.05)
        if not selection_log_seen:
            raise AssertionError("FTXUI selection drag was not observed in the trace log")
        return {
            "mode": "selection-drag",
            "selection_drag_seen": True,
            "selection_line_y": y,
            "selection_x_range": [x_start, x_end],
            "latest_chat_box": latest.chat_box,
            "latest_scrollbar_box": latest.scrollbar_box,
            "log": str(log_path),
        }
    finally:
        runner.close()


def run_ask_overlay(exe: Path, root: Path, verbose: bool) -> dict[str, object]:
    home = root / "home-ask"
    workspace = root / "workspace-ask"
    home.mkdir(parents=True, exist_ok=True)
    workspace.mkdir(parents=True, exist_ok=True)

    fake_openai = FakeOpenAiServer()
    fake_openai.start()
    configure_home(home, "always", fake_openai.base_url)

    session_id = "20260523-000002-ask"
    create_session(home, workspace, session_id, "ChatViewport AskUserQuestion smoke", long_session_messages())

    env = os.environ.copy()
    env["USERPROFILE"] = str(home)
    env["HOME"] = str(home)
    env["ACECODE_TUI_CHAT_VIEWPORT"] = "1"
    env["ACECODE_TUI_CHAT_VIEWPORT_TRACE"] = "1"

    log_path = workspace / "acecode.log"
    if log_path.exists():
        log_path.unlink()

    runner = PtyRunner(
        [str(exe), "--resume", session_id],
        cwd=workspace,
        env=env,
        dimensions=(30, 110),
        verbose=verbose,
    )
    try:
        wait_for_trace(log_path, 1)
        runner.write("ask smoke\r")
        runner.wait_until(lambda: fake_openai.request_count >= 1, 12.0, "first fake OpenAI request")
        runner.wait_until(
            lambda: "Choose the smoke path?" in runner.clean_output()
            and "Proceed" in runner.clean_output(),
            12.0,
            "AskUserQuestion overlay",
        )

        traces_before_review = parse_traces(read_log(log_path))
        runner.write("\x1b[5~")  # PageUp should review the chat transcript behind the overlay.
        time.sleep(0.2)
        runner.write("\x1b[5~")
        time.sleep(0.2)
        traces_after_review = wait_for_trace(log_path, len(traces_before_review) + 1)
        top_values = sorted({entry.top for entry in traces_after_review})
        if len(top_values) < 2:
            raise AssertionError("Ask overlay PageUp did not move the chat transcript")

        runner.write("\r")  # Default focus is the first option: Proceed.
        runner.wait_until(
            lambda: fake_openai.request_count >= 2,
            12.0,
            "follow-up fake OpenAI request after AskUserQuestion answer",
        )
        for _ in range(8):
            runner.write("\x1b[6~")
            time.sleep(0.05)
        runner.wait_until(lambda: ASK_FINAL_TEXT in runner.clean_output(), 12.0, "final assistant text")

        with fake_openai._lock:
            request_bodies = [request["body"] for request in fake_openai.requests]
        second_request_json = json.dumps(request_bodies[-1], ensure_ascii=False)
        if ASK_TOOL_CALL_ID not in second_request_json or "Proceed" not in second_request_json:
            raise AssertionError("AskUserQuestion answer was not sent back as a tool result")

        latest = parse_traces(read_log(log_path))[-1]
        assert_box_sane("chat_box", latest.chat_box, 30, 110)
        assert_box_sane("scrollbar_box", latest.scrollbar_box, 30, 110)
        return {
            "mode": "ask-overlay",
            "overlay_seen": True,
            "answer_roundtrip_seen": True,
            "request_count": fake_openai.request_count,
            "chat_top_values": top_values,
            "latest_chat_box": latest.chat_box,
            "latest_scrollbar_box": latest.scrollbar_box,
            "log": str(log_path),
        }
    finally:
        runner.close()
        fake_openai.close()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--exe",
        type=Path,
        default=Path("build") / "Release" / "acecode.exe",
        help="Path to acecode.exe, default: build/Release/acecode.exe",
    )
    parser.add_argument(
        "--mode",
        choices=[
            "always",
            "never",
            "compat",
            "stream",
            "selection",
            "ask",
            "slash-resume",
            "slash-resume-legacy",
            "both",
        ],
        default="both",
        help="TUI mode to smoke; both runs AltScreen, TerminalOutput, forced compat, slash resume, legacy slash resume, streaming, tool expand, and ask overlay",
    )
    parser.add_argument("--keep-temp", action="store_true", help="Keep generated temp files")
    parser.add_argument("--verbose", action="store_true", help="Mirror TUI output to stdout")
    args = parser.parse_args(argv)

    exe = args.exe.resolve()
    if not exe.exists():
        raise SystemExit(f"acecode executable not found: {exe}")

    root = Path(tempfile.mkdtemp(prefix="acecode-chat-viewport-smoke-"))
    try:
        results: list[dict[str, object]] = []
        if args.mode == "both":
            results.append(run_long_mode(exe, root, "always", args.verbose))
            results.append(run_long_mode(exe, root, "never", args.verbose))
            results.append(run_long_mode(exe, root, "never", args.verbose, force_compat=True))
            results.append(run_slash_resume_tail(exe, root, args.verbose))
            results.append(run_slash_resume_tail(exe, root, args.verbose, use_chat_viewport=False))
            results.append(run_short_streaming(exe, root, args.verbose))
            results.append(run_selection_drag(exe, root, args.verbose))
            results.append(run_tool_expand(exe, root, args.verbose))
            results.append(run_ask_overlay(exe, root, args.verbose))
        elif args.mode == "ask":
            results.append(run_ask_overlay(exe, root, args.verbose))
        elif args.mode == "slash-resume":
            results.append(run_slash_resume_tail(exe, root, args.verbose))
        elif args.mode == "slash-resume-legacy":
            results.append(run_slash_resume_tail(exe, root, args.verbose, use_chat_viewport=False))
        elif args.mode == "stream":
            results.append(run_short_streaming(exe, root, args.verbose))
        elif args.mode == "selection":
            results.append(run_selection_drag(exe, root, args.verbose))
        elif args.mode == "compat":
            results.append(run_long_mode(exe, root, "never", args.verbose, force_compat=True))
            results.append(run_tool_expand(exe, root, args.verbose))
        else:
            results.append(run_long_mode(exe, root, args.mode, args.verbose))
            results.append(run_tool_expand(exe, root, args.verbose))
        print(json.dumps({"ok": True, "root": str(root), "results": results}, indent=2))
    finally:
        if args.keep_temp:
            print(f"kept temp root: {root}")
        else:
            shutil.rmtree(root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
