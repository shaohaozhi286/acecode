#!/usr/bin/env python3
"""Evaluate ChatViewport manual validation reports against OpenSpec tasks."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any


TASKS = {
    "8.5": "Windows Terminal / ConHost / TerminalOutput / AltScreen chat box coordinates",
    "9.3": "Short session input, streaming, tail-follow, wheel, PageUp/PageDown, copy",
    "9.4": "Long session fast wheel, scrollbar top/middle/bottom, long-message row scrolling",
    "9.5": "AskUserQuestion overlay answer, transcript review, drag-select, copy",
    "9.6": "Tool result expand/collapse cache invalidation and scroll stability",
    "9.7": "Terminal resize rewrap, clamp, and tail-follow",
}

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

TASK_REQUIRED_CHECKLIST = {
    "8.5": [
        "Confirm the chat box stays visually stable",
    ],
    "9.3": [
        "Type a normal prompt",
        "While streaming at tail",
        "Scroll up, type another prompt",
        "wheel/PageUp/PageDown",
        "Right-click selected text",
    ],
    "9.4": [
        "Use fast wheel scrolling",
        "Drag the chat scrollbar to top",
        "Verify long assistant messages scroll internally",
    ],
    "9.5": [
        "Type 'ask smoke'",
        "While the overlay is open",
        "Press Return on Proceed",
        "Right-click selected text",
    ],
    "9.6": [
        "Press Ctrl+O",
        "Expanded output should appear",
        "Press Ctrl+O again",
    ],
    "9.7": [
        "Resize the terminal",
    ],
}


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


def update_tasks_file(path: Path, evaluation: dict[str, dict[str, Any]]) -> list[str]:
    incomplete = [task_id for task_id in TASKS if not evaluation[task_id]["passed"]]
    if incomplete:
        raise RuntimeError("manual coverage incomplete for tasks: " + ", ".join(incomplete))

    text = path.read_text(encoding="utf-8")
    updated: list[str] = []
    for task_id in TASKS:
        unchecked = f"- [ ] {task_id} "
        checked = f"- [x] {task_id} "
        if checked in text:
            continue
        if unchecked not in text:
            raise RuntimeError(f"{path}: task {task_id} checkbox not found")
        text = text.replace(unchecked, checked, 1)
        updated.append(task_id)

    path.write_text(text, encoding="utf-8")
    return updated


def describe_report(report: dict[str, Any]) -> str:
    log_summary = report.get("log_summary")
    latest = latest_trace(report)
    parts = [
        str(report.get("_path", "")),
        f"host={report.get('terminal_host', 'unspecified')}",
        f"detected_host={report.get('terminal_environment', {}).get('detected_host', 'unknown')}",
        f"host_match={report.get('terminal_host_matches_detection', 'unknown')}",
        f"case={report.get('case', 'unknown')}",
        f"alt_screen_mode={report.get('alt_screen_mode', 'unknown')}",
        f"force_compat={bool(report.get('force_compat'))}",
        f"source_snapshot={'ok' if source_snapshot_ok(report) else 'stale'}",
    ]
    rejection_reasons = report_rejection_reasons(report)
    if rejection_reasons:
        parts.append("rejected=" + ",".join(rejection_reasons))
    if isinstance(log_summary, dict):
        parts.append(f"trace_count={int(log_summary.get('trace_count', 0) or 0)}")
    if isinstance(latest, dict):
        parts.append(f"viewport_rows={latest.get('viewport_rows', 'unknown')}")
        parts.append(f"total_rows={latest.get('total_rows', 'unknown')}")
        parts.append(f"chat_box={latest.get('chat_box', 'unknown')}")
        parts.append(f"scrollbar_box={latest.get('scrollbar_box', 'unknown')}")
    return " | ".join(parts)


def write_evidence_file(
    path: Path,
    *,
    reports: list[dict[str, Any]],
    evaluation: dict[str, dict[str, Any]],
    complete: bool,
    updated_tasks: list[str],
    update_error: str | None,
) -> None:
    lines = [
        "# ChatViewport Manual Validation Evidence",
        "",
        f"- complete: {str(complete).lower()}",
        f"- report_count: {len(reports)}",
    ]
    if updated_tasks:
        lines.append(f"- updated_tasks: {', '.join(updated_tasks)}")
    if update_error is not None:
        lines.append(f"- update_error: {update_error}")

    lines.extend(["", "## Task Coverage", ""])
    for task_id in TASKS:
        item = evaluation[task_id]
        status = "PASS" if item["passed"] else "MISSING"
        lines.append(f"### {task_id} {status}")
        lines.append("")
        lines.append(TASKS[task_id])
        lines.append("")
        missing = item.get("missing") or []
        if missing:
            lines.append("Missing:")
            for entry in missing:
                lines.append(f"- {entry}")
            lines.append("")

        evidence = item.get("evidence") or []
        if evidence:
            lines.append("Evidence reports:")
            reports_by_path = {str(report.get("_path", "")): report for report in reports}
            for entry in evidence:
                report = reports_by_path.get(str(entry))
                if report is None:
                    lines.append(f"- {entry}")
                else:
                    lines.append(f"- {describe_report(report)}")
            lines.append("")

    lines.extend(["## All Reports", ""])
    for report in reports:
        lines.append(f"- {describe_report(report)}")

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def load_report(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise ValueError(f"{path}: report root must be an object")
    data["_path"] = str(path)
    return data


def resolve_report_paths(inputs: list[Path]) -> list[Path]:
    paths: list[Path] = []
    for item in inputs:
        if item.is_dir():
            matches = sorted(item.glob("manual-report*.json"))
            if not matches:
                raise FileNotFoundError(f"{item}: no manual-report*.json files found")
            paths.extend(matches)
        elif item.is_file():
            paths.append(item)
        else:
            raise FileNotFoundError(f"{item}: report file or directory not found")

    seen: set[Path] = set()
    unique_paths: list[Path] = []
    for path in paths:
        resolved = path.resolve()
        if resolved in seen:
            continue
        seen.add(resolved)
        unique_paths.append(path)
    return unique_paths


def status_counts(report: dict[str, Any]) -> dict[str, int]:
    counts = {"passed": 0, "failed": 0, "skipped": 0, "manual_unverified": 0}
    checklist = report.get("checklist")
    if not isinstance(checklist, list):
        counts["manual_unverified"] = 1
        return counts
    for item in checklist:
        status = str(item.get("status", "manual-unverified")) if isinstance(item, dict) else ""
        if status == "passed":
            counts["passed"] += 1
        elif status == "failed":
            counts["failed"] += 1
        elif status == "skipped":
            counts["skipped"] += 1
        else:
            counts["manual_unverified"] += 1
    return counts


def report_passed(report: dict[str, Any]) -> bool:
    if bool(report.get("print_only")):
        return False
    if report.get("returncode") != 0:
        return False
    counts = status_counts(report)
    return (
        counts["passed"] > 0
        and counts["failed"] == 0
        and counts["skipped"] == 0
        and counts["manual_unverified"] == 0
    )


def command_executable(report: dict[str, Any]) -> Path | None:
    command = report.get("command")
    if not isinstance(command, list) or not command:
        return None
    executable = command[0]
    if not isinstance(executable, str) or not executable:
        return None
    return Path(executable)


def source_snapshot_ok(report: dict[str, Any]) -> bool:
    snapshot = report.get("source_snapshot")
    if not isinstance(snapshot, dict):
        return False
    executable = command_executable(report)
    if executable is None:
        return False
    current_snapshot = build_source_snapshot(executable)
    return snapshot == current_snapshot and current_snapshot.get("executable_is_current") is True


def source_snapshot_rejection_reason(report: dict[str, Any]) -> str:
    snapshot = report.get("source_snapshot")
    if not isinstance(snapshot, dict):
        return "source-snapshot"
    executable = command_executable(report)
    if executable is None:
        return "source-snapshot"
    current_snapshot = build_source_snapshot(executable)
    if current_snapshot.get("executable_is_current") is not True:
        return "executable-build"
    if snapshot != current_snapshot:
        return "source-snapshot"
    return ""


def report_rejection_reasons(report: dict[str, Any]) -> list[str]:
    reasons: list[str] = []
    if bool(report.get("print_only")):
        reasons.append("print-only")
    if report.get("returncode") != 0:
        reasons.append("nonzero-exit")
    snapshot_reason = source_snapshot_rejection_reason(report)
    if snapshot_reason:
        reasons.append(snapshot_reason)
    if not has_sane_trace(report):
        reasons.append("trace")
    if not host_detection_ok(report):
        reasons.append("host-detection")
    return reasons


def latest_trace(report: dict[str, Any]) -> dict[str, Any] | None:
    log_summary = report.get("log_summary")
    if not isinstance(log_summary, dict):
        return None
    latest = log_summary.get("latest_trace")
    return latest if isinstance(latest, dict) else None


def has_sane_trace(report: dict[str, Any]) -> bool:
    log_summary = report.get("log_summary")
    if isinstance(log_summary, dict) and log_summary.get("trace_boxes_sane") is False:
        return False
    trace = latest_trace(report)
    if trace is None:
        return False
    chat_box = trace.get("chat_box")
    scrollbar_box = trace.get("scrollbar_box")
    viewport_rows = int(trace.get("viewport_rows", 0) or 0)
    total_rows = int(trace.get("total_rows", 0) or 0)
    if not isinstance(chat_box, list) or len(chat_box) != 4:
        return False
    if not isinstance(scrollbar_box, list) or len(scrollbar_box) != 4:
        return False
    return (
        viewport_rows > 0
        and total_rows >= 0
        and int(chat_box[2]) >= int(chat_box[0])
        and int(chat_box[3]) >= int(chat_box[1])
        and int(scrollbar_box[2]) >= int(scrollbar_box[0])
        and int(scrollbar_box[3]) >= int(scrollbar_box[1])
    )


def host_detection_ok(report: dict[str, Any]) -> bool:
    if report.get("terminal_host_matches_detection") is not True:
        return False

    environment = report.get("terminal_environment")
    if not isinstance(environment, dict):
        return False

    detected_host = str(environment.get("detected_host", "unknown"))
    requested_host = str(report.get("terminal_host", "unspecified"))
    if requested_host == "windows-terminal":
        return detected_host == "windows-terminal"
    if requested_host == "classic-conhost":
        return detected_host == "classic-conhost"
    return detected_host != "unknown"


def has_tui_mode(report: dict[str, Any], *, mode: str | None = None, compat: str | None = None) -> bool:
    log_summary = report.get("log_summary")
    if not isinstance(log_summary, dict):
        return False
    modes = log_summary.get("tui_modes")
    if not isinstance(modes, list):
        return False
    for item in modes:
        if not isinstance(item, dict):
            continue
        if mode is not None and item.get("mode") != mode:
            continue
        if compat is not None and item.get("compat") != compat:
            continue
        return True
    return False


def has_ftxui_mode(report: dict[str, Any], *, alt: str, terminal: str) -> bool:
    log_summary = report.get("log_summary")
    if not isinstance(log_summary, dict):
        return False
    modes = log_summary.get("ftxui_modes")
    if not isinstance(modes, list):
        return False
    for item in modes:
        if isinstance(item, dict) and item.get("alt") == alt and item.get("terminal") == terminal:
            return True
    return False


def checklist_item_passed(report: dict[str, Any], needle: str) -> bool:
    checklist = report.get("checklist")
    if not isinstance(checklist, list):
        return False
    needle_lower = needle.lower()
    for item in checklist:
        if not isinstance(item, dict):
            continue
        text = str(item.get("item", "")).lower()
        if needle_lower in text and item.get("status") == "passed":
            return True
    return False


def missing_required_checklist_items(report: dict[str, Any], needles: list[str]) -> list[str]:
    return [needle for needle in needles if not checklist_item_passed(report, needle)]


def has_required_checklist_items(report: dict[str, Any], task_id: str) -> bool:
    return not missing_required_checklist_items(report, TASK_REQUIRED_CHECKLIST[task_id])


def eligible_reports(reports: list[dict[str, Any]], *, case: str | None = None) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for report in reports:
        if case is not None and report.get("case") != case:
            continue
        if (
            report_passed(report)
            and source_snapshot_ok(report)
            and has_sane_trace(report)
            and host_detection_ok(report)
        ):
            out.append(report)
    return out


def eligible_reports_for_task(
    reports: list[dict[str, Any]],
    *,
    task_id: str,
    case: str | None = None,
) -> list[dict[str, Any]]:
    return [
        report
        for report in eligible_reports(reports, case=case)
        if has_required_checklist_items(report, task_id)
    ]


def rejection_reason_label(reason: str) -> str:
    labels = {
        "print-only": "non-print-only run",
        "nonzero-exit": "zero exit code",
        "source-snapshot": "current source/executable snapshot match",
        "executable-build": "executable rebuilt after ChatViewport source changes",
        "trace": "sane chat viewport trace",
        "host-detection": "terminal host detection match",
    }
    return labels.get(reason, reason)


def missing_task_report_reason(reports: list[dict[str, Any]], *, task_id: str, case: str) -> list[str]:
    case_reports = [report for report in reports if report.get("case") == case]
    passed_reports = [report for report in case_reports if report_passed(report)]
    if not passed_reports:
        return [f"passed {case} report"]

    rejected_reasons = sorted(
        {
            reason
            for report in passed_reports
            for reason in report_rejection_reasons(report)
        }
    )
    if rejected_reasons:
        return [
            f"passed {case} report with {rejection_reason_label(reason)}"
            for reason in rejected_reasons
        ]

    candidates = eligible_reports(reports, case=case)
    if not candidates:
        return [f"passed {case} report"]

    missing_items: list[str] = []
    for report in candidates:
        missing_items.extend(missing_required_checklist_items(report, TASK_REQUIRED_CHECKLIST[task_id]))
    unique_missing = sorted(set(missing_items))
    return [f"passed {case} report with checklist item: {item}" for item in unique_missing]


def evaluate(reports: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}

    passed_with_trace = [
        report
        for report in reports
        if report_passed(report) and source_snapshot_ok(report) and has_sane_trace(report)
    ]
    rejected_host_detection = [report for report in passed_with_trace if not host_detection_ok(report)]
    passed_sane = [
        report
        for report in passed_with_trace
        if host_detection_ok(report) and has_required_checklist_items(report, "8.5")
    ]
    missing_85: list[str] = []
    for host in ["windows-terminal", "classic-conhost"]:
        host_reports = [
            report for report in passed_sane if str(report.get("terminal_host", "unspecified")) == host
        ]
        if not host_reports:
            missing_85.append(f"{host} report")
            continue
        if not any(has_ftxui_mode(report, alt="1", terminal="0") for report in host_reports):
            missing_85.append(f"{host} AltScreen FTXUI mode")
        if not any(has_ftxui_mode(report, alt="0", terminal="1") for report in host_reports):
            missing_85.append(f"{host} TerminalOutput FTXUI mode")

    has_classic_compat = any(
        str(report.get("terminal_host", "unspecified")) == "classic-conhost"
        and report.get("force_compat")
        and has_tui_mode(report, mode="AltScreen", compat="1")
        for report in passed_sane
    )
    if not has_classic_compat:
        missing_85.append("classic-conhost forced compatibility layout")
    if missing_85 and rejected_host_detection:
        missing_85.append("terminal host detection match")
    rejected_snapshot_reports = [
        report
        for report in reports
        if report_passed(report) and not source_snapshot_ok(report)
    ]
    if missing_85 and rejected_snapshot_reports:
        missing_85.append("current source/executable snapshot match")
    stable_trace_reports = eligible_reports_for_task(reports, task_id="8.5")
    if passed_with_trace and not stable_trace_reports:
        missing_85.append("passed chat-box stability checklist item")
    result["8.5"] = {
        "passed": not missing_85,
        "missing": missing_85,
        "evidence": [report["_path"] for report in passed_sane],
    }

    short_reports = [
        report
        for report in eligible_reports_for_task(reports, task_id="9.3", case="short")
        if int(report.get("fake_openai_request_count", 0) or 0) >= 1
    ]
    result["9.3"] = {
        "passed": bool(short_reports),
        "missing": []
        if short_reports
        else missing_task_report_reason(reports, task_id="9.3", case="short")
        + ["short report with >=1 fake OpenAI request"],
        "evidence": [report["_path"] for report in short_reports],
    }

    long_reports = eligible_reports_for_task(reports, task_id="9.4", case="long")
    result["9.4"] = {
        "passed": bool(long_reports),
        "missing": [] if long_reports else missing_task_report_reason(reports, task_id="9.4", case="long"),
        "evidence": [report["_path"] for report in long_reports],
    }

    ask_reports = [
        report for report in eligible_reports_for_task(reports, task_id="9.5", case="ask")
        if int(report.get("fake_openai_request_count", 0) or 0) >= 2
        and bool(report.get("log_summary", {}).get("saw_ask_final_text"))
    ]
    result["9.5"] = {
        "passed": bool(ask_reports),
        "missing": []
        if ask_reports
        else missing_task_report_reason(reports, task_id="9.5", case="ask")
        + ["Ask final text and >=2 fake OpenAI requests"],
        "evidence": [report["_path"] for report in ask_reports],
    }

    tool_reports = eligible_reports_for_task(reports, task_id="9.6", case="tool")
    result["9.6"] = {
        "passed": bool(tool_reports),
        "missing": [] if tool_reports else missing_task_report_reason(reports, task_id="9.6", case="tool"),
        "evidence": [report["_path"] for report in tool_reports],
    }

    resize_reports = [
        report for report in eligible_reports_for_task(reports, task_id="9.7")
    ]
    result["9.7"] = {
        "passed": bool(resize_reports),
        "missing": [] if resize_reports else ["passed resize checklist item in at least one report"],
        "evidence": [report["_path"] for report in resize_reports],
    }

    return result


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "reports",
        nargs="+",
        type=Path,
        help="manual-report.json files or directories containing manual-report*.json",
    )
    parser.add_argument("--json", action="store_true", help="Print machine-readable JSON")
    parser.add_argument("--strict", action="store_true", help="Exit non-zero unless every mapped task passes")
    parser.add_argument(
        "--update-tasks",
        type=Path,
        help="Mark mapped OpenSpec manual validation tasks complete when every mapped task passes",
    )
    parser.add_argument(
        "--write-evidence",
        type=Path,
        help="Write a markdown evidence summary for the evaluated manual reports",
    )
    args = parser.parse_args(argv)

    report_paths = resolve_report_paths(args.reports)
    reports = [load_report(path) for path in report_paths]
    evaluation = evaluate(reports)
    complete = all(item["passed"] for item in evaluation.values())
    updated_tasks: list[str] = []
    update_error: str | None = None
    if args.update_tasks is not None:
        try:
            updated_tasks = update_tasks_file(args.update_tasks, evaluation)
        except RuntimeError as exc:
            update_error = str(exc)
    evidence_error: str | None = None
    if args.write_evidence is not None:
        try:
            write_evidence_file(
                args.write_evidence,
                reports=reports,
                evaluation=evaluation,
                complete=complete,
                updated_tasks=updated_tasks,
                update_error=update_error,
            )
        except OSError as exc:
            evidence_error = str(exc)

    if args.json:
        print(
            json.dumps(
                {
                    "complete": complete,
                    "tasks": evaluation,
                    "updated_tasks": updated_tasks,
                    "update_error": update_error,
                    "evidence_path": str(args.write_evidence) if args.write_evidence else None,
                    "evidence_error": evidence_error,
                },
                ensure_ascii=False,
                indent=2,
            )
        )
    else:
        print(f"Manual report coverage: {'complete' if complete else 'incomplete'}")
        for task_id in TASKS:
            item = evaluation[task_id]
            status = "PASS" if item["passed"] else "MISSING"
            print(f"{task_id} {status}: {TASKS[task_id]}")
            if item["missing"]:
                print("  missing: " + "; ".join(item["missing"]))
            if item["evidence"]:
                print("  evidence: " + "; ".join(item["evidence"]))
        if updated_tasks:
            print("Updated OpenSpec tasks: " + ", ".join(updated_tasks))
        if args.write_evidence is not None and evidence_error is None:
            print("Wrote evidence: " + str(args.write_evidence))
        if update_error is not None:
            print("Did not update OpenSpec tasks: " + update_error, file=sys.stderr)
        if evidence_error is not None:
            print("Did not write evidence: " + evidence_error, file=sys.stderr)

    if update_error is not None or evidence_error is not None:
        return 1
    return 1 if args.strict and not complete else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
