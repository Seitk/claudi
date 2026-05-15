#!/usr/bin/env bash
# Push a synthetic workflow into a running claudid for a visual demo.
# Requires the `claudi` binary on PATH (e.g. `cargo run --release -p claudi`).

set -euo pipefail

CLAUDI="${CLAUDI:-./target/release/claudi}"

if [ ! -x "$CLAUDI" ]; then
  echo "claudi binary not found at $CLAUDI — build with: cargo build --release -p claudi" >&2
  exit 1
fi

send () {
  "$CLAUDI" simulate "$@"
}

echo "seeding sessions…"

send --event session_start --session sess-alpha-001 --input ""
send --event session_start --session sess-bravo-002 --input ""
send --event session_start --session sess-charlie-003 --input ""

sleep 0.3
send --event user_prompt_submit --session sess-alpha-001 --input "refactor the protocol crate to support partial RLE updates"
send --event pre_tool_use --session sess-alpha-001 --tool Read  --input "$HOME/Development/claudi/crates/protocol/src/lib.rs"
sleep 0.2
send --event pre_tool_use --session sess-alpha-001 --tool Edit  --input "$HOME/Development/claudi/crates/protocol/src/lib.rs"
sleep 0.2
send --event pre_tool_use --session sess-alpha-001 --tool Bash  --input "cargo test -p claudi-protocol"
sleep 0.2

send --event user_prompt_submit --session sess-bravo-002 --input "ship the visual review report"
send --event pre_tool_use --session sess-bravo-002 --tool Grep  --input "fn report"
sleep 0.2
send --event notification --session sess-bravo-002 --input "Bash tool needs approval"

send --event pre_tool_use --session sess-charlie-003 --tool Agent --input "find all callers of compute_dirty_rects"

echo "done. Look at the simulator window."
