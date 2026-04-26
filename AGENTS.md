# Repository Guidelines

## Project Structure & Module Organization

ACECode is a C++17 terminal AI coding agent. The root `main.cpp` contains the TUI entry point, while reusable logic lives under `src/` by feature area: `agent_loop/`, `commands/`, `config/`, `daemon/`, `provider/`, `session/`, `skills/`, `tool/`, `tui/`, and `utils/`. Unit tests live in `tests/` and mirror source paths, for example `src/session/session_storage.cpp` is tested by `tests/session/session_storage_test.cpp`. Static model catalog assets are in `assets/models_dev/`; user docs are in `docs/`; vendored or submodule code is under `external/`; vcpkg overlay ports are under `ports/`.

## Build, Test, and Development Commands

- `git submodule update --init --recursive`: fetch required submodules.
- `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=<triplet> -DBUILD_TESTING=ON`: configure a testable build.
- `cmake --build build --config Release`: build the `acecode` executable.
- `cmake --build build --target acecode_unit_tests`: build the GoogleTest binary.
- `ctest --test-dir build --output-on-failure`: run the unit suite.
- `scripts/code_quality_check.sh` or `scripts/code_quality_check.bat`: run local quality checks for common DRY and error-handling issues.

## Coding Style & Naming Conventions

Follow `.editorconfig`: UTF-8, LF line endings, final newline, 4-space indentation for C++ and CMake, 2-space indentation for JSON/YAML. Use C++17. Keep headers in `src/**/*.hpp` and implementations in matching `.cpp` files. Name test files with the singular suffix `_test.cpp`; `tests/CMakeLists.txt` discovers that pattern automatically. Prefer existing helpers such as `ToolArgsParser`, `ToolErrors`, and path/session utilities instead of duplicating parsing or validation logic.

## Testing Guidelines

Tests use GoogleTest through the `acecode_unit_tests` target. Add tests for pure logic, serializers, parsers, validators, and headless state machines. Keep TUI-heavy code in `src/tui/`, `src/markdown/`, and `main.cpp` manually validated unless the code can be isolated. Use `testing::TempDir()` or `std::filesystem::temp_directory_path()` for file I/O; do not write test artifacts into the repository tree. Prefer `EXPECT_*` unless failure would make later assertions unsafe.

## Commit & Pull Request Guidelines

Recent history uses short imperative commits, sometimes with `feat:` prefixes, for example `feat: Implement AskUserQuestion tool` or `Add unit tests for session serialization`. Keep commits focused and mention tests when relevant. Pull requests should describe the behavior change, list verification commands, link related issues, and include screenshots or terminal captures for visible TUI changes.

## Security & Configuration Tips

Do not commit API keys, Copilot tokens, generated session data, or local `~/.acecode/config.json` contents. Treat `--dangerous` mode as a local-only convenience and avoid recommending it in docs or examples without a clear warning.
