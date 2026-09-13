# Releasing Studio Duo

Studio Duo releases are created from semantic version tags on `main`. A release
tag starts the GitHub Actions release workflow, which builds and tests the
application before publishing:

- a universal macOS DMG for Apple Silicon and Intel Macs
- a universal macOS ZIP used only by the in-app updater
- a signed Windows x64 Inno Setup installer
- a portable Windows x64 ZIP containing the signed `Studio Duo.exe`
- `SHA256SUMS.txt` for download verification
- `update-manifest.json` for in-app discovery and verified downloads

## Prerequisites

The release maintainer needs:

- a clean checkout of the `main` branch
- permission to push `main` and tags to the repository
- Git, CMake 3.25 or newer, and the normal Studio Duo build toolchain

Before the first signed Windows release, configure these GitHub Actions
repository secrets:

- `WINDOWS_SIGNING_CERTIFICATE_BASE64`: the Base64-encoded contents of a trusted
  Authenticode code-signing certificate in PFX format
- `WINDOWS_SIGNING_CERTIFICATE_PASSWORD`: the PFX password

For example, encode the PFX without line breaks:

```sh
base64 < studio-duo-signing.pfx | tr -d '\n'
```

Never commit the PFX or its password. The release workflow fails instead of
publishing unsigned Windows binaries when either secret is unavailable.

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

For the next patch release:

```sh
./scripts/release.sh 0.1.1
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
is published. Windows publication also requires successful Authenticode signing
and verification of both `Studio Duo.exe` and the installer. The installer
embeds the current Microsoft Visual C++ x64 Redistributable, verifies its
Microsoft signature while packaging, and installs it only when the installed
runtime is older. The publication job generates `update-manifest.json` from the
finished macOS updater ZIP and Windows installer, including their exact release
URLs, byte sizes, and SHA-256 checksums. GitHub automatically generates release
notes from the merged changes since the previous tag.

The updater introduces no new release secret or signing system. macOS updates
use the same ad-hoc-signed bundle produced by the normal build and do not
require Apple notarization or a separate update-signing key. Windows updates
reuse the existing installer and its permanent Inno Setup application ID; no
Windows service or updater-specific credential is required.

## Install released artifacts

Download artifacts from the repository's
[Releases](https://github.com/mbianchidev/studio-duo/releases) page.

On macOS, open the DMG and drag **Studio Duo** into **Applications**. The current
pipeline applies an ad hoc application signature but does not use an Apple
Developer ID or notarization, so macOS may require approval in **System
Settings > Privacy & Security** on first launch. Later updates are discovered
and downloaded inside Studio Duo. **Restart and Update** replaces the installed
bundle when the `.app` and its parent directory are writable.

On Windows, run
`Studio-Duo-<version>-Windows-x64-Setup.exe`. It installs to
`C:\Program Files\Studio Duo`, creates a Start Menu shortcut, optionally creates
a desktop shortcut, and registers an uninstaller in **Settings > Installed
apps**. The permanent installer application ID makes later versions upgrade the
same installation. Uninstalling or upgrading does not remove user data under
`%APPDATA%\Studio Duo`.

The ZIP remains available for portable use: extract it to a writable directory
and run `Studio Duo.exe`. Neither Windows package currently associates Explorer
with `.studioduo` directory packages.

Verify either download against `SHA256SUMS.txt` before installation.
