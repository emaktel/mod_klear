#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# live_smoke.sh — end-to-end smoke test against a running FreeSWITCH.
#
# Uses `fs_cli` to originate a null/nothing session with an inline echo
# app (bypassing whatever dialplan config your FS has), attaches mod_klear
# to it, lets a few seconds of audio flow through, and verifies from the
# FS log that the media bug callback actually fired. This is the fastest
# way to confirm that the full stack — module load, app registration,
# media bug attach, per-frame processing, resampling path — works on your
# actual FS install.
#
# Requires:
#   * mod_klear installed and loaded (`fs_cli -x "load mod_klear"`)
#   * fs_cli available in PATH
#   * Read access to /var/log/freeswitch/freeswitch.log (or $FS_LOG)
#
# Exit codes:
#   0 — success, klear processed at least one frame
#   1 — couldn't originate a test channel
#   2 — klear attach failed
#   3 — no frames flowed through the callback
set -euo pipefail

FS_LOG="${FS_LOG:-/var/log/freeswitch/freeswitch.log}"
MIN_FRAMES="${MIN_FRAMES:-10}"
DURATION="${DURATION:-2}"

say() { printf '[live_smoke] %s\n' "$*"; }

say "originating null/nothing &echo"
ORIG=$(fs_cli -x "originate null/nothing &echo" 2>&1)
if [[ "$ORIG" != +OK* ]]; then
    printf '!! originate failed: %s\n' "$ORIG" >&2
    exit 1
fi
UUID=$(echo "$ORIG" | awk '{print $2}')
say "channel uuid=$UUID"

# Wait briefly for the channel to answer and codec to settle.
sleep 0.5

say "attaching klear"
KS=$(fs_cli -x "klear $UUID start" 2>&1)
if [[ "$KS" != +OK* ]]; then
    printf '!! klear start failed: %s\n' "$KS" >&2
    fs_cli -x "uuid_kill $UUID" >/dev/null 2>&1 || true
    exit 2
fi

say "flowing audio for ${DURATION}s"
sleep "$DURATION"

say "detaching and killing channel"
fs_cli -x "klear $UUID stop" >/dev/null 2>&1 || true
fs_cli -x "uuid_kill $UUID"  >/dev/null 2>&1 || true

# Give FS a moment to flush the close event and stats log.
sleep 0.5

say "parsing close log for $UUID"
LINE=$(grep -a "$UUID" "$FS_LOG" 2>/dev/null | grep -a "klear: close" | tail -1 || true)
if [[ -z "$LINE" ]]; then
    printf '!! no klear close log line found for %s\n' "$UUID" >&2
    printf '   check %s, and ensure console loglevel is INFO or higher\n' "$FS_LOG" >&2
    exit 3
fi
echo "   $LINE"

FRAMES=$(echo "$LINE" | sed -n 's/.*frames_capture=\([0-9]*\).*/\1/p')
if [[ -z "$FRAMES" || "$FRAMES" -lt "$MIN_FRAMES" ]]; then
    printf '!! too few frames processed: %s (min %s)\n' "$FRAMES" "$MIN_FRAMES" >&2
    exit 3
fi

say "PASS: $FRAMES capture frames processed through mod_klear on live FS channel"
exit 0
