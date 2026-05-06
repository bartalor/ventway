#!/usr/bin/env bash
# Run a GDB Python diagnostic against firmware running in Renode.
#
# Starts Renode with the GDB server on :3333, waits for the port,
# runs the given gdb script, then tears Renode down. The trap
# guarantees cleanup on success, error, or Ctrl-C.
#
# Usage: debug/run_diagnostic.sh <gdb-python-script> <elf>
# Env:   RENODE (path to renode binary), RESC (path to .resc to include)

set -euo pipefail

GDB_SCRIPT="${1:?usage: $0 <gdb-python-script> <elf>}"
ELF="${2:?usage: $0 <gdb-python-script> <elf>}"

RENODE="${RENODE:?RENODE must be set}"
RESC="${RESC:?RESC must be set}"

LOG=$(mktemp)
PIDF=$(mktemp)

cleanup() {
    if [ -s "$PIDF" ]; then
        kill "$(cat "$PIDF")" 2>/dev/null || true
    fi
    rm -f "$LOG" "$PIDF"
}
trap cleanup EXIT INT TERM

LD_LIBRARY_PATH="$HOME/renode_portable:${LD_LIBRARY_PATH:-}" "$RENODE" \
    --disable-xwt --console --hide-log \
    -e "include @${RESC}; machine StartGdbServer 3333" \
    > "$LOG" 2>&1 &
echo $! > "$PIDF"

for _ in $(seq 1 100); do
    if ss -tln 2>/dev/null | grep -q ':3333'; then
        break
    fi
    if ! kill -0 "$(cat "$PIDF")" 2>/dev/null; then
        echo "Renode exited before opening :3333 — log:" >&2
        cat "$LOG" >&2
        exit 1
    fi
    sleep 0.1
done

if ! ss -tln 2>/dev/null | grep -q ':3333'; then
    echo "Timed out waiting for :3333 — log:" >&2
    cat "$LOG" >&2
    exit 1
fi

gdb-multiarch -batch -x "$GDB_SCRIPT" "$ELF"
