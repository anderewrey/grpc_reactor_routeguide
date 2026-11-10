#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 anderewrey
#
# One cell of the smoke matrix: starts a fresh server, runs one client application against it to its end, and
# writes the client's digest, which smoke_matrix_compare.sh compares across servers and between twins.
#
# The client must exit with status 0, log no failed RPC status and no sanitizer report. The server must still
# run at the end, and log no sanitizer report; it is then killed, so its exit-time checks (LeakSanitizer) do not
# run.
#
# The digest has one line per logger, "<logger> <messages>", where messages counts the messages the scenario
# received: its read_ok and skipped lines when it logs any, which the callback reactors log from the gRPC thread
# for every message, else its RESPONSE lines. The RouteChat loggers make one "chat <n>" line instead, the replies
# of all of them together. Both servers answer each note with every note already left at its location, and store
# the new one, so each pair of notes at one location makes exactly one reply. What one chat receives depends on
# how the chats interleave; their total does not: it must equal the replies the server owes for the notes its log
# shows it read, the sum of n(n-1)/2 over the locations, which proves every reply was received. It assumes every
# chat reads to the end of its stream, and sends no note with an empty message, which the callback server answers
# without logging or storing it.
#
# A chat whose logger logs a line per note it sends, "typed    |" or "sent     |", also makes a
# "notes:<logger> <n>" line, which smoke_matrix_compare.sh compares between a twin and its parent: a chat's replies
# depend on the other chats of its application, but the notes it sends do not.
#
# Usage: smoke_matrix_cell.sh <server> <client> <digest>

set -u

server=$1
client=$2
digest=$3
work=$(mktemp -d)
server_pid=

# The loggers of the scenarios that share the server's RouteChat note history.
chat_loggers='^(RouteChat.*|ConvoyChat([.]coro)?|GuidedTourBriefing)$'

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
  echo "FAIL: $(basename "$client") against $(basename "$server"): $*"
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

# A stale digest must not survive a failed run.
rm -f "$digest"
mkdir -p "$(dirname "$digest")"

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

log="$work/client.log"
# RecordRoute waits only a few milliseconds between points, rather than gRPC's example's 0.5 to 1.5 s.
"$client" --point_delay_ms=5 > "$log" 2>&1 || fail "client exited with status $?"
check_sanitizers "$log"

# An RPC that fails the same way on both servers would leave the digests equal. A false OnReadDone() or
# OnWriteDone() is the normal end of a stream, not a failure.
failed=$(grep 'OK: false' "$log" | grep -vE 'On(Read|Write)Done\(\) OK: false')
[ -z "$failed" ] || fail "failed RPC status: $failed"

kill -0 "$server_pid" 2> /dev/null || fail "server exited during the test"
check_sanitizers "$work/server.log"

# The second field of the log pattern, [time][logger][thread]..., is the logger.
awk -F '\\]\\[' -v chat="$chat_loggers" '
  NF > 2 {
    logger = $2
    if (logger ~ chat) {
      chats[logger] = 1
      if ($0 ~ /(typed    |sent     )\|/) notes[logger]++
    }
    if ($0 ~ /(read_ok|skipped)  \|/) received[logger]++
    else if ($0 ~ /RESPONSE \|/) responses[logger]++
  }
  END {
    n = 0
    replies = 0
    for (l in chats) {
      n++
      replies += (l in received) ? received[l] : responses[l]
    }
    for (l in received) if (!(l in chats)) print l, received[l]
    for (l in responses) if (!(l in received) && !(l in chats)) print l, responses[l]
    if (n > 0) print "chat", replies
    for (l in notes) print "notes:" l, notes[l]
  }' "$log" | LC_ALL=C sort > "$work/digest"
[ -s "$work/digest" ] || fail "no scenario received anything"

# The replies the server owes: for the notes it read, keyed by their location, n(n-1)/2 per location.
owed=$(awk '
  /\[RouteChat\]/ && /REQUEST  \| RouteNote:/ {
    key = match($0, /location [{][^}]*[}]/) ? substr($0, RSTART, RLENGTH) : "none"
    notes[key]++
  }
  END {
    owed = 0
    for (k in notes) owed += notes[k] * (notes[k] - 1) / 2
    print owed
  }' "$work/server.log")
chat=$(awk '$1 == "chat" { print $2 }' "$work/digest")
if [ -n "$chat" ] && [ "$chat" != "$owed" ]; then
  fail "the chats received $chat replies, the server owed $owed"
fi
mv "$work/digest" "$digest"
cat "$digest"
echo "PASS"
