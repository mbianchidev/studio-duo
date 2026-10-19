#!/bin/sh
set -eu

if [ "$#" -lt 7 ] || [ "$#" -gt 8 ]; then
    echo "Invalid Studio Duo update helper arguments." >&2
    exit 1
fi
pid="$1"
archive="$2"
target="$3"
log_file="$4"
expected_version="$5"
expected_bundle_id="$6"
swap_helper="$7"
mode="${8:-}"
case "$pid" in ''|*[!0-9]*) echo "Invalid parent PID." >&2; exit 1;; esac
case "$mode" in ''|--no-relaunch) ;; *) echo "Invalid relaunch mode." >&2; exit 1;; esac
for path in "$archive" "$target" "$log_file" "$swap_helper"; do
    case "$path" in /*) ;; *) echo "Update paths must be absolute." >&2; exit 1;; esac
done

exec >>"$log_file" 2>&1
echo "Applying Studio Duo $expected_version update"
remaining=600
while /bin/kill -0 "$pid" 2>/dev/null; do
    if [ "$remaining" -eq 0 ]; then
        echo "The application did not quit within 120 seconds; update cancelled without replacement."
        exit 1
    fi
    remaining=$((remaining - 1))
    /bin/sleep 0.2
done

parent=$(/usr/bin/dirname "$target")
lock="$parent/.studio-duo-update.lock"
if ! /bin/mkdir "$lock"; then
    echo "Another update or interrupted-update recovery owns the installation lock; no files changed."
    exit 1
fi
stage=""
original_identity=""
update_complete=0
preserve_stage=0

cleanup() {
    result=$?
    trap - EXIT HUP INT TERM
    if [ "$update_complete" -eq 0 ] && [ -n "$stage" ] && [ -n "$original_identity" ]; then
        candidate="$stage/Studio Duo.app"
        if [ -d "$candidate" ] && [ ! -d "$target" ]; then
            echo "The installed directory is unavailable; preserving $stage and the installation lock."
            preserve_stage=1
            result=1
        elif [ -d "$candidate" ] && [ -d "$target" ]; then
            candidate_identity=$(/usr/bin/stat -f '%d:%i' "$candidate") || candidate_identity=""
            if [ -z "$candidate_identity" ]; then
                echo "Could not identify the recovery application; preserving $stage for manual recovery."
                preserve_stage=1
                result=1
            elif [ "$candidate_identity" = "$original_identity" ]; then
                echo "Restoring the previous application after interrupted or failed replacement."
                if ! "$swap_helper" "$candidate" "$target"; then
                    echo "Rollback failed; preserving both application directories at $target and $stage."
                    preserve_stage=1
                    result=1
                fi
            fi
        fi
    fi
    if [ -n "$stage" ] && [ "$preserve_stage" -eq 0 ]; then
        if ! /bin/rm -rf "$stage"; then
            echo "Recovery-directory cleanup failed; preserving the installation lock."
            preserve_stage=1
            result=1
        fi
    fi
    if [ "$preserve_stage" -eq 0 ]; then
        /bin/rmdir "$lock" || result=1
    fi
    if [ "$result" -ne 0 ] && [ "$mode" != --no-relaunch ] && [ -d "$target" ]; then
        /usr/bin/open "$target" || echo "The preserved application could not be reopened; see this log."
    fi
    exit "$result"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

if [ ! -d "$target" ]; then
    echo "The installed application directory is missing; no replacement attempted."
    exit 1
fi
original_identity=$(/usr/bin/stat -f '%d:%i' "$target")
stage=$(/usr/bin/mktemp -d "$parent/.studio-duo-update.XXXXXX")
candidate="$stage/Studio Duo.app"
/usr/bin/ditto -x -k "$archive" "$stage"
if [ ! -d "$candidate" ]; then
    echo "The update archive does not contain Studio Duo.app."
    exit 1
fi
actual_version=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$candidate/Contents/Info.plist")
actual_bundle_id=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$candidate/Contents/Info.plist")
if [ "$actual_version" != "$expected_version" ] || [ "$actual_bundle_id" != "$expected_bundle_id" ]; then
    echo "The staged application's version or bundle identity does not match the verified release."
    exit 1
fi
/usr/bin/codesign --verify --deep --strict "$candidate"

"$swap_helper" "$candidate" "$target"
if [ "$mode" != --no-relaunch ]; then
    /usr/bin/open "$target"
fi
update_complete=1
/bin/rm -f "$archive"
echo "Studio Duo $expected_version update applied; previous application cleanup follows."
