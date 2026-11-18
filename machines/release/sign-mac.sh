#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Sign the kits' macOS binaries and libraries with the Developer ID and have
# Apple notarize them, so that macOS runs a downloaded kit without asking.
# They get the hardened runtime and entitlements.plist: TCG needs allow-jit.
# DIR is OUT/macos-arm64 of build-mac.sh, or bin/macos-arm64 of an unpacked
# kit. Signing changes the files in place.
# -c only checks DIR: each Mach-O file is signed with a Developer ID, with
# the hardened runtime and a timestamp, and notarized (this asks Apple).
# The identity comes from DEV_ID and NOTARY_PROFILE, else from .env next to
# this script, else from that .env in the main worktree. Git ignores .env;
# .env.example shows what goes in it. SIGN=adhoc signs ad hoc instead, for
# builds that never go into a kit.
# usage: sign-mac.sh [-c] DIR
set -e
R=$(cd "$(dirname "$0")" && pwd)
check=
[ "$1" = -c ] && { check=1; shift; }
[ $# -eq 1 ] && [ -d "$1" ] || { echo "usage: $0 [-c] DIR" >&2; exit 1; }
D=$(cd "$1" && pwd -P)
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
: > "$t/libs"; : > "$t/bins"
find "$D" -type f | sort | while IFS= read -r f; do
    case $(file -b "$f") in
    Mach-O*executable*) echo "$f" >> "$t/bins" ;;
    Mach-O*) echo "$f" >> "$t/libs" ;;
    esac
done
[ -s "$t/bins" ] || { echo "$0: no Mach-O binary in $D" >&2; exit 1; }

# codesign -f OPTIONS FILE, quiet unless it fails
sig() {
    codesign -f "$@" 2> "$t/err" || { cat "$t/err" >&2; exit 1; }
}

# the value of KEY in the first .env that has it (KEY := value, as the
# Makefiles of other projects read it, or KEY=value)
conf() {
    for e in "$R/.env" "$main/machines/release/.env"; do
        [ -f "$e" ] || continue
        v=$(sed -n "s/^[[:space:]]*$1[[:space:]]*:\{0,1\}=[[:space:]]*//p" "$e" |
            tail -1 | sed 's/[[:space:]]*$//; s/^"\(.*\)"$/\1/')
        [ -n "$v" ] && { printf '%s\n' "$v"; return; }
    done
}

if [ -z "$check" ] && [ "$SIGN" = adhoc ]; then
    while IFS= read -r f; do sig -s - "$f"; done < "$t/libs"
    while IFS= read -r f; do
        sig -s - --entitlements "$R/entitlements.plist" "$f"
    done < "$t/bins"
    echo "sign: ad hoc, NOT FOR KITS: the pack scripts refuse these binaries"
    exit 0
fi

if [ -z "$check" ]; then
    main=$(git -C "$R" rev-parse --path-format=absolute --git-common-dir 2>/dev/null) || main=
    main=${main%/.git}
    DEV_ID=${DEV_ID:-$(conf DEV_ID)}
    NOTARY_PROFILE=${NOTARY_PROFILE:-$(conf NOTARY_PROFILE)}
    if [ -z "$DEV_ID" ] || [ -z "$NOTARY_PROFILE" ]; then
        echo "sign: no DEV_ID or NOTARY_PROFILE: copy $R/.env.example to $R/.env and fill it in, or set SIGN=adhoc for a build that never goes into a kit" >&2
        exit 1
    fi
    security find-identity -v -p codesigning | grep -qF "\"$DEV_ID\"" ||
        { echo "sign: the DEV_ID identity is not in the keychain" >&2; exit 1; }
    n=0
    while IFS= read -r f; do
        sig -s "$DEV_ID" -o runtime --timestamp "$f"; n=$((n + 1))
    done < "$t/libs"
    while IFS= read -r f; do
        sig -s "$DEV_ID" -o runtime --timestamp \
            --entitlements "$R/entitlements.plist" "$f"; n=$((n + 1))
    done < "$t/bins"
    echo "sign: $n files with the Developer ID"
    # A bare binary can't be stapled: Gatekeeper looks its ticket up online
    # the first time it runs.
    ditto -c -k --keepParent "$D" "$t/mac.zip"
    xcrun notarytool submit "$t/mac.zip" --keychain-profile "$NOTARY_PROFILE" \
        --wait --timeout 2h --output-format json > "$t/n.json" 2> "$t/n.err" || true
    id=$(plutil -extract id raw -o - "$t/n.json" 2>/dev/null) || id=
    st=$(plutil -extract status raw -o - "$t/n.json" 2>/dev/null) || st=
    echo "notarize: ${st:-no answer} $id"
    if [ "$st" != Accepted ]; then
        cat "$t/n.err" "$t/n.json" >&2
        [ -z "$id" ] ||
            xcrun notarytool log "$id" --keychain-profile "$NOTARY_PROFILE" >&2 || true
        exit 1
    fi
    # Each file must be in the ticket. Asking this Mac (-c) right away can
    # fail: it caches a "no" for a while.
    xcrun notarytool log "$id" --keychain-profile "$NOTARY_PROFILE" "$t/log.json" > /dev/null
    k=$(plutil -extract ticketContents raw -o - "$t/log.json")
    while [ "$k" -gt 0 ]; do
        k=$((k - 1))
        plutil -extract ticketContents.$k.cdhash raw -o - "$t/log.json"
    done > "$t/ticket"
    cat "$t/libs" "$t/bins" | while IFS= read -r f; do
        h=$(codesign -dvvv "$f" 2>&1 | sed -n 's/^CDHash=//p')
        grep -qx "$h" "$t/ticket" || { echo "check mac: ${f#"$D"/}: not in the ticket"; echo > "$t/bad"; }
    done
    [ ! -f "$t/bad" ] || { echo "check mac: FAILED: Apple's ticket lacks these files"; exit 1; }
    echo "check mac: OK, all $n files are in the ticket"
    exit 0
fi

cat "$t/libs" "$t/bins" > "$t/all"
bad=0 late=0 n=0
while IFS= read -r f; do
    m=${f#"$D"/} n=$((n + 1))
    if ! codesign --verify --strict "$f" 2>/dev/null; then
        echo "check mac: $m: no valid signature"; bad=1; continue
    fi
    i=$(codesign -dvv "$f" 2>&1)
    case $i in
    *"Authority=Developer ID Application:"*) ;;
    *) echo "check mac: $m: not signed with a Developer ID"; bad=1; continue ;;
    esac
    case $i in *"Timestamp="*) ;; *) echo "check mac: $m: no timestamp"; bad=1 ;; esac
    case $i in *"(runtime)"*) ;; *) echo "check mac: $m: no hardened runtime"; bad=1 ;; esac
    codesign --verify --check-notarization -R=notarized "$f" 2>/dev/null ||
        { echo "check mac: $m: not notarized"; bad=1 late=1; }
done < "$t/all"
while IFS= read -r f; do
    codesign -d --entitlements - "$f" 2>/dev/null | grep -q com.apple.security.cs.allow-jit ||
        { echo "check mac: ${f#"$D"/}: no allow-jit entitlement, which TCG needs"; bad=1; }
done < "$t/bins"
if [ $bad = 0 ]; then
    echo "check mac: OK, $n files signed and notarized"
    exit 0
fi
# A Mac that asked before Apple accepted them keeps that "no" for about
# 5 minutes.
[ $late = 1 ] && echo "check mac: if Apple accepted them minutes ago, try again in 5 minutes"
echo "check mac: FAILED: sign them with sh $R/sign-mac.sh $D"
exit 1
