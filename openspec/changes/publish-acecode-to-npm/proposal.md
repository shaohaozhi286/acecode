## Why

ACECode tag builds currently report success even when npm publishing is skipped, and the generated package scope does not match the existing `aceagent` npm organization. Users also cannot install the CLI with the desired `npm i acecode` command because the main package is generated as a scoped package.

## What Changes

- Publish the CLI launcher as the public unscoped package `acecode` so local installs use `npm i acecode` and global installs use `npm i -g acecode`.
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
- Publishes public registry packages named `acecode`, `@aceagent/desktop`, and `@aceagent/<platform>`.
- Uses the repository `NPM_TOKEN` secret for the initial publication and retries.
