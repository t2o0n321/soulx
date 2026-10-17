# SoulX Android Emulator - shared by bump-version-calendar.sh and bump-version-patch.sh (source it, do not run it).
#
# Versions are YY.M.PATCH: the two-digit year, the month without a leading zero, and a patch number that starts
# at 0 each month (26.10.0, 26.10.1, 26.11.0 ...). macOS needs at most three integers without leading zeros.
#
# The version lives in the VERSION file at the top of the repository (CMake, packaging and releases read it).
# A bump writes VERSION, turns CHANGELOG.md's "## Unreleased" section into "## v<version> (<date>)" with a new
# empty "## Unreleased" above it, commits, and makes the tag v<version>. It does not push: pushing the tag publishes the release (.github/workflows/release.yml).

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
version_file="$repo_root/VERSION"
changelog="$repo_root/CHANGELOG.md"
dry_run=0

die() {
    echo "error: $*" >&2
    exit 1
}

usage_common() {
    cat <<'EOF'
  --dry-run   print what would change, change nothing
  -h, --help  this help

The working tree must be clean and CHANGELOG.md must have an "## Unreleased" section with the changes.
Afterwards publish with: git push origin main v<version>
EOF
}

current_version() {
    local v
    v="$(tr -d '[:space:]' < "$version_file" 2>/dev/null || true)"
    [[ "$v" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "VERSION must hold one version like 26.10.0"
    echo "$v"
}

# The text of the "## Unreleased" section (empty when there is none or it has no entries).
unreleased_notes() {
    awk '/^## /{ if (found) exit; found = ($2 == "Unreleased"); next } found' "$changelog"
}

preflight() {
    cd "$repo_root"
    [[ -f "$changelog" ]] || die "CHANGELOG.md is missing"
    grep -q '^## Unreleased' "$changelog" || die "CHANGELOG.md has no \"## Unreleased\" section"
    [[ -n "$(unreleased_notes | tr -d '[:space:]')" ]] ||
        die "the \"## Unreleased\" section of CHANGELOG.md is empty: describe the changes first"
    if [[ "$dry_run" == 0 ]]; then
        [[ -z "$(git status --porcelain --untracked-files=no)" ]] || die "commit or stash your changes first"
    fi
}

# apply NEW_VERSION: write, commit and tag it.
apply() {
    local old new today tag
    old="$(current_version)"
    new="$1"
    tag="v$new"
    today="$(date +%Y-%m-%d)"
    git -C "$repo_root" rev-parse -q --verify "refs/tags/$tag" >/dev/null && die "tag $tag already exists"
    echo "$old -> $new"
    if [[ "$dry_run" == 1 ]]; then
        echo "(dry run) would write \"$new\" to VERSION, rename \"## Unreleased\" to \"## v$new ($today)\","
        echo "          commit \"Release $tag\" and tag $tag"
        return
    fi
    echo "$new" > "$version_file"
    awk -v v="$new" -v d="$today" '
        !done && /^## Unreleased/ { print "## Unreleased\n\n## v" v " (" d ")"; done = 1; next }
        { print }
    ' "$changelog" > "$changelog.tmp" && mv "$changelog.tmp" "$changelog"
    [[ "$(current_version)" == "$new" ]] || die "could not write $version_file"
    git -C "$repo_root" add "$version_file" "$changelog"
    git -C "$repo_root" commit -q -m "Release $tag"
    git -C "$repo_root" tag -a "$tag" -m "SoulX $new"
    echo "Committed and tagged $tag. Publish it with:"
    echo "  git push origin main $tag"
}

parse_common_args() {
    for arg in "$@"; do
        case "$arg" in
            --dry-run) dry_run=1 ;;
            -h|--help) usage; exit 0 ;;
            *) echo "error: unknown option: $arg" >&2; usage >&2; exit 2 ;;
        esac
    done
}
