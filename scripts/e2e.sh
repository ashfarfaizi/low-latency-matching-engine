#!/usr/bin/env bash
# End-to-end check: start the server (journaling every command) and the
# market-data listener, run the selftest, a latency run and a flood run over
# TCP, verify the UDP feed had no sequence gaps, then replay the journal and
# require the replay's event digest to equal the live server's.
set -euo pipefail
BUILD=${BUILD:-build}
PORT=${PORT:-9300}
MD_PORT=$((PORT + 1))
LOGS=$(mktemp -d)

"$BUILD/lob_server" --port "$PORT" --md "127.0.0.1:$MD_PORT" --journal "$LOGS/journal.bin" > "$LOGS/server.log" 2>&1 &
SERVER=$!
"$BUILD/lob_md_listen" 127.0.0.1 "$MD_PORT" 3 > "$LOGS/md.log" 2>&1 &
LISTENER=$!
trap 'kill -INT $SERVER 2>/dev/null || true; kill $LISTENER 2>/dev/null || true' EXIT
sleep 0.5

"$BUILD/lob_client" selftest 0 127.0.0.1 "$PORT"
"$BUILD/lob_client" latency "${LATENCY_N:-50000}" 127.0.0.1 "$PORT"
"$BUILD/lob_client" flood "${FLOOD_N:-500000}" 127.0.0.1 "$PORT"

wait $LISTENER && MD_OK=1 || MD_OK=0
kill -INT $SERVER; wait $SERVER || true
trap - EXIT
cat "$LOGS/server.log"
cat "$LOGS/md.log"
if [ "$MD_OK" != 1 ]; then echo "market data had gaps"; exit 1; fi

"$BUILD/lob_replay" "$LOGS/journal.bin" | tee "$LOGS/replay.log"
LIVE=$(grep -o 'event digest [0-9a-f]*' "$LOGS/server.log" | awk '{print $3}')
REPLAY=$(grep -o 'event digest [0-9a-f]*' "$LOGS/replay.log" | awk '{print $3}')
echo "live digest $LIVE, replay digest $REPLAY"
if [ -z "$LIVE" ] || [ "$LIVE" != "$REPLAY" ]; then echo "replay does not match live run"; exit 1; fi
echo "e2e passed"
