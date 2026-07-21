## Why

ACECode tag builds previously reported success even when npm publishing was skipped, and the generated package scope did not match the existing `aceagent` npm organization. An attempted public backfill then established that npm rejects the unscoped name `acecode` as too similar to the active `ace-code` package, so the user selected the organization-owned `@aceagent/acecode` name.

## What Changes

- Publish the CLI launcher as the public scoped package `@aceagent/acecode`, while retaining the `acecode` executable name.
- Publish the desktop launcher and native platform packages under the existing `@aceagent` scope.
- Include Windows ARM64 in the npm platform-package set alongside the existing supported platforms.
- Make a missing npm publishing credential fail visibly instead of silently succeeding.
- Add an explicit manual npm-version input so an already-built release version can be rebuilt and backfilled without creating another GitHub Release.
- Preserve idempotent retries and `latest` versus `next` dist-tag behavior.

## Capabilities

### New Capabilities

- `npm-distribution`: Defines ACECode npm package names, platform dependency selection, authenticated tag publishing, and manual version backfill behavior.

### Modified Capabilities

None.

## Impact

- Affects npm launcher templates under `npm/`, the staging generator in `scripts/npm/prepare-npm-packages.mjs`, and `.github/workflows/package.yml`.
- Publishes public registry packages named `@aceagent/acecode`, `@aceagent/desktop`, and `@aceagent/<platform>`.
- Uses the repository `NPM_TOKEN` secret for the initial publication and retries.
