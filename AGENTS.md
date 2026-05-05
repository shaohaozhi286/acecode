# Repository Guidelines

## Project Structure & Module Organization

ACECode is a C++17 terminal AI coding agent. The root `main.cpp` contains the TUI entry point, while reusable logic lives under `src/` by feature area: `agent_loop/`, `commands/`, `config/`, `daemon/`, `provider/`, `session/`, `skills/`, `tool/`, `tui/`, and `utils/`. Unit tests live in `tests/` and mirror source paths, for example `src/session/session_storage.cpp` is tested by `tests/session/session_storage_test.cpp`. Static model catalog assets are in `assets/models_dev/`; user docs are in `docs/`; vendored or submodule code is under `external/`; vcpkg overlay ports are under `ports/`.

Start with docs instead of duplicating them here: [README.md](README.md) for product overview, [ARCHITECTURE.md](ARCHITECTURE.md) for high-level structure, [DOCS_INDEX.md](DOCS_INDEX.md) for document navigation, [docs/daemon-api.md](docs/daemon-api.md) for HTTP/WebSocket behavior, [docs/model-context-resolution.md](docs/model-context-resolution.md) for model selection, [docs/skills.md](docs/skills.md) and [docs/skills-implementation.md](docs/skills-implementation.md) for skills, and [docs/desktop-shell/multi-workspace.md](docs/desktop-shell/multi-workspace.md) for the current desktop multi-workspace model. [CLAUDE.md](CLAUDE.md) is a detailed implementation memory; consult it for current subsystem notes but keep this file concise.

## Build, Test, and Development Commands

- `git submodule update --init --recursive`: fetch required submodules.
- `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=<triplet> -DVCPKG_OVERLAY_PORTS=$PWD/ports -DBUILD_TESTING=ON`: configure a testable build.
- `cmake --build build --config Release`: build the `acecode` executable.
- `cmake --build build --target acecode_unit_tests`: build the GoogleTest binary.
- `ctest --test-dir build --output-on-failure`: run the unit suite.
- `scripts/code_quality_check.sh` or `scripts/code_quality_check.bat`: run local quality checks for common DRY and error-handling issues.
- Web UI lives in `web/`: run `pnpm install` once, `pnpm test` for JS tests, and `pnpm build` before configuring/building CMake when embedded frontend assets must be refreshed. See [web/README.md](web/README.md).
- Desktop shell is opt-in: configure with `-DACECODE_BUILD_DESKTOP=ON` and build `acecode-desktop`. The desktop target expects the daemon executable beside it at runtime.

## Architecture Boundaries

- Keep TUI-specific code in `main.cpp`, `src/tui/`, and `src/markdown/`; place reusable, testable logic in the relevant `src/<feature>/` directory.
- `acecode_testable` intentionally excludes the full TUI entry point and desktop WebView shell; pure helpers can be added there and covered by unit tests.
- Daemon/API work usually touches `src/daemon/`, `src/web/`, and `src/session/`; update [docs/daemon-api.md](docs/daemon-api.md) when protocol behavior changes.
- React/Vite/Tailwind frontend work stays under `web/src/`. Do not edit generated build output directly; regenerate it with the web build.
- Avoid modifying vendored/submodule trees (`external/`, `hermes-agent/`, `claudecodehaha/`) unless the task explicitly targets them.

## OpenSpec Workflow

For non-trivial behavior changes, create or continue an OpenSpec change under `openspec/changes/` before implementation. Use the prompts in [.github/prompts/opsx-propose.prompt.md](.github/prompts/opsx-propose.prompt.md), [.github/prompts/opsx-apply.prompt.md](.github/prompts/opsx-apply.prompt.md), and [.github/prompts/opsx-archive.prompt.md](.github/prompts/opsx-archive.prompt.md). During implementation, read the change context, complete tasks one by one, and immediately update task checkboxes in the change's `tasks.md`.

## Coding Style & Naming Conventions

Follow `.editorconfig`: UTF-8, LF line endings, final newline, 4-space indentation for C++ and CMake, 2-space indentation for JSON/YAML. Use C++17. Keep headers in `src/**/*.hpp` and implementations in matching `.cpp` files. Name test files with the singular suffix `_test.cpp`; `tests/CMakeLists.txt` discovers that pattern automatically. Prefer existing helpers such as `ToolArgsParser`, `ToolErrors`, and path/session utilities instead of duplicating parsing or validation logic.

This is a terminal UI project: avoid adding Emoji or ambiguous-width glyphs to C++ source, rendered UI, logs, and console output. Prefer ASCII or width-stable symbols already used in the codebase.

## Testing Guidelines

Tests use GoogleTest through the `acecode_unit_tests` target. Add tests for pure logic, serializers, parsers, validators, and headless state machines. Keep TUI-heavy code in `src/tui/`, `src/markdown/`, and `main.cpp` manually validated unless the code can be isolated. Use `testing::TempDir()` or `std::filesystem::temp_directory_path()` for file I/O; do not write test artifacts into the repository tree. Prefer `EXPECT_*` unless failure would make later assertions unsafe.

If CMake test discovery/build integration is unavailable in the editor, still keep changes compatible with the documented `cmake --build` and `ctest` commands. For web-only changes, at minimum run `pnpm test` and `pnpm build` from `web/`.

## Commit & Pull Request Guidelines

Recent history uses short imperative commits, sometimes with `feat:` prefixes, for example `feat: Implement AskUserQuestion tool` or `Add unit tests for session serialization`. Keep commits focused and mention tests when relevant. Pull requests should describe the behavior change, list verification commands, link related issues, and include screenshots or terminal captures for visible TUI changes.

## Security & Configuration Tips

Do not commit API keys, Copilot tokens, generated session data, or local `~/.acecode/config.json` contents. Treat `--dangerous` mode as a local-only convenience and avoid recommending it in docs or examples without a clear warning.

On Windows PowerShell 5.1, avoid `Set-Content` / `Out-File` for files containing Chinese or other non-ASCII text because they can write BOMs or mojibake. Use editor-based edits or explicit UTF-8 without BOM APIs instead.
