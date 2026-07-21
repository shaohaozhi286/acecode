## 1. Package Layout

- [x] 1.1 Rename the CLI package to `acecode` and move desktop/platform resolution to `@aceagent`
- [x] 1.2 Add the Windows ARM64 native npm package and exact optional dependency
- [x] 1.3 Update npm package documentation and repository publishing notes

## 2. Publication Workflow

- [x] 2.1 Add an optional manual `npm_version` dispatch input with canonical-version validation
- [x] 2.2 Make missing authentication and registry failures fail the npm job while preserving ordered idempotent retries

## 3. Verification

- [x] 3.1 Generate all package manifests from synthetic artifacts and run npm pack checks
- [x] 3.2 Install the packed Windows package plus `acecode` launcher in a clean temporary project and run the launcher
- [x] 3.3 Run strict OpenSpec validation, workflow checks, and `git diff --check`

## 4. Publication

- [ ] 4.1 Commit and push only the npm publishing and OpenSpec changes
- [ ] 4.2 Dispatch the 0.7.7 npm backfill and wait for the workflow to finish
- [ ] 4.3 Verify npm registry metadata and a clean `npm i acecode` installation
