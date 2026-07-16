## 1. Windows Dependency and Shared Target

- [x] 1.1 Add WinToast as a Windows-only vcpkg dependency and configure its CMake package target.
- [x] 1.2 Move the Windows notification source into the native support boundary shared by TUI and Desktop while retaining non-Windows stubs.

## 2. WinToast Backend

- [x] 2.1 Replace the Shell_NotifyIcon balloon implementation with WinToast initialization, delivery, per-toast payload handlers, and orderly shutdown.
- [x] 2.2 Add native helpers and tests for Unicode-safe notification text, object/JSON-string bridge payload parsing, activation payload fidelity, and failure/no-op behavior.
- [x] 2.3 Remove obsolete tray balloon click coupling and stale legacy-notification comments.

## 3. Desktop Integration

- [x] 3.1 Initialize WinToast independently of tray availability and register Desktop window/session activation routing.
- [x] 3.2 Route the WebView notification bridge through the shared parser/backend while preserving completion, question, config, and focus-suppression behavior.

## 4. TUI Integration

- [x] 4.1 Expose and test a direct resume-by-session-id entry point backed by the canonical `/resume` restoration logic.
- [x] 4.2 Initialize WinToast for the TUI, publish normal completed-turn notifications on the FTXUI thread, and suppress aborted/error/replay or foreground-visible completions as specified.
- [x] 4.3 On TUI notification activation, restore the terminal and post exact-session resume to the FTXUI thread; shut the backend down before captured runtime objects are destroyed.

## 5. Verification

- [x] 5.1 Run focused native notification/routing tests and existing Web notification/transcript tests.
- [x] 5.2 Run the complete Web test/build pipeline and C++ unit suite.
- [x] 5.3 Configure and build both `acecode` and `acecode-desktop` on Windows, then perform a runtime smoke of delivery and activation routing.
- [x] 5.4 Validate the OpenSpec change and audit every requirement against implementation and test evidence.

Verification note: the full 2,607-test C++ run passed 2,599 tests and skipped
three expected opt-in/platform fixtures. Five unchanged `DesktopSingleInstance`
tests were blocked because a production `acecode-desktop.exe` held the product
singleton mutex during the run; the same seven-test group passed 7/7 in an
isolated rerun before that live process started. All notification, terminal
outcome, session routing, Web, build, and opt-in WinToast runtime checks passed.
