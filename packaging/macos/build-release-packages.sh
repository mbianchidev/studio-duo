#!/usr/bin/env bash
set -euo pipefail

fail() {
    printf 'macOS packaging failed: %s\n' "$*" >&2
    exit 1
}

if [[ $# -lt 3 || $# -gt 4 ]]; then
    fail "Usage: $0 VERSION APP OUTPUT_DIRECTORY [--universal]"
fi
version="$1"
app="$2"
output_directory="$3"
require_universal="${4:-}"
[[ "$(uname -s)" == Darwin ]] || fail "This script requires macOS."
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "VERSION must be MAJOR.MINOR.PATCH."
[[ -z "$require_universal" || "$require_universal" == --universal ]] || fail "Unknown packaging option."
[[ -d "$app" ]] || fail "The application bundle is missing: $app"
app="$(cd "$(dirname "$app")" && pwd)/$(basename "$app")"
mkdir -p "$output_directory"
output_directory="$(cd "$output_directory" && pwd)"
executable="$app/Contents/MacOS/Studio Duo"
[[ -x "$executable" ]] || fail "The application executable is missing."
swap_helper="$app/Contents/Helpers/studio-duo-update-swap"
[[ -x "$swap_helper" ]] || fail "The atomic application-exchange helper is missing."
bundle_version="$(plutil -extract CFBundleShortVersionString raw -o - "$app/Contents/Info.plist")"
[[ "$bundle_version" == "$version" ]] || fail "The application version does not match the package version."
codesign --verify --deep --strict "$app"
if [[ "$require_universal" == --universal ]]; then
    for architecture in arm64 x86_64; do
        lipo "$executable" -verify_arch "$architecture"
        lipo "$swap_helper" -verify_arch "$architecture"
    done
    architecture_label=universal
else
    architecture_label="$(lipo -archs "$executable")"
    [[ "$architecture_label" == arm64 || "$architecture_label" == x86_64 ]] \
        || fail "Native smoke packages must contain exactly one supported architecture."
fi
if pgrep -x 'Studio Duo' >/dev/null; then
    fail "Close the running Studio Duo application before installed-startup validation."
fi
os_version="$(sw_vers -productVersion)"
os_major="${os_version%%.*}"
[[ "$os_major" =~ ^[0-9]+$ ]] || fail "Could not determine the macOS tooling version."

work="$(mktemp -d "${TMPDIR:-/tmp}/studio-duo-macos-package.XXXXXX")"
mount="$work/mount"
mounted=false
cleanup() {
    result=$?
    trap - EXIT
    if [[ "$mounted" == true ]]; then
        if ! diskutil eject "$mount"; then
            printf 'Could not detach the package smoke-test volume: %s\n' "$mount" >&2
            result=1
        else
            mounted=false
        fi
    fi
    if [[ "$mounted" == true ]]; then
        printf 'Preserving the mounted smoke-test directory for safe cleanup: %s\n' "$work" >&2
        exit "$result"
    fi
    if ! rm -rf -- "$work"; then
        printf 'Could not remove the isolated packaging directory: %s\n' "$work" >&2
        result=1
    fi
    exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

name="Studio-Duo-$version-macOS-$architecture_label"
zip="$output_directory/$name.zip"
dmg="$output_directory/$name.dmg"
mkdir -p "$work/staging" "$mount" "$work/install" "$work/unpacked" "$work/user-data"
ditto "$app" "$work/staging/Studio Duo.app"
ln -s /Applications "$work/staging/Applications"
ditto -c -k --sequesterRsrc --keepParent "$app" "$zip"
if [[ "$os_major" -ge 26 ]]; then
    rm -f -- "$dmg"
    diskutil image create from --volumeName "Studio Duo $version" --format UDZO "$work/staging" "$dmg"
else
    hdiutil create -volname "Studio Duo $version" -srcfolder "$work/staging" -ov -format UDZO "$dmg"
fi
hdiutil verify "$dmg"
if [[ "$os_major" -ge 26 ]]; then
    diskutil image attach --readOnly --nobrowse --mountPoint "$mount" "$dmg" > "$work/mount.log"
else
    hdiutil attach -readonly -nobrowse -noautoopen -mountpoint "$mount" "$dmg" > "$work/mount.log"
fi
mounted=true
[[ -L "$mount/Applications" ]] || fail "The DMG Applications shortcut is missing."
ditto "$mount/Studio Duo.app" "$work/install/Studio Duo.app"
ditto -x -k "$zip" "$work/unpacked"
installed="$work/install/Studio Duo.app"
unzipped="$work/unpacked/Studio Duo.app"
for bundle in "$installed" "$unzipped"; do
    codesign --verify --deep --strict "$bundle"
    cmp -s "$executable" "$bundle/Contents/MacOS/Studio Duo" \
        || fail "A packaged application executable differs from the built executable."
    [[ -x "$bundle/Contents/Helpers/studio-duo-update-swap" ]] \
        && cmp -s "$swap_helper" "$bundle/Contents/Helpers/studio-duo-update-swap" \
        || fail "A packaged application-exchange helper is missing or changed."
    for license in \
        lame-COPYING.txt lame-NOTICE.txt lame-config.h \
        lame-4.0-source.tar.gz studio-duo-source.tar.gz \
        signalsmith-stretch-LICENSE.txt signalsmith-linear-LICENSE.txt; do
        [[ -s "$bundle/Contents/Resources/licenses/$license" ]] \
            || fail "Installed dependency source/license material is missing or empty: $license"
    done
done

printf '{"syntheticUserData":true}\n' > "$work/user-data/preserved.json"
if ! /usr/bin/perl -e 'alarm 30; exec @ARGV or die "Could not launch installed smoke test: $!\n";' \
    "$installed/Contents/MacOS/Studio Duo" --startup-self-test > "$work/startup.log" 2>&1; then
    cat "$work/startup.log" >&2
    cp "$work/startup.log" "$output_directory/$name-install.log"
    fail "The installed application failed its bounded driver-free startup check."
fi
cp "$work/startup.log" "$output_directory/$name-install.log"
rm -rf -- "$installed"
[[ ! -e "$installed" ]] || fail "The smoke installation was not removed."
[[ -s "$work/user-data/preserved.json" ]] || fail "Removing the application removed unrelated user data."
printf 'Validated DMG, updater ZIP, installed startup, and isolated app removal: %s\n' "$name"
