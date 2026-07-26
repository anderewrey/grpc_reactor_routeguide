#!/bin/sh
###
### SPDX-License-Identifier: Apache-2.0
### Copyright 2026 anderewrey
###
### Reproduction for the suspected use-after-free in route_guide_callback_server.cpp: several
### reactor subclasses (Lister, Chatter) hold a logger as a *member reference* and delete `this`
### from OnDone(), but their normal completion paths log through that member *after* calling
### Finish()/StartWriteAndFinish() - a method gRPC's callback API may respond to by invoking
### OnDone() (and thus `delete this`) before or concurrently with that call returning.
###
### route_guide_callback_client's main() already drives a ListFeatures and a RouteChat call to
### completion, which is exactly the sequence that exercises both suspect code paths server-side.
### Run under the project's clang-asan-ubsan CI variant, both binaries are already sanitizer-
### instrumented, so this needs no new C++ code - just wiring a real client against a real server
### and checking the server's own output for a sanitizer report.
set -u

server_exe="$1"
client_exe="$2"
log_file="$(mktemp)"

"$server_exe" >"$log_file" 2>&1 &
server_pid=$!

# Give the server a moment to bind and start listening before the client connects.
sleep 2

"$client_exe"
client_status=$?

kill "$server_pid" 2>/dev/null
wait "$server_pid" 2>/dev/null
server_status=$?

echo "----- server output -----"
cat "$log_file"
echo "--------------------------"
echo "client exit: $client_status, server exit: $server_status"

if grep -qE "AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:" "$log_file"; then
  echo "Sanitizer reported an error in the server process - see server output above."
  rm -f "$log_file"
  exit 1
fi

rm -f "$log_file"
exit 0
