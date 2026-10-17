#!/usr/bin/env bash
#
# SoulX Android Emulator - start this month's version: YY.M.0 (e.g. 26.10.0 in October 2026)
#
# Use it for the first release of a month; later releases in the same month use bump-version-patch.sh.
# SOULX_RELEASE_DATE=YYYY-MM overrides the month (for tests).
#
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

usage() {
    echo "Usage: scripts/versioning/bump-version-calendar.sh [--dry-run]"
    echo
    echo "Sets the version to YY.M.0 for the current month, commits and tags it."
    echo
    usage_common
}

parse_common_args "$@"

month="${SOULX_RELEASE_DATE:-$(date +%Y-%m)}"
[[ "$month" =~ ^20([0-9]{2})-([0-9]{2})$ ]] || die "SOULX_RELEASE_DATE must be YYYY-MM (got: $month)"
yy=$((10#${BASH_REMATCH[1]}))
mm=$((10#${BASH_REMATCH[2]}))
(( mm >= 1 && mm <= 12 )) || die "no such month: $month"

old="$(current_version)"
IFS=. read -r o1 o2 _ <<< "$old"
if (( o1 == yy && o2 == mm )); then
    die "$old is already this month's version: use scripts/versioning/bump-version-patch.sh"
fi
if (( o1 > yy || (o1 == yy && o2 > mm) )); then
    die "$old is newer than $yy.$mm (check the date)"
fi
preflight

apply "$yy.$mm.0"
