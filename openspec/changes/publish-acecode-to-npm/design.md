## Context

The existing tag workflow already assembles one small JavaScript launcher package for each user-facing command and native packages selected through `optionalDependencies`. The implementation currently hard-codes the nonexistent `@acecode` scope, omits the newly supported Windows ARM64 artifact, and treats an absent `NPM_TOKEN` as a successful no-op. The public `aceagent` npm organization and repository `NPM_TOKEN` now exist, while the requested CLI install name is the unscoped package `acecode`.

## Goals / Non-Goals

**Goals:**

- Make `npm i acecode` install the CLI launcher and exactly one compatible native package.
- Keep desktop and native binary packages in the controlled `@aceagent` namespace.
- Publish tag builds automatically and permit an explicit version backfill from a manual workflow run.
- Fail publishing visibly when authentication or registry publication fails.
- Keep retries safe after a partially successful multi-package publication.

**Non-Goals:**

- Publish old-glibc Linux variants, because npm cannot select packages by glibc compatibility.
- Replace the current native binary co-location contract.
- Configure npm trusted publishers in this change; the initial publication continues to use the configured repository secret.

## Decisions

### Separate the CLI name from the native-package scope

The CLI template will use the unscoped name `acecode`, while the generator and both launchers will use `@aceagent` when resolving native packages. The desktop template will use `@aceagent/desktop`. This keeps the requested short install command without placing large platform artifacts in the global namespace.

Alternative considered: keep `@aceagent/cli`. This avoids possible registry similarity checks but does not satisfy `npm i acecode`.

### Publish one native package per npm-supported OS/CPU pair

The generator will create exact-version optional dependencies for Linux x64/arm64, Windows x64/arm64, and macOS x64/arm64. Each native package retains the colocated CLI, desktop shell, and browser host files required by runtime path discovery.

### Use an explicit manual npm version for backfills

`workflow_dispatch` will expose an optional `npm_version` input. Supplying it enables the npm job after the normal build matrix completes, without enabling the GitHub Release job. The requested value must equal the version declared in `CMakeLists.txt`, preventing a manual run from labeling unrelated source as an older release.

Alternative considered: download artifacts from an older workflow run. A fresh matrix build is easier to audit and also produces the newly supported Windows ARM64 package. For the 0.7.7 backfill, `master` differs from `v0.7.7` only in packaging workflow configuration, so the product source remains identical.

### Preserve idempotent ordered publication

Native packages publish first, followed by `acecode` and `@aceagent/desktop`. Before each publish, the job checks whether the exact package version exists and skips it when present. A retry can therefore finish after a partial registry publication without attempting to overwrite immutable versions.

### Treat absent credentials as failure

Until trusted publishing is configured, an empty `NPM_TOKEN` will emit an Actions error and exit nonzero. This makes the workflow conclusion match registry state.

## Risks / Trade-offs

- [The registry may reject `acecode` as too similar to `ace-code`] -> Publish the real package to obtain the authoritative result; retain already-published `@aceagent` platform versions for an idempotent retry or request npm name review if rejected.
- [A manual backfill could publish the wrong source version] -> Require the input version to match `project(acecode VERSION ...)` in `CMakeLists.txt`.
- [A later platform matrix addition could be omitted from npm] -> Keep the generator platform table and workflow extraction list explicit and validate all expected inputs.
- [A publish can stop after some packages succeed] -> Check exact versions before every publish and keep platform-first ordering.

## Migration Plan

1. Update templates, launchers, generator, documentation, and workflow.
2. Validate generated manifests and local installation with synthetic platform artifacts.
3. Commit and push only the npm/OpenSpec files, preserving unrelated working-tree edits.
4. Dispatch `package.yml` with `npm_version=0.7.7` and wait for the npm job.
5. Verify registry metadata and install/run `acecode` from a clean temporary project.

Rollback consists of reverting the repository configuration before any successful publish. Published npm versions are immutable; after publication, corrections require a new version rather than overwriting 0.7.7.

## Open Questions

None.
