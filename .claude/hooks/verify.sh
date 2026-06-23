#!/usr/bin/env bash
# Local verification for the Claudi pet-state hook bridge. No hardware required.
# Exits non-zero if any check fails.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY="${CLAUDI_PYTHON:-python3}"
fail=0

echo "== 1. byte-compile =="
if "$PY" -m py_compile "$HERE/claudi_hook.py"; then
  echo "  ok"
else
  echo "  FAIL"; fail=1
fi

echo "== 2. mapping self-test =="
if "$PY" "$HERE/claudi_hook.py" --self-test; then
  echo "  ok"
else
  echo "  FAIL"; fail=1
fi

echo "== 3. stdin payload (dry-run, snapshot) =="
TMP_STATE="$(mktemp -t claudi_state.XXXXXX)"
rm -f "$TMP_STATE"   # start from a clean (non-existent) state file
out="$(echo '{"hook_event_name":"PreToolUse","tool_name":"Bash","session_id":"verify"}' \
  | CLAUDI_DRY_RUN=true CLAUDI_STATE_FILE="$TMP_STATE" "$PY" "$HERE/claudi_hook.py")"
rm -f "$TMP_STATE" "$TMP_STATE.tmp" "$TMP_STATE.lock"
echo "  $out"
case "$out" in
  *"state=working"*"running=1"*) echo "  ok" ;;
  *) echo "  FAIL (expected state=working running=1)"; fail=1 ;;
esac

echo
if [ "$fail" -eq 0 ]; then
  echo "ALL CHECKS PASSED"
else
  echo "SOME CHECKS FAILED"
fi
exit "$fail"
