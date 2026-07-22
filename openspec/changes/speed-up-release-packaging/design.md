## Context

The package workflow builds six current platform archives, two old-glibc Linux archives, debug symbols, a browser extension, GitHub Release assets, and npm packages. Recent runs show that dependency installation and native compilation dominate the critical path; compression and release upload take only seconds. The workflow currently relies on `run-vcpkg` setting the removed `x-gha` backend, rebuilds the Web UI in every native job, runs the full matrix for pull requests, `master`, and tags, and configures `amd64_arm64` tools on an ARM64 Windows runner.

The published filenames, combined CLI/desktop/browser-host layout, old-Linux ABI checks, debug artifacts, npm package set, and tag release behavior are compatibility constraints.

## Goals / Non-Goals

**Goals:**

- Restore vcpkg packages from a supported persistent cache on repeated builds.
- Remove x64 compiler emulation from the Windows ARM64 critical path.
- Avoid duplicate full matrices when a release commit and its tag are pushed.
- Build one deterministic Web UI output and embed it across all native jobs.
- Keep test-only packages out of production dependency installation.
- Demonstrate the improvement with two complete manual runs on the same commit before release.

**Non-Goals:**

- Changing runtime features, archive names, or package contents.
- Splitting the combined CLI and desktop distributions.
- Removing old-Linux builds, debug symbols, or npm publishing.
- Introducing a C++ compiler cache or changing optimization/strip flags in this change.
- Making the Windows updater consume Unix tarballs.

## Decisions

### Use vcpkg's file provider with GitHub Actions cache

Each job will override the removed `x-gha` backend with `clear;default,readwrite`, restore `VCPKG_DEFAULT_BINARY_CACHE` through `actions/cache`, and key it by cache schema, runner label, target triplet, manifest/configuration files, and overlay ports. Test builds use a distinct key because they enable the test dependency feature. Old-Linux builds retain their container and ABI checks but gain a container-specific binary cache.

This is preferred over a NuGet feed for the first repair because it needs no package-registry credentials or Mono setup and follows the existing GitHub Actions storage boundary. A NuGet feed remains an option if cache size or immutable-key behavior becomes a maintenance problem.

### Cross-compile Windows ARM64 on the x64 Windows runner

The ARM64 package will run on `windows-2022` with `amd64_arm64`. The current ARM64 runner invokes `HostX64\\ARM64`, so the compiler itself runs through x64 emulation. Running that same supported cross compiler on its native x64 host avoids emulation while preserving the ARM64 target triplet.

The packaged executables will be checked with `dumpbin /headers` for the ARM64 machine type before upload. Keeping the ARM runner and selecting `HostARM64\\ARM64` was considered, but the x64 cross runner has the more mature tool image and directly matches the configured toolchain.

### Reserve the full matrix for tags and manual dispatch

`package.yml` will stop running for ordinary pull requests and `master` pushes. The existing `test.yml` remains the required PR/master validation path. Manual dispatch remains available for full pre-release validation and npm backfill. A concurrency group cancels superseded non-tag runs but never cancels a version-tag release.

This is preferred over attempting to reuse artifacts across separate master and tag runs because cross-run promotion adds release-state lookup and retention failure modes. A future release workflow may create the tag only after a successful prebuild if tag-to-release latency needs to approach zero.

### Build Web assets once

An Ubuntu job will install pnpm with dependency caching, build `web/dist`, and upload it as a workflow artifact. Every native package job downloads the same artifact before CMake configure. Old-Linux containers no longer install Node or pnpm. This guarantees identical embedded Web bytes while removing repeated frontend work from the matrix.

### Make GoogleTest an explicit manifest feature

`gtest` moves from the default vcpkg dependency list into a `tests` feature. The unit-test workflow enables that feature; package workflows keep it disabled alongside `BUILD_TESTING=OFF`.

## Risks / Trade-offs

- [GitHub cache entries are immutable and scoped] -> Version the cache key, include the runner label/triplet/dependency hash, and prove restoration with a second run on the same branch.
- [A restored package could violate old-Linux compatibility] -> Keep the Buster-specific cache key and run the existing GLIBC/WebKitGTK verification on every build.
- [Cross-compilation could accidentally produce x64 executables] -> Fail the ARM64 job unless every packaged `.exe` reports the ARM64 machine type.
- [Removing full PR packaging could hide a platform-only workflow error] -> Keep manual full dispatch, validate packaging changes before merge, and retain the dedicated native test workflow for normal changes.
- [The Web artifact can become an undeclared release asset] -> Give it a non-package artifact name and keep release collection restricted to archive/debug extensions.

## Migration Plan

1. Apply workflow and manifest changes on an isolated branch.
2. Run local YAML/OpenSpec/diff validation and production/test manifest checks.
3. Push the branch and run the full manual package workflow twice on the same commit.
4. Confirm the second run restores vcpkg packages, contains no removed-backend warning, produces all expected archives, and materially improves the 24-minute baseline.
5. Fast-forward the validated commit to `master`, publish the next tag, and verify all GitHub and update-server assets.

Rollback is a revert of the workflow/manifest commit. Existing release assets and updater manifests are unaffected by the CI-only change.

## Open Questions

None. Compiler-cache and build-before-tag promotion are intentionally deferred until the supported dependency cache and ARM64 runner change have measured results.
