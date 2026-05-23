#!/usr/bin/env python3
"""Check ChatViewport manual-validation and legacy-scroll cleanup status."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


MANUAL_TASKS = ["8.5", "9.3", "9.4", "9.5", "9.6", "9.7"]
CLEANUP_TASKS = ["10.1", "10.2"]
LEGACY_PATTERNS = {
    "legacy_state_struct": "LegacyChatScrollLayoutState",
    "legacy_state_variable": "legacy_chat_scroll",
    "legacy_line_counts": "message_line_counts",
    "legacy_message_boxes": "message_boxes",
    "legacy_message_layout_boxes": "message_layout_boxes",
    "legacy_focus_position": "focusPosition(0, frame_focus_y)",
    "legacy_yframe_chat": "| yframe | reflect(chat_box)",
}


def task_checked(tasks_text: str, task_id: str) -> bool:
    pattern = re.compile(rf"^- \[x\] {re.escape(task_id)}\s", re.MULTILINE)
    return bool(pattern.search(tasks_text))


def evidence_complete(evidence_text: str) -> bool:
    if "- complete: true" not in evidence_text:
        return False
    return all(f"### {task_id} PASS" in evidence_text for task_id in MANUAL_TASKS)


def scan_legacy_patterns(source_text: str) -> dict[str, int]:
    return {
        name: source_text.count(pattern)
        for name, pattern in LEGACY_PATTERNS.items()
    }


def evaluate(tasks_path: Path, evidence_path: Path, source_path: Path) -> dict[str, Any]:
    tasks_text = tasks_path.read_text(encoding="utf-8")
    evidence_text = evidence_path.read_text(encoding="utf-8") if evidence_path.exists() else ""
    source_text = source_path.read_text(encoding="utf-8", errors="replace")

    manual_task_status = {task_id: task_checked(tasks_text, task_id) for task_id in MANUAL_TASKS}
    cleanup_task_status = {task_id: task_checked(tasks_text, task_id) for task_id in CLEANUP_TASKS}
    legacy_hits = scan_legacy_patterns(source_text)
    manual_complete = all(manual_task_status.values()) and evidence_complete(evidence_text)
    legacy_clean = all(count == 0 for count in legacy_hits.values())
    cleanup_tasks_complete = all(cleanup_task_status.values())

    return {
        "manual_complete": manual_complete,
        "manual_tasks": manual_task_status,
        "evidence_complete": evidence_complete(evidence_text),
        "cleanup_complete": cleanup_tasks_complete and legacy_clean,
        "cleanup_tasks": cleanup_task_status,
        "legacy_clean": legacy_clean,
        "legacy_hits": legacy_hits,
        "tasks_path": str(tasks_path),
        "evidence_path": str(evidence_path),
        "source_path": str(source_path),
    }


def print_text(result: dict[str, Any]) -> None:
    manual_status = "complete" if result["manual_complete"] else "incomplete"
    cleanup_status = "complete" if result["cleanup_complete"] else "incomplete"
    print(f"Manual validation: {manual_status}")
    print(f"Legacy cleanup: {cleanup_status}")
    print(f"Evidence complete: {str(result['evidence_complete']).lower()}")

    missing_manual = [task_id for task_id, ok in result["manual_tasks"].items() if not ok]
    if missing_manual:
        print("Missing manual task checkboxes: " + ", ".join(missing_manual))

    missing_cleanup = [task_id for task_id, ok in result["cleanup_tasks"].items() if not ok]
    if missing_cleanup:
        print("Missing cleanup task checkboxes: " + ", ".join(missing_cleanup))

    legacy_hits = {name: count for name, count in result["legacy_hits"].items() if count}
    if legacy_hits:
        print("Legacy source markers still present:")
        for name, count in legacy_hits.items():
            print(f"  {name}: {count}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--tasks",
        type=Path,
        default=Path("openspec") / "changes" / "add-ftxui-chat-viewport" / "tasks.md",
        help="OpenSpec tasks.md path",
    )
    parser.add_argument(
        "--evidence",
        type=Path,
        default=Path("openspec") / "changes" / "add-ftxui-chat-viewport" / "manual-validation-evidence.md",
        help="Manual validation evidence markdown path",
    )
    parser.add_argument(
        "--source",
        type=Path,
        default=Path("main.cpp"),
        help="Source file to scan for legacy chat-scroll markers",
    )
    parser.add_argument("--json", action="store_true", help="Print machine-readable JSON")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Exit non-zero unless manual validation and legacy cleanup are both complete",
    )
    args = parser.parse_args(argv)

    result = evaluate(args.tasks, args.evidence, args.source)
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        print_text(result)

    if args.strict and not (result["manual_complete"] and result["cleanup_complete"]):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
