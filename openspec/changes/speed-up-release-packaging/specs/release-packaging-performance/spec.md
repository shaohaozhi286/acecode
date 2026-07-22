## ADDED Requirements

### Requirement: Full package matrix is release-scoped
The package workflow SHALL run the complete platform matrix for version tags and explicit manual dispatches, and SHALL NOT run that matrix for ordinary pull requests or `master` pushes.

#### Scenario: Version tag package build
- **WHEN** a `v*` tag is pushed
- **THEN** the workflow builds every current and old-Linux package and makes the release jobs eligible

#### Scenario: Ordinary source push
- **WHEN** a commit is pushed to `master` without a version tag
- **THEN** the dedicated test workflow runs without starting the full package matrix

### Requirement: Supported persistent dependency cache
Every native package and unit-test job SHALL use a supported persistent vcpkg binary-cache provider keyed by its build environment and dependency inputs, and MUST NOT use the removed `x-gha` provider.

#### Scenario: Repeated build with unchanged dependencies
- **WHEN** the package workflow runs twice for the same target triplet and unchanged manifest, registry baseline, and overlay ports
- **THEN** the second run restores compatible vcpkg binary packages instead of rebuilding all dependencies from source

#### Scenario: Old-Linux cache restoration
- **WHEN** an old-Linux job restores cached dependencies
- **THEN** it still verifies the maximum GLIBC version and WebKitGTK ABI before packaging

### Requirement: Native-speed Windows ARM64 production
The Windows ARM64 package job SHALL run the ARM64 cross compiler on a native x64 host or use a native ARM64 compiler host, and SHALL verify that every packaged executable targets ARM64.

#### Scenario: Cross-compiled ARM64 package
- **WHEN** the Windows ARM64 job finishes compiling and packaging
- **THEN** `acecode.exe`, `acecode-desktop.exe`, and `ace-browser-host.exe` all report the ARM64 machine type before upload

### Requirement: Single Web UI build per package workflow
The package workflow SHALL build `web/dist` once and SHALL provide that identical output to every current and old-Linux native package job.

#### Scenario: Native matrix embeds Web assets
- **WHEN** native package jobs configure CMake
- **THEN** each job has downloaded the Web artifact produced by the single Web build job

### Requirement: Test-only native dependencies
Production package configuration SHALL exclude GoogleTest, while the unit-test workflow SHALL explicitly enable the manifest feature that provides it.

#### Scenario: Production dependency resolution
- **WHEN** CMake configures a package with `BUILD_TESTING=OFF`
- **THEN** vcpkg does not install GoogleTest for that package job

#### Scenario: Unit-test dependency resolution
- **WHEN** the test workflow configures with `BUILD_TESTING=ON`
- **THEN** the test manifest feature provides GoogleTest and the test target remains buildable

### Requirement: Release artifact contract remains unchanged
The optimized workflow MUST preserve the existing platform archive names and contents, debug-symbol artifacts, browser-extension artifact, GitHub Release behavior, and npm platform publication set.

#### Scenario: Stable tag publication
- **WHEN** an optimized stable tag workflow completes
- **THEN** all expected Windows, Linux, old-Linux, macOS, browser-extension, debug, and npm outputs are published with the same user-facing layout as before

### Requirement: Optimization is measured before release
The change SHALL be validated using two complete manual package runs on the same commit, with the warm run showing supported dependency restoration and no removed-backend warning.

#### Scenario: Warm validation run
- **WHEN** the second manual run completes on the unchanged validation commit
- **THEN** its logs show binary-package restoration and its critical path is compared with the recorded 24-minute baseline before the release tag is created
