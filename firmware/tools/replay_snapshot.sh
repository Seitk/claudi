#!/usr/bin/env bash
# replay_snapshot.sh — drive the claudi device through every pet state without
# Claude Code, by POSTing canned snapshots (the same shape claudi_hook.py sends)
# and reading back GET /status. Acceptance test for the ingest + state engine.
#
# Usage:
#   tools/replay_snapshot.sh [base_url]      # default http://claudi.local
#   BASE=http://192.168.1.50 tools/replay_snapshot.sh
set -euo pipefail

BASE="${1:-${BASE:-http://claudi.local}}"
PASS=0
FAIL=0

post() { curl -fsS -m 3 -H 'Content-Type: application/json' -d "$2" "$BASE/snapshot" >/dev/null; }
effective() { curl -fsS -m 3 "$BASE/status" | sed -n 's/.*"effective":"\([a-z]*\)".*/\1/p'; }

check() { # name  json  expected_effective
    local name="$1" body="$2" want="$3" got
    post "$name" "$body"
    sleep 0.3
    got="$(effective)"
    if [ "$got" = "$want" ]; then
        printf '  [ok]   %-22s -> %s\n' "$name" "$got"; PASS=$((PASS+1))
    else
        printf '  [FAIL] %-22s -> %s (want %s)\n' "$name" "$got" "$want"; FAIL=$((FAIL+1))
    fi
}

echo "claudi snapshot replay against $BASE"
echo "(make sure the device is on the network: curl $BASE/status)"
echo

check "idle"        '{"running":0,"waiting":0,"total":0,"msg":"idle"}'                                   idle
check "working"     '{"running":2,"waiting":0,"total":5,"msg":"using Bash"}'                             working
check "attention"   '{"running":1,"waiting":1,"total":6,"prompt":{"id":"x","tool":"Bash","hint":"rm -rf /tmp/foo"}}' attention
check "overlay-idea" '{"running":1,"total":7,"overlay":"idea","overlay_ms":2500,"msg":"wrote file"}'      idea
echo "  ... waiting 3s for the idea overlay to expire ..."
sleep 3
ov_after="$(effective)"
if [ "$ov_after" = "working" ]; then
    printf '  [ok]   %-22s -> %s\n' "overlay-revert" "$ov_after"; PASS=$((PASS+1))
else
    printf '  [FAIL] %-22s -> %s (want working)\n' "overlay-revert" "$ov_after"; FAIL=$((FAIL+1))
fi

# Staleness: send an idle snapshot, then wait past the 30s threshold.
check "pre-stale-idle" '{"running":0,"waiting":0,"total":8,"msg":"quiet"}'                               idle
echo "  ... waiting 31s for staleness -> sleepy ..."
sleep 31
st_after="$(effective)"
if [ "$st_after" = "sleepy" ]; then
    printf '  [ok]   %-22s -> %s\n' "stale-sleepy" "$st_after"; PASS=$((PASS+1))
else
    printf '  [FAIL] %-22s -> %s (want sleepy)\n' "stale-sleepy" "$st_after"; FAIL=$((FAIL+1))
fi

echo
echo "replay: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
