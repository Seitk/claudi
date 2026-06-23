#!/usr/bin/env bash
# Thin wrapper around claudi_hook.py used by .claude/settings.json hooks.
#
# Why a wrapper:
#   * picks a python3 that exists,
#   * always exits 0 so a hook can never break Claude Code,
#   * forwards stdin (the hook payload) and any args through untouched.
#
# It deliberately backgrounds nothing: the Python script is already fast and
# short-timeout, and keeping it synchronous makes logs/ordering predictable.

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY="${CLAUDI_PYTHON:-}"
if [ -z "$PY" ]; then
  if command -v python3 >/dev/null 2>&1; then
    PY="python3"
  elif command -v python >/dev/null 2>&1; then
    PY="python"
  else
    # No python available: silently succeed so the hook is a no-op.
    exit 0
  fi
fi

"$PY" "$HERE/claudi_hook.py" "$@" || true
exit 0
