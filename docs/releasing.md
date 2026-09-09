# Releasing Studio Duo

Studio Duo releases are created from semantic version tags on `main`. A release
tag starts the GitHub Actions release workflow, which builds and tests the
application before publishing:

- a universal macOS DMG for Apple Silicon and Intel Macs
- a Windows x64 ZIP containing `Studio Duo.exe`
- `SHA256SUMS.txt` for download verification

## Prerequisites

The release maintainer needs:

- a clean checkout of the `main` branch
- permission to push `main` and tags to the repository
- Git, CMake 3.25 or newer, and the normal Studio Duo build toolchain

The script fast-forwards the local checkout to the latest `origin/main`, runs a
release build and the complete test suite, creates any required version commit
and the annotated tag, then pushes both atomically. If branch protection rejects
direct release commits, a repository administrator must grant the release
maintainer the required bypass before running it.

## Mint a release

From the repository root on `main`, run:

```sh
./scripts/release.sh
```

With no argument, the script increments the patch version. For example, `0.1.0`
becomes `0.1.1`.

To select a version explicitly:

```sh
./scripts/release.sh 0.2.0
```

A leading `v` is also accepted. Release versions must use the stable
`MAJOR.MINOR.PATCH` form; prerelease suffixes are not supported.

The script updates `VERSION`, whose value is consumed by CMake and embedded in
the application. When the requested version differs, it commits the change as
`chore: release v0.2.0`. It then creates the matching `v0.2.0` tag and pushes
the commit and tag together. An explicit version equal to the current untagged
`VERSION` can therefore publish that version without an unnecessary commit.
The tag triggers `.github/workflows/release.yml`.

If the atomic push fails after the local commit and tag were created, fix the
reported remote or permission problem and run the same command again. The
script detects the unpublished release commit and retries it instead of
incrementing the version.

## Publication guarantees

The workflow rejects a tag when:

- its name is not `vMAJOR.MINOR.PATCH`
- the tag does not match `VERSION`
- the tagged commit is not reachable from `origin/main`

Both platform builds must pass the complete test suite before the GitHub Release
is published. GitHub automatically generates release notes from the merged
changes since the previous tag.

## Install released artifacts

Download artifacts from the repository's
[Releases](https://github.com/mbianchidev/studio-duo/releases) page.

On macOS, open the DMG and drag **Studio Duo** into **Applications**. The current
pipeline applies an ad hoc application signature but does not use an Apple
Developer ID or notarization, so macOS may require approval in **System
Settings > Privacy & Security** on first launch.

On Windows, extract the ZIP to a writable directory and run `Studio Duo.exe`.

Verify either download against `SHA256SUMS.txt` before installation.
