#!/usr/bin/env bash
#
# SoulX Android Emulator - the next release within a month: YY.M.PATCH + 1 (e.g. 26.10.0 -> 26.10.1)
#
# The year and month stay those of the version being patched; start a new month with bump-version-calendar.sh.
#
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

usage() {
    echo "Usage: scripts/versioning/bump-version-patch.sh [--dry-run]"
    echo
    echo "Raises the patch number (YY.M.PATCH -> YY.M.PATCH+1), commits and tags it."
    echo
    usage_common
}

parse_common_args "$@"
preflight

IFS=. read -r yy mm patch <<< "$(current_version)"
apply "$yy.$mm.$((patch + 1))"
