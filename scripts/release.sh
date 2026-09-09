#!/usr/bin/env bash

set -euo pipefail

readonly remote="origin"
readonly main_branch="main"
readonly version_file="VERSION"

usage() {
    cat <<'EOF'
Usage: ./scripts/release.sh [VERSION]

Create a Studio Duo release from the latest main branch.

VERSION must be a stable semantic version such as 0.2.0. A leading "v" is
accepted. When VERSION is omitted, the current patch version is incremented.
EOF
}

fail() {
    printf 'release: %s\n' "$*" >&2
    exit 1
}

is_semantic_version() {
    [[ "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
}

is_greater_version() {
    local candidate_major candidate_minor candidate_patch
    local current_major current_minor current_patch

    IFS=. read -r candidate_major candidate_minor candidate_patch <<< "$1"
    IFS=. read -r current_major current_minor current_patch <<< "$2"

    (( candidate_major > current_major )) && return 0
    (( candidate_major < current_major )) && return 1
    (( candidate_minor > current_minor )) && return 0
    (( candidate_minor < current_minor )) && return 1
    (( candidate_patch > current_patch ))
}

if (( $# > 1 )); then
    usage >&2
    exit 2
fi

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" \
    || fail "run this script inside the Studio Duo repository"
cd "$repo_root"

[[ -f "$version_file" ]] || fail "$version_file is missing"
[[ "$(git branch --show-current)" == "$main_branch" ]] \
    || fail "check out $main_branch before creating a release"
[[ -z "$(git status --porcelain)" ]] \
    || fail "the working tree must be clean before creating a release"

git fetch "$remote" "$main_branch" --tags
git pull --ff-only "$remote" "$main_branch"

current_version="$(tr -d '[:space:]' < "$version_file")"
is_semantic_version "$current_version" \
    || fail "$version_file contains an invalid version: $current_version"

requested_version="${1:-}"
requested_version="${requested_version#v}"

pending_tag="v$current_version"
pending_release=false
if git show-ref --verify --quiet "refs/tags/$pending_tag" \
    && [[ "$(git rev-list -n 1 "$pending_tag")" == "$(git rev-parse HEAD)" ]] \
    && ! git ls-remote --exit-code --tags "$remote" "refs/tags/$pending_tag" >/dev/null 2>&1; then
    if [[ -z "$requested_version" || "$requested_version" == "$current_version" ]]; then
        version="$current_version"
        tag="$pending_tag"
        pending_release=true
        printf 'Retrying unpublished release %s.\n' "$tag"
    fi
fi

if [[ "$pending_release" == false ]]; then
    [[ "$(git rev-parse HEAD)" == "$(git rev-parse "$remote/$main_branch")" ]] \
        || fail "$main_branch contains commits that are not on $remote/$main_branch"

    if [[ -z "$requested_version" ]]; then
        IFS=. read -r major minor patch <<< "$current_version"
        version="$major.$minor.$((patch + 1))"
    else
        version="$requested_version"
    fi

    is_semantic_version "$version" \
        || fail "VERSION must use stable semantic versioning, for example 0.2.0"
    if [[ "$version" != "$current_version" ]]; then
        is_greater_version "$version" "$current_version" \
            || fail "$version must not be older than the current version $current_version"
    fi

    tag="v$version"
    ! git show-ref --verify --quiet "refs/tags/$tag" \
        || fail "tag $tag already exists"
    ! git ls-remote --exit-code --tags "$remote" "refs/tags/$tag" >/dev/null 2>&1 \
        || fail "tag $tag already exists on $remote"

    version_changed=false
    if [[ "$version" != "$current_version" ]]; then
        original_version="$current_version"
        version_committed=false
        version_changed=true
        restore_version_on_error() {
            local status=$?
            if (( status != 0 )) && [[ "$version_committed" == false ]]; then
                if ! git restore --staged -- "$version_file" >/dev/null 2>&1; then
                    printf 'release: warning: could not unstage %s\n' "$version_file" >&2
                fi
                printf '%s\n' "$original_version" > "$version_file"
                printf 'release: restored %s after the failed release attempt\n' "$version_file" >&2
            fi
            exit "$status"
        }
        trap restore_version_on_error EXIT

        printf '%s\n' "$version" > "$version_file"
    fi
fi

build_dir="build/release-validation"
cmake -S . -B "$build_dir" -DSTUDIO_DUO_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --config Release --parallel
ctest --test-dir "$build_dir" --build-config Release --output-on-failure

if [[ "$pending_release" == false ]]; then
    if [[ "$version_changed" == true ]]; then
        git diff --check -- "$version_file"
        git add -- "$version_file"
        git commit --no-gpg-sign -m "chore: release $tag"
        version_committed=true
    fi
    git tag -a "$tag" -m "Studio Duo $tag"
fi

git push --atomic "$remote" "HEAD:refs/heads/$main_branch" "refs/tags/$tag"

if [[ "${version_changed:-false}" == true ]]; then
    trap - EXIT
fi

printf 'Release %s pushed. GitHub Actions will publish the installers and checksums.\n' "$tag"
