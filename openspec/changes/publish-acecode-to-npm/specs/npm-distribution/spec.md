## ADDED Requirements

### Requirement: Organization-owned CLI package
The npm distribution SHALL publish the ACECode CLI launcher as the public scoped package `@aceagent/acecode` and SHALL expose the executable name `acecode`.

#### Scenario: Local CLI installation
- **WHEN** a user runs `npm i @aceagent/acecode` on a supported platform
- **THEN** npm installs the CLI launcher and the matching native platform package

#### Scenario: Global CLI installation
- **WHEN** a user runs `npm i -g @aceagent/acecode` on a supported platform
- **THEN** the `acecode` command is available on the user's command path

### Requirement: Organization-owned native packages
The npm distribution SHALL publish the desktop launcher and native platform packages under the `@aceagent` scope.

#### Scenario: Native dependency selection
- **WHEN** npm resolves `@aceagent/acecode` or `@aceagent/desktop`
- **THEN** exact-version optional dependencies identify the supported `@aceagent/<os>-<cpu>` packages

#### Scenario: Supported platform set
- **WHEN** the staging generator processes a complete current release build
- **THEN** it creates native packages for Linux x64/arm64, Windows x64/arm64, and macOS x64/arm64

### Requirement: Runtime binary co-location
Each native platform package MUST preserve the colocated CLI, desktop, and browser-host files expected by ACECode runtime path resolution.

#### Scenario: Launcher execution
- **WHEN** an installed launcher resolves its matching native package
- **THEN** it executes the native binary in place without copying it away from its peer binaries

### Requirement: Authenticated publication is observable
The npm publication job MUST fail when its required publishing credential is absent or when any registry publication fails.

#### Scenario: Missing token
- **WHEN** a tag or requested backfill reaches the publish step without `NPM_TOKEN`
- **THEN** the workflow reports an error and exits with a failed conclusion

#### Scenario: Registry rejection
- **WHEN** npm rejects a package publication
- **THEN** the publish job stops and reports failure instead of reporting a successful release

### Requirement: Tag and manual backfill publication
The workflow SHALL publish npm packages for version tags and SHALL support an explicit manual npm-version backfill without creating another GitHub Release.

#### Scenario: Stable tag publication
- **WHEN** a `vX.Y.Z` tag build succeeds
- **THEN** the workflow publishes version `X.Y.Z` with dist-tag `latest`

#### Scenario: Prerelease tag publication
- **WHEN** a tag version contains a prerelease suffix
- **THEN** the workflow publishes that version with dist-tag `next`

#### Scenario: Manual version backfill
- **WHEN** a workflow dispatch supplies `npm_version` equal to the version declared in `CMakeLists.txt`
- **THEN** the workflow builds the platform artifacts and publishes that npm version without running the GitHub Release job

#### Scenario: Mismatched manual version
- **WHEN** a workflow dispatch supplies an `npm_version` different from the version declared in `CMakeLists.txt`
- **THEN** the npm job fails before publishing any package

### Requirement: Idempotent ordered retry
The publication job MUST publish native packages before launcher packages and MUST skip exact package versions already present in the registry.

#### Scenario: Retry after partial publication
- **WHEN** a previous attempt published some packages before failing
- **THEN** a rerun skips those exact versions and continues with the remaining packages
