# Releasing Studio Duo

Studio Duo releases are created from semantic version tags on `main`. A release
tag starts the GitHub Actions release workflow, which builds and tests the
application before publishing:

- a universal macOS DMG for Apple Silicon and Intel Macs
- a universal macOS ZIP used only by the in-app updater
- a signed Windows x64 Inno Setup installer
- a portable Windows x64 ZIP containing the signed `Studio Duo.exe`
- a Windows x64 symbols ZIP for crash diagnostics
- `SHA256SUMS.txt` for download verification
- `update-manifest.json` for in-app discovery and verified downloads

## Prerequisites

The release maintainer needs:

- a clean checkout of the `main` branch
- permission to push `main` and tags to the repository
- Git, CMake 3.25 or newer, and the normal Studio Duo build toolchain

Before the first signed Windows release, configure these GitHub Actions
repository secrets:

- `WINDOWS_SIGNING_CERTIFICATE_BASE64`: the Base64-encoded contents of a
  self-signed or publicly trusted Authenticode code-signing certificate in PFX format
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
is published. The macOS build uses the same two-worker Ninja configuration and
architecture-aware sccache backend as CI, so default-branch compilation can be
reused by tags. The application and standalone plugins remain universal; test-only
executables and fixtures are native to the runner, matching the architecture
that actually executes the complete suite. See
[universal build configuration](development.md#universal-macos-builds-and-compilation-caching).
The release build requests only the signed app and complete test targets,
including their fixture dependencies. Standalone VST3/AU exports are not in the
release archives and remain covered by CI's full build instead.
Windows publication also requires successful Authenticode signing,
`signtool verify /pa /v`, and an exact signer-thumbprint match for both
`Studio Duo.exe` and the installer. CI exercises the same verification path
with an ephemeral test certificate.

The hosted Windows workflows explicitly pass `-TrustSigningCertificate` to
temporarily trust the configured certificate in the disposable runner's
LocalMachine Root store. Hosted Windows runners are elevated; this store avoids
the protected CurrentUser Root confirmation dialog in headless sessions.
Only the public certificate enters that store; the private key
remains ephemeral. Packaging preserves any pre-existing trust entry and removes
only the entry it added, including on failure. The switch is rejected outside
disposable GitHub-hosted runners; local packaging uses normal Windows trust.
This supports the existing self-signed publisher while still checking signature
integrity and signer identity. It does not establish public-CA trust or
SmartScreen reputation, and no certificate is installed on end-user machines.
Native packaging tools retain their bounded deadlines and report stdout/stderr
in the workflow log so signing and verification failures include their cause.

Both native readers must also open the synthetic packages produced by both
platforms before tagged publication. JUnit and numeric hardening evidence are
retained in workflow artifacts. The signed installer is installed into a
temporary directory on the disposable runner; the installed executable must
pass its driver-free main-window startup check. The smoke test also exercises
same-version replacement and checks that uninstall removes application
executables while preserving a synthetic user-data sentinel. This does not
qualify a different-version upgrade.

The installer embeds the current Microsoft Visual C++ x64 Redistributable, verifies its
Microsoft signature while packaging, and installs it only when the installed
runtime is older. The publication job generates `update-manifest.json` from the
finished macOS updater ZIP and Windows installer, including their exact release
URLs, byte sizes, and SHA-256 checksums. GitHub automatically generates release
notes from the merged changes since the previous tag.

Publication downloads only artifacts matching `*-release` and uploads an explicit
list of the five versioned packages plus `SHA256SUMS.txt` and
`update-manifest.json`. Every listed asset must be a regular file before
publication starts. Test logs, hardening evidence, and native exchange fixtures
remain in workflow artifacts and are not uploaded to the GitHub Release.

macOS packaging shares `packaging/macos/build-release-packages.sh` between CI
and releases. It verifies the bundle version/development signature, packages
the DMG and updater ZIP, checks mounted/extracted binary equality and
license/source material, then installs and starts a private copy with a
30-second driver-free deadline. Cleanup removes only its own temporary install
and detaches its own volume; it refuses to run while Studio Duo is already open.
Tagged builds additionally require both ARM64 and x86_64 slices.
macOS 26 and later use the current `diskutil image` creation/attachment APIs;
older supported build hosts retain the compatible `hdiutil` path. Image
checksums are verified before installation.
The build's development-signature target runs after all bundle resource copies;
packaging verifies that completed seal instead of re-signing an invalid bundle
and hiding an ordering failure.

```sh
./packaging/macos/build-release-packages.sh \
  "$(cat VERSION)" \
  "build/StudioDuo_artefacts/Release/Studio Duo.app" \
  build/phase6-packages
```

Use `--universal` as the fourth argument for a universal Release build. These
checks do not imply Developer ID/notarization, clean-machine Gatekeeper behavior,
physical hardware operation, or actual updater/different-version upgrade passes.
The [candidate compatibility matrix](release-readiness.md) records those
release-blocking qualifications separately. Keep Phase 6 open until they have
relevant candidate evidence.

The updater introduces no new release secret or signing system. macOS updates
use the same ad-hoc-signed bundle produced by the normal build and do not
require Apple notarization or a separate update-signing key. Windows updates
reuse the existing installer and its permanent Inno Setup application ID; no
Windows service or updater-specific credential is required. Portable ZIP
builds do not update in place; the installed Setup build provides the automatic
update path.

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
