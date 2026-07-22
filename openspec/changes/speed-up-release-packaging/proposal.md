## Why

ACECode's full GitHub packaging workflow now takes about 24 minutes because its vcpkg binary cache uses the removed `x-gha` backend, the Windows ARM64 job runs an x64-hosted compiler under emulation, and every release commit is built once for `master` and again for its tag. The release pipeline should restore reusable work and reserve the full platform matrix for actual packaging requests without changing the published artifact contract.

## What Changes

- Replace the removed vcpkg GitHub Actions backend with an explicit supported binary-cache path for normal, old-Linux, and unit-test builds.
- Build Windows ARM64 with a native-speed supported toolchain instead of running the x64 compiler on an ARM64 host.
- Run the full package matrix only for version tags and explicit manual dispatches, while ordinary pull requests and `master` pushes continue through the dedicated test workflow.
- Build the embedded Web UI once per package workflow and reuse the same output in every native matrix job.
- Move GoogleTest into a test-only vcpkg feature so production package configuration does not install it.
- Preserve all current platform archives, debug-symbol artifacts, GitHub Release behavior, and npm publication behavior.

## Capabilities

### New Capabilities

- `release-packaging-performance`: Defines cache reuse, trigger isolation, single-build Web assets, native-speed Windows ARM64 compilation, and unchanged release outputs.

### Modified Capabilities

None.

## Impact

- Affects `.github/workflows/package.yml`, `.github/workflows/test.yml`, and `vcpkg.json`.
- Changes GitHub Actions cache storage and full-matrix trigger behavior, but does not change runtime APIs or end-user package layouts.
- Requires live workflow-dispatch validation on the same commit to prove cold and warm paths before publishing the next release.
