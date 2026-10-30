#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 anderewrey
#
# Compares the digests the smoke matrix cells wrote, see smoke_matrix_cell.sh:
# - each client's digest must be the same against every server;
# - each logger of a coroutine twin application, named after its callback parent's logger with a .coro suffix,
#   must count what the parent's logger counts, against the same server. Every .coro logger must find its parent,
#   and every twin application must have one, so a renamed logger fails rather than goes unchecked.
#
# Usage: smoke_matrix_compare.sh <digest dir> "<servers>" "<clients>" "<twin>=<parent> ..."

set -u

dir=$1
servers=$2
clients=$3
twins=$4
failures=0

fail() {
  echo "FAIL: $*"
  failures=$((failures + 1))
}

# Checks that the digest of client $1 against server $2 exists, and names it in $file.
digest_of() {
  file="$dir/$1@$2.digest"
  [ -s "$file" ] || { fail "missing digest $1@$2.digest"; return 1; }
}

for client in $clients; do
  first=
  for server in $servers; do
    digest_of "$client" "$server" || continue
    if [ -z "$first" ]; then
      first=$file
    elif ! cmp -s "$first" "$file"; then
      fail "$client: digest against $server differs from $(basename "$first")"
      diff "$first" "$file"
    fi
  done
done

for pair in $twins; do
  twin=${pair%%=*}
  parent=${pair#*=}
  for server in $servers; do
    digest_of "$twin" "$server" || continue
    twin_file=$file
    digest_of "$parent" "$server" || continue
    parent_file=$file
    compared=0
    while read -r logger count; do
      case $logger in
        *.coro) ;;
        *) continue ;;
      esac
      expected=$(awk -v l="${logger%.coro}" '$1 == l { print $2 }' "$parent_file")
      if [ -z "$expected" ]; then
        fail "$twin against $server: $logger has no ${logger%.coro} in $parent"
      elif [ "$count" != "$expected" ]; then
        fail "$twin against $server: $logger received $count, ${logger%.coro} received $expected"
      else
        echo "$twin against $server: $logger = ${logger%.coro} = $count"
      fi
      compared=$((compared + 1))
    done < "$twin_file"
    [ "$compared" -gt 0 ] || fail "$twin against $server: no .coro logger to compare with $parent"
  done
done

[ "$failures" -eq 0 ] || { echo "$failures failure(s)"; exit 1; }
echo "PASS"
