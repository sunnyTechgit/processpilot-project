#!/bin/bash
# End-to-end test: starts the real daemon and exercises start order, crash recovery,
# dependency cascade, failure limit, stop/start, event journal and clean shutdown.
cd "$(dirname "$0")/.."
BIN=./build/processpilot; CTL=./build/ppctl
T=$(mktemp -d); SOCK=$T/s.sock; export PROCESSPILOT_SOCK=$SOCK
pass=0; fail=0
ok()  { echo "  PASS  $1"; pass=$((pass+1)); }
bad() { echo "  FAIL  $1"; fail=$((fail+1)); }
col() { $CTL status 2>/dev/null | awk -v n="$1" -v c="$2" '$1==n{print $c}'; }
wait_for() { # desc, name, col, expected
  for _ in $(seq 1 60); do [ "$(col "$2" "$3")" = "$4" ] && { ok "$1"; return 0; }; sleep 0.1; done
  bad "$1 (got '$(col "$2" "$3")', wanted '$4')"; return 1; }

cat > $T/t.conf <<CONF
[db]
command=sleep 300
[web]
command=sleep 301
depends=db
restart=always
backoff_ms=100
[job]
command=sh -c "exit 1"
restart=on-failure
max_restarts=2
backoff_ms=50
CONF
$BIN --check -c $T/t.conf | grep -q "db web job" && ok "--check prints start order" || bad "--check"
$BIN -c $T/t.conf -s $SOCK -l $T/logs 2>$T/daemon.err & DPID=$!
for _ in $(seq 1 50); do [ -S $SOCK ] && break; sleep 0.1; done

wait_for "db Running" db 2 Running
wait_for "web Running (after db)" web 2 Running
wait_for "job reaches Failed after max_restarts" job 2 Failed
[ "$(col job 4)" = "3" ] && ok "job restart counter = 3 (limit 2 exceeded)" || bad "job restarts '$(col job 4)'"

DB1=$(col db 3); WEB1=$(col web 3)
kill -9 "$DB1"
wait_for "db restarted after SIGKILL" db 2 Running
[ "$(col db 3)" != "$DB1" ] && ok "db has new PID" || bad "db PID unchanged"
wait_for "web Running again after cascade" web 2 Running
[ "$(col web 3)" != "$WEB1" ] && ok "web was bounced with its dependency" || bad "web PID unchanged"

$CTL stop db >/dev/null
wait_for "stop db also stops dependent web" web 2 Stopped
wait_for "db Stopped" db 2 Stopped
$CTL start web >/dev/null
wait_for "start web pulls in db" db 2 Running
wait_for "web Running after start" web 2 Running

$CTL restart db >/dev/null
sleep 1
wait_for "restart db: db back" db 2 Running
wait_for "restart db: web back" web 2 Running

$CTL bogus | grep -q commands && ok "unknown command prints help" || bad "help"
$CTL stop nope >/dev/null 2>&1 && bad "unknown service should fail" || ok "unknown service rejected"
$CTL logs 200 | grep -q "started .db." && ok "logs command shows events" || bad "logs command"
[ -d $T/logs ] && ok "per-service log dir created" || bad "log dir"

$CTL shutdown >/dev/null
for _ in $(seq 1 60); do kill -0 $DPID 2>/dev/null || break; sleep 0.1; done
kill -0 $DPID 2>/dev/null && { bad "daemon exited on shutdown"; kill -9 $DPID; } || ok "daemon exited on shutdown"
[ -e $SOCK ] && bad "socket removed" || ok "socket removed"
pgrep -x sleep -a | grep -qE "sleep 30[01]$" && bad "no orphan children" || ok "no orphan children"

# SIGTERM path
$BIN -c $T/t.conf -s $SOCK 2>/dev/null & D2=$!
for _ in $(seq 1 50); do [ -S $SOCK ] && break; sleep 0.1; done
sleep 0.5; kill -TERM $D2
for _ in $(seq 1 60); do kill -0 $D2 2>/dev/null || break; sleep 0.1; done
kill -0 $D2 2>/dev/null && { bad "SIGTERM graceful stop"; kill -9 $D2; } || ok "SIGTERM graceful stop"
pgrep -x sleep -a | grep -qE "sleep 30[01]$" && bad "no orphans after SIGTERM" || ok "no orphans after SIGTERM"

echo "integration: $pass passed, $fail failed"
rm -rf $T
[ $fail -eq 0 ]
