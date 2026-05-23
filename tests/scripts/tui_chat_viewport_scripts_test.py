#!/usr/bin/env python3
"""Unit tests for ChatViewport validation helper scripts."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import tui_chat_viewport_cleanup_check as cleanup_check  # noqa: E402
import tui_chat_viewport_manual as manual_check  # noqa: E402
import tui_chat_viewport_report as report_check  # noqa: E402


COMMON_CHECKLIST_ITEMS = [
    "Confirm the chat box stays visually stable after several redraws.",
    "Confirm the transcript is rendered and wheel/PageUp/PageDown move by rows.",
    "Drag-select transcript text; selection should remain stable while scrolling.",
    "Right-click selected text and confirm copy status appears.",
    "Resize the terminal; transcript should rewrap, clamp, and keep tail-follow when pinned.",
]


CASE_CHECKLIST_ITEMS = {
    "short": [
        "Type a normal prompt; fake OpenAI should stream three chunks.",
        "While streaming at tail, output should remain pinned to the newest row.",
        "Scroll up, type another prompt, and confirm review position is not stolen unless tail is pinned.",
    ],
    "long": [
        "Use fast wheel scrolling through the seeded long transcript.",
        "Drag the chat scrollbar to top, middle, and bottom.",
        "Verify long assistant messages scroll internally by display row, not by whole message only.",
    ],
    "ask": [
        "Type 'ask smoke' to trigger the fake OpenAI AskUserQuestion tool call.",
        "While the overlay is open, wheel/PageUp over transcript and drag-select text behind it.",
        "Press Return on Proceed; final text should become 'Ask smoke complete.'.",
    ],
    "tool": [
        "Press Ctrl+O on the focused summarized tool result.",
        "Expanded output should appear without jumping to the wrong scroll position.",
        "Press Ctrl+O again and verify the summary row returns.",
    ],
}


def checklist_items_for_case(case: str, omit_needles: set[str] | None = None) -> list[str]:
    omit_needles = omit_needles or set()
    items = [*CASE_CHECKLIST_ITEMS[case], *COMMON_CHECKLIST_ITEMS]
    return [
        item
        for item in items
        if not any(needle.lower() in item.lower() for needle in omit_needles)
    ]


def make_report(
    path: Path,
    *,
    host: str = "windows-terminal",
    detected_host: str | None = None,
    case: str = "short",
    alt: bool = True,
    force_compat: bool = False,
    ask: bool = False,
    passed: bool = True,
    skipped: bool = False,
    omit_checklist_needles: set[str] | None = None,
    print_only: bool = False,
    returncode: int | None = 0,
    fake_openai_request_count: int | None = None,
    include_source_snapshot: bool = True,
    source_snapshot: dict[str, object] | None = None,
) -> None:
    status = "skipped" if skipped else ("passed" if passed else "manual-unverified")
    detected_host = detected_host or host
    checklist_items = checklist_items_for_case(case, omit_checklist_needles)
    exe = path.parent / "acecode.exe"
    exe.touch()
    data = {
        "case": case,
        "terminal_host": host,
        "terminal_environment": {
            "detected_host": detected_host,
            "signals": {
                "WT_SESSION": host == "windows-terminal",
                "ConEmuPID": False,
                "TERM_PROGRAM": "",
                "TERM": "",
                "SESSIONNAME": "",
            },
        },
        "terminal_host_matches_detection": True,
        "alt_screen_mode": "always" if alt else "never",
        "force_compat": force_compat,
        "print_only": print_only,
        "returncode": returncode,
        "command": [str(exe)],
        "fake_openai_request_count": fake_openai_request_count
        if fake_openai_request_count is not None
        else (2 if ask else 1),
        "checklist_summary": {
            "passed": 0 if skipped else (len(checklist_items) if passed else 0),
            "failed": 0,
            "skipped": len(checklist_items) if skipped else 0,
            "manual_unverified": 0 if passed else len(checklist_items),
        },
        "checklist": [
            {"item": item, "status": status}
            for item in checklist_items
        ],
        "log_summary": {
            "trace_count": 4,
            "trace_boxes_sane": True,
            "latest_trace": {
                "top": 0,
                "viewport_rows": 10,
                "total_rows": 20,
                "chat_box": [0, 1, 79, 20],
                "scrollbar_box": [80, 1, 80, 20],
            },
            "ftxui_modes": [
                {"alt": "1" if alt else "0", "terminal": "0" if alt else "1"}
            ],
            "tui_modes": [
                {
                    "mode": "AltScreen" if alt else "TerminalOutput",
                    "compat": "1" if force_compat else "0",
                }
            ],
            "saw_ask_final_text": ask,
        },
    }
    if include_source_snapshot:
        data["source_snapshot"] = source_snapshot or report_check.build_source_snapshot(exe)
    path.write_text(json.dumps(data), encoding="utf-8")


def make_complete_report_dir(root: Path) -> None:
    make_report(root / "manual-report-wt-alt-short.json", case="short")
    make_report(root / "manual-report-wt-alt-long.json", case="long")
    make_report(root / "manual-report-wt-alt-ask.json", case="ask", ask=True)
    make_report(root / "manual-report-wt-alt-tool.json", case="tool")
    make_report(root / "manual-report-wt-terminal-output.json", case="short", alt=False)
    make_report(root / "manual-report-wt-forced-compat.json", case="short", force_compat=True)
    make_report(
        root / "manual-report-conhost-alt-short.json",
        host="classic-conhost",
        detected_host="classic-conhost",
        case="short",
    )
    make_report(
        root / "manual-report-conhost-terminal-output.json",
        host="classic-conhost",
        detected_host="classic-conhost",
        case="short",
        alt=False,
    )
    make_report(
        root / "manual-report-conhost-forced-compat.json",
        host="classic-conhost",
        detected_host="classic-conhost",
        case="short",
        force_compat=True,
    )


def make_tasks_text(*, checked: set[str] | None = None) -> str:
    checked = checked or set()
    rows = []
    for task_id in ["8.5", "9.3", "9.4", "9.5", "9.6", "9.7", "10.1", "10.2"]:
        mark = "x" if task_id in checked else " "
        rows.append(f"- [{mark}] {task_id} task\n")
    return "".join(rows)


def powershell_executable() -> str | None:
    return shutil.which("powershell") or shutil.which("pwsh")


def run_validation_wrapper(
    *args: str,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    powershell = powershell_executable()
    if powershell is None:
        raise unittest.SkipTest("PowerShell is not available")
    command = [powershell, "-NoProfile"]
    if Path(powershell).name.lower() == "powershell.exe":
        command.extend(["-ExecutionPolicy", "Bypass"])
    command.extend(["-File", str(REPO_ROOT / "scripts" / "tui_chat_viewport_validation.ps1")])
    command.extend(args)
    return subprocess.run(
        command,
        cwd=REPO_ROOT,
        env=env,
        text=True,
        capture_output=True,
        check=False,
        timeout=60,
    )


class ChatViewportReportScriptTest(unittest.TestCase):
    def test_evaluate_directory_and_update_tasks(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            make_complete_report_dir(root)

            paths = report_check.resolve_report_paths([root, root / "manual-report-wt-alt-short.json"])
            self.assertEqual(9, len(paths))

            reports = [report_check.load_report(path) for path in paths]
            evaluation = report_check.evaluate(reports)
            self.assertTrue(all(item["passed"] for item in evaluation.values()))

            tasks_path = root / "tasks.md"
            tasks_path.write_text(make_tasks_text(), encoding="utf-8")
            updated = report_check.update_tasks_file(tasks_path, evaluation)
            self.assertEqual(["8.5", "9.3", "9.4", "9.5", "9.6", "9.7"], updated)

            tasks_text = tasks_path.read_text(encoding="utf-8")
            for task_id in updated:
                self.assertIn(f"- [x] {task_id} ", tasks_text)
            self.assertIn("- [ ] 10.1 ", tasks_text)
            self.assertIn("- [ ] 10.2 ", tasks_text)

            evidence_path = root / "evidence.md"
            report_check.write_evidence_file(
                evidence_path,
                reports=reports,
                evaluation=evaluation,
                complete=True,
                updated_tasks=updated,
                update_error=None,
            )
            evidence = evidence_path.read_text(encoding="utf-8")
            self.assertIn("- complete: true", evidence)
            self.assertIn("### 8.5 PASS", evidence)
            self.assertIn("manual-report-wt-alt-short.json", evidence)
            self.assertIn("detected_host=windows-terminal", evidence)
            self.assertIn("host_match=True", evidence)
            self.assertIn("source_snapshot=ok", evidence)
            self.assertIn("trace_count=4", evidence)
            self.assertIn("chat_box=[0, 1, 79, 20]", evidence)

    def test_evaluate_requires_85_modes_per_terminal_host(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            make_complete_report_dir(root)
            (root / "manual-report-conhost-terminal-output.json").unlink()

            reports = [report_check.load_report(path) for path in report_check.resolve_report_paths([root])]
            evaluation = report_check.evaluate(reports)

            self.assertFalse(evaluation["8.5"]["passed"])
            self.assertIn("classic-conhost TerminalOutput FTXUI mode", evaluation["8.5"]["missing"])

    def test_evaluate_requires_classic_conhost_forced_compat(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            make_complete_report_dir(root)
            (root / "manual-report-conhost-forced-compat.json").unlink()

            reports = [report_check.load_report(path) for path in report_check.resolve_report_paths([root])]
            evaluation = report_check.evaluate(reports)

            self.assertFalse(evaluation["8.5"]["passed"])
            self.assertIn(
                "classic-conhost forced compatibility layout",
                evaluation["8.5"]["missing"],
            )

    def test_update_tasks_refuses_incomplete_coverage(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            make_report(root / "manual-report-alt-short.json", passed=False)
            reports = [report_check.load_report(root / "manual-report-alt-short.json")]
            evaluation = report_check.evaluate(reports)
            self.assertFalse(all(item["passed"] for item in evaluation.values()))

            tasks_path = root / "tasks.md"
            original = make_tasks_text()
            tasks_path.write_text(original, encoding="utf-8")
            with self.assertRaises(RuntimeError):
                report_check.update_tasks_file(tasks_path, evaluation)
            self.assertEqual(original, tasks_path.read_text(encoding="utf-8"))

    def test_evaluate_rejects_terminal_host_detection_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            report_path = root / "manual-report-wt-alt-short.json"
            make_report(report_path, case="short")
            data = json.loads(report_path.read_text(encoding="utf-8"))
            data["terminal_environment"]["detected_host"] = "classic-conhost"
            data["terminal_host_matches_detection"] = False
            report_path.write_text(json.dumps(data), encoding="utf-8")

            reports = [report_check.load_report(report_path)]
            self.assertFalse(report_check.host_detection_ok(reports[0]))

            evaluation = report_check.evaluate(reports)
            self.assertFalse(evaluation["8.5"]["passed"])
            self.assertFalse(evaluation["9.3"]["passed"])
            self.assertIn("terminal host detection match", evaluation["8.5"]["missing"])
            self.assertEqual([], evaluation["9.3"]["evidence"])

    def test_evaluate_rejects_skipped_manual_items_as_incomplete(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            report_path = root / "manual-report-wt-alt-short.json"
            make_report(report_path, case="short", skipped=True)

            reports = [report_check.load_report(report_path)]
            self.assertFalse(report_check.report_passed(reports[0]))

            evaluation = report_check.evaluate(reports)
            self.assertFalse(evaluation["9.3"]["passed"])
            self.assertEqual([], evaluation["9.3"]["evidence"])

    def test_evaluate_rejects_report_missing_required_task_checklist_item(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            report_path = root / "manual-report-wt-alt-short.json"
            make_report(
                report_path,
                case="short",
                omit_checklist_needles={"Type a normal prompt"},
            )

            reports = [report_check.load_report(report_path)]
            self.assertTrue(report_check.report_passed(reports[0]))

            evaluation = report_check.evaluate(reports)
            self.assertFalse(evaluation["9.3"]["passed"])
            self.assertEqual([], evaluation["9.3"]["evidence"])
            self.assertIn(
                "passed short report with checklist item: Type a normal prompt",
                evaluation["9.3"]["missing"],
            )

    def test_report_passed_uses_checklist_not_stale_summary(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            report_path = root / "manual-report-wt-alt-short.json"
            make_report(report_path, case="short")
            data = json.loads(report_path.read_text(encoding="utf-8"))
            data["checklist_summary"] = {
                "passed": len(data["checklist"]),
                "failed": 0,
                "skipped": 0,
                "manual_unverified": 0,
            }
            data["checklist"][0]["status"] = "failed"
            report_path.write_text(json.dumps(data), encoding="utf-8")

            report = report_check.load_report(report_path)
            self.assertEqual(1, report_check.status_counts(report)["failed"])
            self.assertFalse(report_check.report_passed(report))

    def test_report_passed_rejects_print_only_and_nonzero_exit(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            print_only_report = root / "manual-report-print-only.json"
            make_report(print_only_report, case="short", print_only=True)
            self.assertFalse(report_check.report_passed(report_check.load_report(print_only_report)))

            failed_report = root / "manual-report-failed.json"
            make_report(failed_report, case="short", returncode=7)
            self.assertFalse(report_check.report_passed(report_check.load_report(failed_report)))

    def test_evaluate_rejects_missing_or_stale_source_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            missing_snapshot_report = root / "manual-report-missing-snapshot.json"
            make_report(missing_snapshot_report, case="short", include_source_snapshot=False)
            missing_report = report_check.load_report(missing_snapshot_report)
            self.assertTrue(report_check.report_passed(missing_report))
            self.assertFalse(report_check.source_snapshot_ok(missing_report))

            stale_snapshot_report = root / "manual-report-stale-snapshot.json"
            stale_exe = root / "acecode.exe"
            stale_snapshot = report_check.build_source_snapshot(stale_exe)
            stale_snapshot["version"] = 0
            make_report(stale_snapshot_report, case="short", source_snapshot=stale_snapshot)
            stale_report = report_check.load_report(stale_snapshot_report)
            self.assertFalse(report_check.source_snapshot_ok(stale_report))

            evaluation = report_check.evaluate([missing_report, stale_report])
            self.assertFalse(evaluation["9.3"]["passed"])
            self.assertEqual([], evaluation["9.3"]["evidence"])
            self.assertIn(
                "passed short report with current source/executable snapshot match",
                evaluation["9.3"]["missing"],
            )
            self.assertIn(
                "source_snapshot=stale",
                report_check.describe_report(stale_report),
            )
            self.assertIn(
                "rejected=source-snapshot",
                report_check.describe_report(stale_report),
            )

    def test_evaluate_rejects_executable_older_than_build_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            report_path = root / "manual-report-stale-exe.json"
            make_report(report_path, case="short")
            report = report_check.load_report(report_path)
            exe = report_check.command_executable(report)
            self.assertIsNotNone(exe)
            stale_time = 1
            os.utime(str(exe), (stale_time, stale_time))

            self.assertFalse(report_check.source_snapshot_ok(report))
            self.assertIn("executable-build", report_check.report_rejection_reasons(report))

            evaluation = report_check.evaluate([report])
            self.assertFalse(evaluation["9.3"]["passed"])
            self.assertIn(
                "passed short report with executable rebuilt after ChatViewport source changes",
                evaluation["9.3"]["missing"],
            )

    def test_evaluate_short_session_requires_fake_openai_request(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            report_path = root / "manual-report-wt-alt-short.json"
            make_report(report_path, case="short", fake_openai_request_count=0)

            reports = [report_check.load_report(report_path)]
            evaluation = report_check.evaluate(reports)
            self.assertFalse(evaluation["9.3"]["passed"])
            self.assertIn(
                "short report with >=1 fake OpenAI request",
                evaluation["9.3"]["missing"],
            )

    def test_host_detection_accepts_classic_conhost(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            report_path = root / "manual-report-conhost-alt-short.json"
            make_report(
                report_path,
                host="classic-conhost",
                detected_host="classic-conhost",
                case="short",
            )

            report = report_check.load_report(report_path)
            self.assertTrue(report_check.host_detection_ok(report))

    def test_host_detection_rejects_other_host_for_classic_conhost(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            report_path = root / "manual-report-conhost-alt-short.json"
            make_report(
                report_path,
                host="classic-conhost",
                detected_host="other",
                case="short",
            )

            report = report_check.load_report(report_path)
            self.assertFalse(report_check.host_detection_ok(report))

    def test_cleanup_check_requires_evidence_tasks_and_no_legacy_markers(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            all_tasks = set(cleanup_check.MANUAL_TASKS + cleanup_check.CLEANUP_TASKS)
            tasks_path = root / "tasks.md"
            tasks_path.write_text(make_tasks_text(checked=all_tasks), encoding="utf-8")
            evidence_path = root / "evidence.md"
            evidence_path.write_text(
                "# Evidence\n\n- complete: true\n\n"
                + "\n".join(f"### {task_id} PASS" for task_id in cleanup_check.MANUAL_TASKS)
                + "\n",
                encoding="utf-8",
            )
            source_path = root / "main.cpp"
            source_path.write_text("int main() { return 0; }\n", encoding="utf-8")

            result = cleanup_check.evaluate(tasks_path, evidence_path, source_path)
            self.assertTrue(result["manual_complete"])
            self.assertTrue(result["cleanup_complete"])

            source_path.write_text("auto x = legacy_chat_scroll;\n", encoding="utf-8")
            result = cleanup_check.evaluate(tasks_path, evidence_path, source_path)
            self.assertFalse(result["legacy_clean"])
            self.assertFalse(result["cleanup_complete"])
            self.assertEqual(1, result["legacy_hits"]["legacy_state_variable"])

    def test_terminal_environment_detection_flags_windows_terminal(self) -> None:
        env = {"WT_SESSION": "abc", "TERM": "xterm-256color"}
        detected = manual_check.detect_terminal_environment(env)
        self.assertEqual("windows-terminal", detected["detected_host"])
        self.assertTrue(detected["signals"]["WT_SESSION"])
        self.assertTrue(
            manual_check.terminal_host_matches_requested(
                "windows-terminal",
                str(detected["detected_host"]),
            )
        )
        self.assertFalse(
            manual_check.terminal_host_matches_requested(
                "classic-conhost",
                str(detected["detected_host"]),
            )
        )

    def test_terminal_host_mismatch_message_is_actionable(self) -> None:
        detected = {
            "detected_host": "classic-conhost",
            "signals": {"WT_SESSION": False},
        }
        message = manual_check.terminal_host_mismatch_message("windows-terminal", detected)
        self.assertIsNotNone(message)
        self.assertIn("requested terminal host 'windows-terminal'", str(message))
        self.assertIn("detected host 'classic-conhost'", str(message))

        self.assertIsNone(
            manual_check.terminal_host_mismatch_message("classic-conhost", detected)
        )

    def test_terminal_environment_detection_flags_other_windows_hosts(self) -> None:
        detected = manual_check.detect_terminal_environment(
            {
                "ConEmuPID": "1234",
                "TERM": "xterm-256color",
                "TERM_PROGRAM": "vscode",
            }
        )
        self.assertEqual("other", detected["detected_host"])
        self.assertFalse(
            manual_check.terminal_host_matches_requested(
                "classic-conhost",
                str(detected["detected_host"]),
            )
        )

    def test_manual_launcher_refuses_host_mismatch_before_launch(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            fake_exe = root / "acecode.exe"
            report_path = root / "manual-report.json"
            fake_exe.touch()

            env = os.environ.copy()
            env.pop("WT_SESSION", None)
            completed = subprocess.run(
                [
                    sys.executable,
                    str(REPO_ROOT / "scripts" / "tui_chat_viewport_manual.py"),
                    "--exe",
                    str(fake_exe),
                    "--case",
                    "short",
                    "--terminal-host",
                    "windows-terminal",
                    "--report",
                    str(report_path),
                ],
                cwd=REPO_ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=False,
                timeout=30,
            )

            self.assertNotEqual(completed.returncode, 0)
            combined_output = completed.stdout + completed.stderr
            self.assertIn("requested terminal host 'windows-terminal'", combined_output)
            self.assertFalse(report_path.exists())

    def test_validation_wrapper_prints_expected_commands(self) -> None:
        completed = run_validation_wrapper("-Mode", "PrintCommands")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("-Mode CheckHost -TerminalHost windows-terminal", completed.stdout)
        self.assertIn("-Mode RunHost -TerminalHost windows-terminal", completed.stdout)
        self.assertIn("-Mode CheckHost -TerminalHost classic-conhost", completed.stdout)
        self.assertIn("-Mode RunHost -TerminalHost classic-conhost", completed.stdout)
        self.assertIn("-Mode Report -UpdateTasks -WriteEvidence", completed.stdout)
        self.assertIn("-Mode Preflight", completed.stdout)
        self.assertIn("-Mode CleanupCheck", completed.stdout)

    def test_validation_wrapper_checkhost_reports_detected_host(self) -> None:
        env = os.environ.copy()
        env.pop("WT_SESSION", None)
        completed = run_validation_wrapper(
            "-Mode",
            "CheckHost",
            "-TerminalHost",
            "unspecified",
            env=env,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("Requested terminal host: unspecified", completed.stdout)
        self.assertIn("Detected terminal host:", completed.stdout)

    def test_validation_wrapper_checkhost_rejects_mismatch(self) -> None:
        env = os.environ.copy()
        env.pop("WT_SESSION", None)
        completed = run_validation_wrapper(
            "-Mode",
            "CheckHost",
            "-TerminalHost",
            "windows-terminal",
            env=env,
        )
        self.assertNotEqual(completed.returncode, 0)
        combined_output = completed.stdout + completed.stderr
        self.assertIn("Requested terminal host: windows-terminal", combined_output)
        self.assertIn("requested terminal host 'windows-terminal'", combined_output)

    def test_validation_wrapper_runhost_print_only_writes_reports(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            report_dir = root / "reports"
            fake_exe = root / "acecode.exe"
            fake_exe.touch()

            completed = run_validation_wrapper(
                "-Mode",
                "RunHost",
                "-TerminalHost",
                "windows-terminal",
                "-ReportDir",
                str(report_dir),
                "-PrintOnly",
                "-NoRecordResults",
                "-Cleanup",
                "-Exe",
                str(fake_exe),
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            reports = sorted(report_dir.glob("manual-report*.json"))
            self.assertEqual(6, len(reports))
            labels = {json.loads(path.read_text(encoding="utf-8"))["label"] for path in reports}
            self.assertEqual(
                {
                    "alt-short",
                    "alt-long",
                    "alt-ask",
                    "alt-tool",
                    "terminal-output-short",
                    "forced-compat-short",
                },
                labels,
            )

    def test_validation_wrapper_runhost_propagates_host_mismatch_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            report_dir = root / "reports"
            fake_exe = root / "acecode.exe"
            fake_exe.touch()

            env = os.environ.copy()
            env.pop("WT_SESSION", None)
            completed = run_validation_wrapper(
                "-Mode",
                "RunHost",
                "-TerminalHost",
                "windows-terminal",
                "-ReportDir",
                str(report_dir),
                "-Cleanup",
                "-Exe",
                str(fake_exe),
                env=env,
            )

            self.assertNotEqual(completed.returncode, 0)
            combined_output = completed.stdout + completed.stderr
            self.assertIn("requested terminal host 'windows-terminal'", combined_output)
            self.assertEqual([], sorted(report_dir.glob("manual-report*.json")))

    def test_validation_wrapper_runhost_rejects_no_record_results_for_real_run(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            report_dir = root / "reports"
            fake_exe = root / "acecode.exe"
            fake_exe.touch()

            completed = run_validation_wrapper(
                "-Mode",
                "RunHost",
                "-TerminalHost",
                "classic-conhost",
                "-ReportDir",
                str(report_dir),
                "-NoRecordResults",
                "-Exe",
                str(fake_exe),
            )

            self.assertNotEqual(completed.returncode, 0)
            combined_output = completed.stdout + completed.stderr
            self.assertIn("-NoRecordResults is only allowed with -PrintOnly", combined_output)
            self.assertEqual([], sorted(report_dir.glob("manual-report*.json")))

    def test_validation_wrapper_report_updates_tasks_and_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            report_dir = root / "reports"
            report_dir.mkdir()
            make_complete_report_dir(report_dir)
            tasks_path = root / "tasks.md"
            tasks_path.write_text(make_tasks_text(), encoding="utf-8")
            evidence_path = root / "evidence.md"

            completed = run_validation_wrapper(
                "-Mode",
                "Report",
                "-WindowsTerminalReportDir",
                str(report_dir),
                "-ConHostReportDir",
                str(report_dir),
                "-TasksPath",
                str(tasks_path),
                "-EvidencePath",
                str(evidence_path),
                "-UpdateTasks",
                "-WriteEvidence",
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn("Updated OpenSpec tasks: 8.5, 9.3, 9.4, 9.5, 9.6, 9.7", completed.stdout)
            self.assertIn("Wrote evidence:", completed.stdout)

            tasks_text = tasks_path.read_text(encoding="utf-8")
            self.assertIn("- [x] 8.5 ", tasks_text)
            self.assertIn("- [x] 9.7 ", tasks_text)
            self.assertIn("- [ ] 10.1 ", tasks_text)
            self.assertIn("- [ ] 10.2 ", tasks_text)

            evidence = evidence_path.read_text(encoding="utf-8")
            self.assertIn("- complete: true", evidence)
            self.assertIn("- updated_tasks: 8.5, 9.3, 9.4, 9.5, 9.6, 9.7", evidence)

    def test_validation_wrapper_cleanup_check_uses_custom_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            all_tasks = set(cleanup_check.MANUAL_TASKS + cleanup_check.CLEANUP_TASKS)
            tasks_path = root / "tasks.md"
            tasks_path.write_text(make_tasks_text(checked=all_tasks), encoding="utf-8")
            evidence_path = root / "evidence.md"
            evidence_path.write_text(
                "# Evidence\n\n- complete: true\n\n"
                + "\n".join(f"### {task_id} PASS" for task_id in cleanup_check.MANUAL_TASKS)
                + "\n",
                encoding="utf-8",
            )
            source_path = root / "main.cpp"
            source_path.write_text("int main() { return 0; }\n", encoding="utf-8")

            completed = run_validation_wrapper(
                "-Mode",
                "CleanupCheck",
                "-TasksPath",
                str(tasks_path),
                "-EvidencePath",
                str(evidence_path),
                "-SourcePath",
                str(source_path),
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn("Manual validation: complete", completed.stdout)
            self.assertIn("Legacy cleanup: complete", completed.stdout)

    def test_validation_wrapper_preflight_dry_run_prints_checks(self) -> None:
        completed = run_validation_wrapper("-Mode", "Preflight", "-DryRun")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("cmake --build build --target acecode --config Release", completed.stdout)
        self.assertIn(
            "cmake --build build --target acecode_unit_tests --config Debug",
            completed.stdout,
        )
        self.assertIn("python -m py_compile", completed.stdout)
        self.assertIn("ctest --test-dir build -C Debug -R 'ChatViewport|tui_chat_viewport_scripts'", completed.stdout)
        self.assertIn("python scripts\\tui_chat_viewport_smoke.py --mode both", completed.stdout)
        self.assertIn("openspec validate add-ftxui-chat-viewport", completed.stdout)
        self.assertIn("git diff --check", completed.stdout)


if __name__ == "__main__":
    unittest.main()
