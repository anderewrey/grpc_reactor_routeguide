#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 anderewrey
#
# Smoke test for the callback example applications: starts route_guide_callback_server, runs
# route_guide_callback_client against it twice, and checks the RouteChat replies of each run.
#
# The client sends four notes, at locations (1,1), (2,2), (3,3) and (1,1) again, and the server
# replies to each with the notes it received earlier at the same location. The first run therefore
# gets 1 reply (the second (1,1) note matches the first) and the second run, against the notes of
# both runs, gets 2 + 1 + 1 + 3 = 7. A server that resent its earlier replies on every read would
# give 16.
#
# The server is killed at the end, so its exit-time sanitizer checks (LeakSanitizer) do not run;
# any report it printed while running fails the test.
#
# Usage: callback_smoke_test.sh <route_guide_callback_server> <route_guide_callback_client>

set -u

server=$1
client=$2
work=$(mktemp -d)
server_pid=

cleanup() {
  if [ -n "$server_pid" ]; then
    kill "$server_pid" 2> /dev/null
    wait "$server_pid" 2> /dev/null
  fi
  rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

fail() {
  echo "FAIL: $*"
  for log in "$work"/*.log; do
    echo "----- $(basename "$log") -----"
    cat "$log"
  done
  exit 1
}

# Sanitizer runtimes print "ERROR: AddressSanitizer", "WARNING: ThreadSanitizer", and so on;
# UndefinedBehaviorSanitizer prints "runtime error:".
check_sanitizers() {
  if grep -qE 'Sanitizer|runtime error:' "$1"; then
    fail "sanitizer report in $(basename "$1")"
  fi
}

"$server" > "$work/server.log" 2>&1 &
server_pid=$!

# Sanitized builds can take a while to start, hence the generous limit (in tenths of a second).
tries=0
until grep -q 'Server listening' "$work/server.log"; do
  kill -0 "$server_pid" 2> /dev/null || fail "server exited before listening"
  tries=$((tries + 1))
  [ "$tries" -le 600 ] || fail "server not listening after 60 s"
  sleep 0.1
done

run_client() {
  run=$1
  expected=$2
  log="$work/client-$run.log"
  "$client" > "$log" 2>&1 || fail "client run $run exited with status $?"
  check_sanitizers "$log"
  grep -q '\[RouteChat\].*post-Await() OK: true' "$log" || fail "client run $run: RouteChat did not succeed"
  replies=$(grep -c '\[RouteChat\].*RESPONSE |' "$log")
  [ "$replies" -eq "$expected" ] || fail "client run $run: $replies RouteChat replies, expected $expected"
  echo "client run $run: $replies RouteChat replies"
}

run_client 1 1
run_client 2 7

kill -0 "$server_pid" 2> /dev/null || fail "server exited during the test"
check_sanitizers "$work/server.log"
echo "PASS"
