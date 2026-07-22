## 1. Dependency Resolution

- [x] 1.1 Move GoogleTest into a test-only vcpkg feature and enable it explicitly in the unit-test workflow
- [x] 1.2 Add supported platform/triplet-scoped vcpkg binary-cache restoration to the current-platform package matrix
- [x] 1.3 Add isolated supported binary caches to old-Linux package jobs and the unit-test workflow while preserving ABI checks

## 2. Package Workflow Critical Path

- [x] 2.1 Build and cache the Web UI once, then download the same `web/dist` artifact in every native package job
- [x] 2.2 Cross-compile Windows ARM64 on a native x64 host and verify every packaged executable reports ARM64
- [x] 2.3 Restrict the full package matrix to tags/manual dispatch and add non-tag concurrency cancellation without changing release/npm conditions

## 3. Validation

- [x] 3.1 Run YAML/workflow checks, production and test manifest checks, strict OpenSpec validation, and `git diff --check`
- [x] 3.2 Push the isolated validation branch and complete a first full manual package run
- [x] 3.3 Complete a second full manual package run on the same commit and verify supported vcpkg cache restoration with no `x-gha` warning
- [x] 3.4 Compare critical-path timing with the 24-minute baseline and audit all expected package/debug artifacts
