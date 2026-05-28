---
name: acecode-release
description: Release ACECode from a user-provided version number. Use when Codex is asked to publish an ACECode version, update the Windows self-upgrade server, build and verify the updater package, commit release code, create an annotated Git version tag, or push release commits/tags.
---

# ACECode Release

## Purpose

Release ACECode in a traceable way: version files, tests, Git commit, annotated tag, Windows updater zip, `aceupdate.json`, and HTTP verification must all agree on the same version.

Use `scripts/publish_acecode_release.ps1` for the mechanical work whenever possible.

## Required Inputs

Require a semantic version such as `0.2.2`. Treat the Git tag as `v<version>`.

Default repository and update service assumptions:

- Repo root: current working directory, normally `C:\Users\shao\acecode`
- Update server folder: `J:\jenkins_green\aupdate`
- Update HTTP base URL: `http://2017studio.imwork.net:82/aupdate/`
- Windows update target: `windows-x64`

## Workflow

1. Inspect `git status --short` before changing anything. Identify which dirty files belong to this release. Do not stage unrelated user changes.
2. Run the release script with the version and explicit `-StageFiles` for all intended release-code changes beyond `CMakeLists.txt` and `vcpkg.json`.
3. Use `-Push` only when the user explicitly wants the commit and tag pushed to `origin`.
4. After the script finishes, report the commit/tag, package path, package size, SHA256, and verification commands.

Typical command:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .acecode\skills\acecode-release\scripts\publish_acecode_release.ps1 `
  -Version 0.2.2 `
  -Repo . `
  -StageFiles main.cpp,src\upgrade\http.cpp,src\upgrade\http.hpp,src\upgrade\upgrade.cpp,tests\upgrade\upgrade_http_test.cpp `
  -Push
```

If the version bump and all release changes are already committed, run without `-StageFiles`; the script will tag the current `HEAD` and package the current Release build after verification.

## Script Behavior

The script:

- Updates `CMakeLists.txt` `project(acecode VERSION ...)`.
- Updates `vcpkg.json` `version-semver` when present.
- Refuses to build/publish if there are dirty files outside the explicit release file set, unless `-AllowDirtyBuild` is passed.
- Builds `acecode` and `acecode_unit_tests` in Release.
- Runs `Upgrade*:*ConfigUpgrade*` tests unless `-SkipTests` is passed.
- Verifies `build\Release\acecode.exe --version` outputs the requested version.
- Commits staged release files unless `-NoCommit` is passed.
- Creates annotated tag `v<version>` unless `-NoTag` is passed.
- Optionally pushes `HEAD` and the tag when `-Push` is passed.
- Creates `acecode-<version>-windows-x64.zip` under the update server folder.
- Ensures update-server `web.config` maps `.zip` to `application/zip`.
- Updates `aceupdate.json`, preserving older release records and putting the new release first.
- Verifies the manifest and zip over HTTP, including `Accept: application/zip`.

## Guardrails

- Do not run `git reset --hard` or revert unrelated files.
- Do not package from a dirty working tree unless the user knowingly accepts `-AllowDirtyBuild`.
- Do not reuse an existing tag unless the user explicitly requests manual tag repair; the script fails on existing tags.
- Keep `aceupdate.json` version, zip filename, SHA256, package size, `acecode.exe --version`, and Git tag consistent.
- Remember that users upgrading from older binaries see the old updater UI during the first upgrade; new UI changes are visible after the upgraded binary is installed.
