# tests/

Unit tests for acecode, built with [GoogleTest](https://github.com/google/googletest).

## Run

```bash
cmake -B build -DBUILD_TESTING=ON ...    # (usual toolchain args)
cmake --build build --target acecode_unit_tests
ctest --test-dir build --output-on-failure
```

## Layout

Test files live at `tests/<same path as under src>/<unit>_test.cpp`:

| Production source                    | Test file                                      |
|--------------------------------------|------------------------------------------------|
| `src/utils/terminal_title.cpp`       | `tests/utils/terminal_title_test.cpp`          |
| `src/session/session_storage.cpp`    | `tests/session/session_storage_test.cpp`       |
| `src/permissions.hpp`                | `tests/permissions_test.cpp`                   |

Adding a new test file needs no CMake edit — `tests/CMakeLists.txt` globs
`*_test.cpp` (note: singular `_test`, not `_tests`).

## What's in scope

- Pure logic: serializers, validators, parsers, glob matchers, hash functions.
- Headless state machines: tool progress line buffering, line/chunk processors.
- Schema roundtrips: `SessionMeta`, `ChatMessage`, config files.

## What's exempt

- `src/tui/*`, `src/markdown/*`, `main.cpp` — these pull FTXUI and are validated
  manually through the TUI. The `acecode_testable` OBJECT library excludes
  them, so unit tests cannot accidentally depend on them.
- LLM provider HTTP paths (`CopilotProvider`, `OpenAiCompatProvider`) — need
  mock HTTP server; left to a future integration-tests change.
- Agent loop end-to-end behavior — same reason.

## Conventions

- Test suite name (first arg to `TEST`) matches the unit under test
  (`SanitizeTitle`, `SessionStorage`, `Base64Encode`).
- Prefer `EXPECT_*` over `ASSERT_*` except when later assertions would crash
  on the failing state (e.g. `ASSERT_EQ(vec.size(), 1u)` before `vec[0]`).
- Use `testing::TempDir()` or `std::filesystem::temp_directory_path()` for any
  file I/O — never write to the repo tree.
