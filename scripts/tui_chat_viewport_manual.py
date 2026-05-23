#!/usr/bin/env python3
"""Interactive manual validation launcher for ACECode ChatViewport.

Run this from the terminal host you want to validate, for example Windows
Terminal or classic conhost. The launcher creates an isolated USERPROFILE,
starts a local fake OpenAI-compatible SSE server, seeds deterministic sessions,
and then runs acecode with ChatViewport enabled.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from tui_chat_viewport_smoke import (
    FakeOpenAiServer,
    FTXUI_MODE_RE,
    TUI_MODE_RE,
    configure_home,
    create_session,
    long_session_messages,
    parse_traces,
    read_log,
    short_session_messages,
    tool_session_messages,
)

MANUAL_CASES = ["short", "long", "ask", "tool"]
SNAPSHOT_FILES = [
    "main.cpp",
    "CMakeLists.txt",
    "src/tui/chat_viewport.cpp",
    "src/tui/chat_viewport.hpp",
    "src/tui/chat_viewport_cache.cpp",
    "src/tui/chat_viewport_cache.hpp",
    "src/tui/chat_viewport_model.hpp",
    "scripts/tui_chat_viewport_manual.py",
    "scripts/tui_chat_viewport_report.py",
    "scripts/tui_chat_viewport_validation.ps1",
]
BUILD_INPUT_FILES = [
    "main.cpp",
    "CMakeLists.txt",
    "src/tui/chat_viewport.cpp",
    "src/tui/chat_viewport.hpp",
    "src/tui/chat_viewport_cache.cpp",
    "src/tui/chat_viewport_cache.hpp",
    "src/tui/chat_viewport_model.hpp",
]


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def file_digest(path: Path) -> dict[str, object]:
    if not path.exists() or not path.is_file():
        return {"missing": True}
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    stat = path.stat()
    return {
        "missing": False,
        "sha256": digest.hexdigest(),
        "size": stat.st_size,
    }


def executable_is_current(exe: Path) -> bool:
    if not exe.exists() or not exe.is_file():
        return False
    root = repo_root()
    input_mtimes: list[float] = []
    for relative in BUILD_INPUT_FILES:
        path = root / relative
        if not path.exists() or not path.is_file():
            return False
        input_mtimes.append(path.stat().st_mtime)
    return exe.stat().st_mtime >= max(input_mtimes)


def build_source_snapshot(exe: Path) -> dict[str, object]:
    root = repo_root()
    return {
        "version": 1,
        "repo_root": str(root),
        "files": {
            relative: file_digest(root / relative)
            for relative in SNAPSHOT_FILES
        },
        "executable": {
            "path": str(exe),
            **file_digest(exe),
        },
        "executable_is_current": executable_is_current(exe),
    }


def safe_label(value: str) -> str:
    label = re.sub(r"[^A-Za-z0-9_.-]+", "-", value.strip())
    return label.strip("-") or "case"


def detect_terminal_environment(environ: dict[str, str]) -> dict[str, object]:
    signals = {
        "WT_SESSION": bool(environ.get("WT_SESSION")),
        "ConEmuPID": bool(environ.get("ConEmuPID")),
        "TERM_PROGRAM": environ.get("TERM_PROGRAM", ""),
        "TERM": environ.get("TERM", ""),
        "SESSIONNAME": environ.get("SESSIONNAME", ""),
    }
    if signals["WT_SESSION"]:
        detected_host = "windows-terminal"
    elif os.name == "nt":
        if signals["ConEmuPID"] or signals["TERM_PROGRAM"] or signals["TERM"]:
            detected_host = "other"
        else:
            detected_host = "classic-conhost"
    else:
        detected_host = "non-windows"
    return {
        "detected_host": detected_host,
        "signals": signals,
    }


def terminal_host_matches_requested(requested: str, detected: str) -> bool:
    if requested in ("unspecified", "other"):
        return True
    if requested == "windows-terminal":
        return detected == "windows-terminal"
    if requested == "classic-conhost":
        return detected == "classic-conhost"
    return False


def terminal_host_mismatch_message(
    requested: str,
    terminal_environment: dict[str, object],
) -> str | None:
    detected = str(terminal_environment.get("detected_host", "unknown"))
    if terminal_host_matches_requested(requested, detected):
        return None
    return (
        f"requested terminal host '{requested}' does not match detected host "
        f"'{detected}'. Rerun from the requested terminal, or use --print-only "
        "only to preview commands."
    )


def checklist(
    case: str,
    terminal_host: str,
    detected_terminal_host: str,
    alt_screen_mode: str,
    force_compat: bool,
    force_osc52_copy: bool,
) -> list[str]:
    common = [
        "Confirm the chat box stays visually stable after several redraws.",
        "Confirm the transcript is rendered and wheel/PageUp/PageDown move by rows.",
        "Drag-select transcript text; selection should remain stable while scrolling.",
        "Right-click selected text and confirm copy status appears.",
        "Resize the terminal; transcript should rewrap, clamp, and keep tail-follow when pinned.",
    ]
    if case == "short":
        specific = [
            "Type a normal prompt; fake OpenAI should stream three chunks.",
            "While streaming at tail, output should remain pinned to the newest row.",
            "Scroll up, type another prompt, and confirm review position is not stolen unless tail is pinned.",
        ]
    elif case == "long":
        specific = [
            "Use fast wheel scrolling through the seeded long transcript.",
            "Drag the chat scrollbar to top, middle, and bottom.",
            "Verify long assistant messages scroll internally by display row, not by whole message only.",
        ]
    elif case == "ask":
        specific = [
            "Type 'ask smoke' to trigger the fake OpenAI AskUserQuestion tool call.",
            "While the overlay is open, wheel/PageUp over transcript and drag-select text behind it.",
            "Press Return on Proceed; final text should become 'Ask smoke complete.'.",
        ]
    else:
        specific = [
            "Press Ctrl+O on the focused summarized tool result.",
            "Expanded output should appear without jumping to the wrong scroll position.",
            "Press Ctrl+O again and verify the summary row returns.",
        ]

    mode = f"alt_screen_mode={alt_screen_mode}"
    if force_compat:
        mode += ", forced conhost compatibility layout"
    if force_osc52_copy:
        mode += ", forced OSC 52 copy"
    host = f"Confirm this run is inside terminal_host={terminal_host}; detected_host={detected_terminal_host}."
    return [f"Mode: {mode}", host, *specific, *common]


def initial_checklist_results(checklist_items: list[str]) -> list[dict[str, object]]:
    return [
        {
            "index": index,
            "item": item,
            "status": "manual-unverified",
            "note": "",
        }
        for index, item in enumerate(checklist_items, start=1)
    ]


def summarize_checklist_results(results: list[dict[str, object]]) -> dict[str, int]:
    summary = {
        "passed": 0,
        "failed": 0,
        "skipped": 0,
        "manual_unverified": 0,
    }
    for result in results:
        status = str(result.get("status", "manual-unverified"))
        if status == "passed":
            summary["passed"] += 1
        elif status == "failed":
            summary["failed"] += 1
        elif status == "skipped":
            summary["skipped"] += 1
        else:
            summary["manual_unverified"] += 1
    return summary


def record_checklist_results(results: list[dict[str, object]]) -> None:
    print()
    print("Record manual results. Enter p/pass, f/fail, s/skip, or u/unverified.")
    print("You may add a short note after the status, for example: p copied via OSC52.")
    choices = {
        "p": "passed",
        "pass": "passed",
        "passed": "passed",
        "f": "failed",
        "fail": "failed",
        "failed": "failed",
        "s": "skipped",
        "skip": "skipped",
        "skipped": "skipped",
        "u": "manual-unverified",
        "unverified": "manual-unverified",
        "manual-unverified": "manual-unverified",
        "": "manual-unverified",
    }
    for result in results:
        prompt = f"[{result['index']}] {result['item']}\nresult> "
        while True:
            raw = input(prompt).strip()
            if raw:
                head, _, note = raw.partition(" ")
            else:
                head, note = "", ""
            status = choices.get(head.lower())
            if status is not None:
                result["status"] = status
                result["note"] = note.strip()
                break
            print("Expected p/pass, f/fail, s/skip, or u/unverified.")


def summarize_log(log_path: Path) -> dict[str, object]:
    log_text = read_log(log_path)
    traces = parse_traces(log_text)
    latest = traces[-1] if traces else None
    chat_box_samples = sorted({entry.chat_box for entry in traces})
    scrollbar_box_samples = sorted({entry.scrollbar_box for entry in traces})
    trace_boxes_sane = bool(traces) and all(
        entry.viewport_rows > 0
        and entry.chat_box[2] >= entry.chat_box[0]
        and entry.chat_box[3] >= entry.chat_box[1]
        and entry.scrollbar_box[2] >= entry.scrollbar_box[0]
        and entry.scrollbar_box[3] >= entry.scrollbar_box[1]
        for entry in traces
    )
    return {
        "log_path": str(log_path),
        "trace_count": len(traces),
        "trace_boxes_sane": trace_boxes_sane,
        "chat_box_samples": chat_box_samples[:16],
        "scrollbar_box_samples": scrollbar_box_samples[:16],
        "latest_trace": None
        if latest is None
        else {
            "top": latest.top,
            "viewport_rows": latest.viewport_rows,
            "total_rows": latest.total_rows,
            "chat_box": latest.chat_box,
            "scrollbar_box": latest.scrollbar_box,
        },
        "top_values": sorted({entry.top for entry in traces}),
        "ftxui_modes": [match.groupdict() for match in FTXUI_MODE_RE.finditer(log_text)],
        "tui_modes": [match.groupdict() for match in TUI_MODE_RE.finditer(log_text)],
        "saw_ask_final_text": "Ask smoke complete." in log_text,
        "saw_osc52_copy": "Sent OSC 52 copy request" in log_text,
        "saw_system_clipboard_copy": "clipboard via system clipboard" in log_text,
    }


def messages_for_case(case: str) -> list[dict[str, object]]:
    if case == "short":
        return short_session_messages()
    if case == "long" or case == "ask":
        return long_session_messages()
    return tool_session_messages()


def write_report(
    report_path: Path,
    *,
    label: str,
    case: str,
    root: Path,
    home: Path,
    workspace: Path,
    command: list[str],
    checklist_results: list[dict[str, object]],
    args: argparse.Namespace,
    fake_openai: FakeOpenAiServer,
    returncode: int | None,
) -> None:
    log_path = workspace / "acecode.log"
    report = {
        "label": label,
        "case": case,
        "terminal_host": args.terminal_host,
        "terminal_environment": args.terminal_environment,
        "terminal_host_matches_detection": terminal_host_matches_requested(
            args.terminal_host,
            str(args.terminal_environment.get("detected_host", "unknown")),
        ),
        "alt_screen_mode": args.alt_screen_mode,
        "force_compat": args.force_compat,
        "force_osc52_copy": args.force_osc52_copy,
        "print_only": args.print_only,
        "returncode": returncode,
        "root": str(root),
        "home": str(home),
        "workspace": str(workspace),
        "command": command,
        "fake_openai_base_url": fake_openai.base_url,
        "fake_openai_request_count": fake_openai.request_count,
        "source_snapshot": build_source_snapshot(Path(command[0])),
        "checklist_summary": summarize_checklist_results(checklist_results),
        "checklist": checklist_results,
        "log_summary": summarize_log(log_path),
    }
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def run_manual_case(
    args: argparse.Namespace,
    *,
    exe: Path,
    case: str,
    report_path: Path | None,
    label: str | None = None,
    case_index: int | None = None,
    case_count: int | None = None,
) -> int:
    label = label or case
    safe_case_label = safe_label(label)
    root = Path(tempfile.mkdtemp(prefix=f"acecode-chat-viewport-manual-{safe_case_label}-"))
    home = root / "home"
    workspace = root / "workspace"
    home.mkdir(parents=True, exist_ok=True)
    workspace.mkdir(parents=True, exist_ok=True)

    fake_openai = FakeOpenAiServer()
    fake_openai.start()
    configure_home(home, args.alt_screen_mode, fake_openai.base_url)

    session_id = f"20260523-manual-{safe_case_label}"
    create_session(home, workspace, session_id, f"ChatViewport manual {label}", messages_for_case(case))

    env = os.environ.copy()
    env["USERPROFILE"] = str(home)
    env["HOME"] = str(home)
    env["ACECODE_TUI_CHAT_VIEWPORT"] = "1"
    env["ACECODE_TUI_CHAT_VIEWPORT_TRACE"] = "1"
    if args.force_compat:
        env["ACECODE_TUI_FORCE_CONHOST_COMPAT_LAYOUT"] = "1"
    if args.force_osc52_copy:
        env["ACECODE_TUI_FORCE_OSC52_COPY"] = "1"

    command = [str(exe), "--resume", session_id]
    terminal_environment = detect_terminal_environment(os.environ)
    args.terminal_environment = terminal_environment
    checklist_items = checklist(
        case,
        args.terminal_host,
        str(terminal_environment.get("detected_host", "unknown")),
        args.alt_screen_mode,
        args.force_compat,
        args.force_osc52_copy,
    )
    if report_path is None:
        report_path = root / "manual-report.json"

    if case_index is not None and case_count is not None:
        print("=" * 72)
        print(f"Matrix case {case_index}/{case_count}: {label} ({case})")
    print("Temp root:", root)
    print("Terminal host:", args.terminal_host)
    print("Detected terminal host:", terminal_environment.get("detected_host", "unknown"))
    print("Workspace:", workspace)
    print("Log:", workspace / "acecode.log")
    print("Report:", report_path)
    if args.cleanup and report_path == root / "manual-report.json":
        print("Report note: default report is inside the temp root and will be deleted by --cleanup.")
    print("Command:", " ".join(command))
    print()
    print("Checklist:")
    for index, item in enumerate(checklist_items, start=1):
        print(f"{index}. {item}")
    print()

    returncode: int | None = None
    checklist_results = initial_checklist_results(checklist_items)
    try:
        if args.print_only:
            print("Print-only mode: fake OpenAI server stops when this script exits.")
            if args.record_results:
                print("Result recording is skipped in print-only mode.")
            return 0
        returncode = subprocess.run(command, cwd=workspace, env=env, check=False).returncode
        if args.record_results:
            record_checklist_results(checklist_results)
        return returncode
    finally:
        write_report(
            report_path,
            label=label,
            case=case,
            root=root,
            home=home,
            workspace=workspace,
            command=command,
            checklist_results=checklist_results,
            args=args,
            fake_openai=fake_openai,
            returncode=returncode,
        )
        print("Wrote report:", report_path)
        fake_openai.close()
        if args.cleanup:
            shutil.rmtree(root, ignore_errors=True)
        else:
            print()
            print("Kept temp root:", root)


def clone_args(
    args: argparse.Namespace,
    *,
    alt_screen_mode: str,
    force_compat: bool,
) -> argparse.Namespace:
    cloned = argparse.Namespace(**vars(args))
    cloned.alt_screen_mode = alt_screen_mode
    cloned.force_compat = force_compat
    return cloned


def run_report_sequence(
    args: argparse.Namespace,
    *,
    exe: Path,
    report_dir: Path,
    entries: list[tuple[str, str, str, bool]],
    title: str,
) -> int:
    report_dir.mkdir(parents=True, exist_ok=True)
    report_paths = [
        report_dir / f"manual-report-{safe_label(label)}.json"
        for label, _case, _alt_screen_mode, _force_compat in entries
    ]
    final_code = 0
    for index, ((label, case, alt_screen_mode, force_compat), report_path) in enumerate(
        zip(entries, report_paths),
        start=1,
    ):
        case_args = clone_args(
            args,
            alt_screen_mode=alt_screen_mode,
            force_compat=force_compat,
        )
        code = run_manual_case(
            case_args,
            exe=exe,
            case=case,
            label=label,
            report_path=report_path,
            case_index=index,
            case_count=len(entries),
        )
        if code != 0 and final_code == 0:
            final_code = code

    print("=" * 72)
    print(f"{title} reports:")
    for report_path in report_paths:
        print(" ", report_path)
    verify_command = [
        sys.executable,
        "scripts\\tui_chat_viewport_report.py",
        "--strict",
        str(report_dir),
    ]
    print("Verifier:", subprocess.list2cmdline(verify_command))
    return final_code


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--exe",
        type=Path,
        default=Path("build") / "Release" / "acecode.exe",
        help="Path to acecode.exe, default: build/Release/acecode.exe",
    )
    parser.add_argument(
        "--case",
        choices=MANUAL_CASES,
        default="short",
        help="Manual validation case to launch",
    )
    parser.add_argument(
        "--matrix",
        action="store_true",
        help="Run short, long, ask, and tool cases sequentially for the same terminal host",
    )
    parser.add_argument(
        "--coverage-matrix",
        action="store_true",
        help=(
            "Run the four AltScreen functional cases plus TerminalOutput and forced-compat "
            "short probes for one terminal host"
        ),
    )
    parser.add_argument(
        "--terminal-host",
        choices=["windows-terminal", "classic-conhost", "other", "unspecified"],
        default="unspecified",
        help="Terminal host being manually validated; stored in the JSON report",
    )
    parser.add_argument(
        "--alt-screen-mode",
        choices=["always", "never"],
        default="always",
        help="Generated ACECode tui.alt_screen_mode",
    )
    parser.add_argument(
        "--force-compat",
        action="store_true",
        help="Set ACECODE_TUI_FORCE_CONHOST_COMPAT_LAYOUT=1",
    )
    parser.add_argument(
        "--force-osc52-copy",
        action="store_true",
        help="Set ACECODE_TUI_FORCE_OSC52_COPY=1 so right-click copy avoids the system clipboard",
    )
    parser.add_argument(
        "--print-only",
        action="store_true",
        help="Create the temp profile, print checklist and command, then exit without launching acecode",
    )
    parser.add_argument(
        "--cleanup",
        action="store_true",
        help="Delete the generated temp root after acecode exits",
    )
    parser.add_argument(
        "--report",
        type=Path,
        help=(
            "Write a JSON validation report; default: <temp-root>/manual-report.json. "
            "Pass an explicit path when using --cleanup."
        ),
    )
    parser.add_argument(
        "--report-dir",
        type=Path,
        help="Directory for matrix JSON reports; default: a generated temp report directory",
    )
    parser.add_argument(
        "--record-results",
        action="store_true",
        help="After acecode exits, prompt for pass/fail/skip/unverified results for each checklist item.",
    )
    args = parser.parse_args(argv)

    exe = args.exe.resolve()
    if not exe.exists():
        raise SystemExit(f"acecode executable not found: {exe}")
    if not executable_is_current(exe):
        raise SystemExit(
            "acecode executable is older than ChatViewport build inputs: "
            f"{exe}. Rebuild the selected configuration before manual validation."
        )

    if args.matrix and args.coverage_matrix:
        raise SystemExit("--matrix and --coverage-matrix are mutually exclusive")

    terminal_environment = detect_terminal_environment(os.environ)
    args.terminal_environment = terminal_environment
    mismatch = terminal_host_mismatch_message(args.terminal_host, terminal_environment)
    if mismatch is not None and not args.print_only:
        raise SystemExit(mismatch)

    if args.matrix:
        if args.report is not None:
            raise SystemExit("--report cannot be used with --matrix; use --report-dir")
        report_dir = args.report_dir or Path(tempfile.mkdtemp(prefix="acecode-chat-viewport-manual-reports-"))
        entries = [
            (case, case, args.alt_screen_mode, args.force_compat)
            for case in MANUAL_CASES
        ]
        return run_report_sequence(
            args,
            exe=exe,
            report_dir=report_dir,
            entries=entries,
            title="Matrix",
        )

    if args.coverage_matrix:
        if args.report is not None:
            raise SystemExit("--report cannot be used with --coverage-matrix; use --report-dir")
        report_dir = args.report_dir or Path(
            tempfile.mkdtemp(prefix="acecode-chat-viewport-coverage-reports-")
        )
        entries = [
            ("alt-short", "short", "always", False),
            ("alt-long", "long", "always", False),
            ("alt-ask", "ask", "always", False),
            ("alt-tool", "tool", "always", False),
            ("terminal-output-short", "short", "never", False),
            ("forced-compat-short", "short", "always", True),
        ]
        return run_report_sequence(
            args,
            exe=exe,
            report_dir=report_dir,
            entries=entries,
            title="Coverage matrix",
        )

    if args.report_dir is not None:
        raise SystemExit("--report-dir is only used with --matrix or --coverage-matrix")
    return run_manual_case(args, exe=exe, case=args.case, report_path=args.report)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
