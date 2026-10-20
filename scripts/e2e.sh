#!/usr/bin/env bash
# End-to-end check: start the server and market-data listener, run the
# selftest, a latency run and a flood run over TCP, then verify the UDP feed
# arrived with no sequence gaps.
set -euo pipefail
BUILD=${BUILD:-build}
PORT=${PORT:-9300}
MD_PORT=$((PORT + 1))
LOGS=$(mktemp -d)

"$BUILD/lob_server" --port "$PORT" --md "127.0.0.1:$MD_PORT" > "$LOGS/server.log" 2>&1 &
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
echo "e2e passed"
