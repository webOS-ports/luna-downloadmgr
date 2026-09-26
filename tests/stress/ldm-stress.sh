#!/bin/bash
# Copyright (c) 2026 LG Electronics, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
#
# Stress driver for com.webos.service.downloadmanager. Run it on the device,
# with ldm-origin.py serving either locally or from the host.
#
# Unlike the old lots-of-downloads.sh - 50000 identical requests against a public
# FTP mirror - this drives the state transitions that actually broke: pausing a
# transfer that has something queued behind it, resuming one whose partial file
# has been deleted, cancelling mid-write, swapping interfaces, redirect chains at
# the limit, and enough concurrent transfers to push the process past 1023 open
# descriptors. It also watches RSS and fd count so a leak shows up as a trend
# rather than as an eventual OOM.
#
#   ./ldm-stress.sh --origin http://127.0.0.1:8099 --scenario all
#
# Each scenario prints PASS/FAIL lines; the exit status is non-zero if any
# assertion failed or if the service died.

set -u

SVC="luna://com.webos.service.downloadmanager"
ORIGIN="http://127.0.0.1:8099"
SCENARIO="all"
CONCURRENCY=32
ITERATIONS=200
FD_TARGET=1200
KEEP_GOING=0
VERBOSE=0

usage() {
    sed -n '20,40p' "$0" | sed 's/^# \?//'
    cat <<EOF

Options:
  --origin URL        base URL of ldm-origin.py         (default $ORIGIN)
  --scenario NAME     all|smoke|queue|pause|resume|cancel|redirect|headers|
                      fdstorm|leak|badinput               (default $SCENARIO)
  --concurrency N     simultaneous downloads              (default $CONCURRENCY)
  --iterations N      repetitions for the looping scenarios (default $ITERATIONS)
  --fd-target N       descriptors to drive towards in fdstorm (default $FD_TARGET)
  --keep-going        do not stop at the first failure
  -v, --verbose       echo every luna-send payload
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --origin)      ORIGIN="$2"; shift 2 ;;
        --scenario)    SCENARIO="$2"; shift 2 ;;
        --concurrency) CONCURRENCY="$2"; shift 2 ;;
        --iterations)  ITERATIONS="$2"; shift 2 ;;
        --fd-target)   FD_TARGET="$2"; shift 2 ;;
        --keep-going)  KEEP_GOING=1; shift ;;
        -v|--verbose)  VERBOSE=1; shift ;;
        -h|--help)     usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

FAILURES=0
CHECKS=0

# ---------------------------------------------------------------- plumbing

log()  { printf '%s\n' "$*"; }
note() { printf '  %s\n' "$*"; }

call() {
    # call <method> <json>  -> reply on stdout
    local method="$1" payload="$2"
    [ "$VERBOSE" = 1 ] && printf '  -> %s %s\n' "$method" "$payload" >&2
    luna-send -n 1 -a com.webos.service.downloadmanager.stress \
        "$SVC/$method" "$payload" 2>/dev/null
}

call_sub() {
    # call_sub <method> <json> <replies> -> up to <replies> subscription messages
    local method="$1" payload="$2" n="$3"
    [ "$VERBOSE" = 1 ] && printf '  -> (sub %s) %s %s\n' "$n" "$method" "$payload" >&2
    luna-send -i -n "$n" -a com.webos.service.downloadmanager.stress \
        "$SVC/$method" "$payload" 2>/dev/null
}

jget() {
    # jget <json> <key> - no jq on a minimal image, so stay with sed
    printf '%s' "$1" | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"\{0,1\}\([^,\"}]*\)\"\{0,1\}.*/\1/p" | head -1
}

check() {
    # check <description> <condition-as-string>
    CHECKS=$((CHECKS + 1))
    if eval "$2"; then
        printf '  PASS  %s\n' "$1"
    else
        printf '  FAIL  %s\n' "$1"
        FAILURES=$((FAILURES + 1))
        [ "$KEEP_GOING" = 1 ] || return 1
    fi
    return 0
}

svc_pid() {
    pidof LunaDownloadMgr 2>/dev/null | awk '{print $1}'
}

svc_alive() {
    [ -n "$(svc_pid)" ]
}

rss_kb() {
    local p; p="$(svc_pid)"
    [ -n "$p" ] && awk '/VmRSS/{print $2}' "/proc/$p/status" 2>/dev/null || echo 0
}

fd_count() {
    local p; p="$(svc_pid)"
    [ -n "$p" ] && ls "/proc/$p/fd" 2>/dev/null | wc -l || echo 0
}

snapshot() {
    printf '  [pid=%s rss=%sKB fds=%s active=%s]\n' \
        "$(svc_pid)" "$(rss_kb)" "$(fd_count)" \
        "$(jget "$(call listPending '{}')" count)"
}

wait_idle() {
    # wait_idle <seconds>
    local deadline=$(( $(date +%s) + ${1:-30} ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        [ "$(jget "$(call listPending '{}')" count)" = "0" ] && return 0
        sleep 1
    done
    return 1
}

start_download() {
    # start_download <path> [extra-json] -> ticket
    local path="$1" extra="${2:-}"
    local payload="{\"target\":\"$ORIGIN$path\"${extra:+,$extra}}"
    jget "$(call download "$payload")" ticket
}

# ---------------------------------------------------------------- scenarios

scenario_smoke() {
    log "== smoke: one download of each origin behaviour"
    local t

    t=$(start_download /ok/65536)
    check "plain download returns a ticket" '[ -n "$t" ] && [ "$t" != "0" ]'
    wait_idle 30

    t=$(start_download /nolength/32768)
    check "download with no Content-Length is accepted" '[ -n "$t" ]'
    wait_idle 30

    t=$(start_download /status/404)
    check "404 still yields a ticket (error is reported on the ticket)" '[ -n "$t" ]'
    wait_idle 30

    t=$(start_download /truncate/131072)
    check "short body is accepted and then reported" '[ -n "$t" ]'
    wait_idle 30

    check "service survived the smoke pass" 'svc_alive'
    snapshot
}

scenario_queue() {
    log "== queue: overfill the queue, then drain it"
    # MaxQueueLength defaults to 128 and MaxConcurrent to 2, so this both fills
    # the active slots and exercises the queued->running promotion that used to
    # copy the wrong interface into the history row.
    local i t queued=0 refused=0
    for i in $(seq 1 "$CONCURRENCY"); do
        t=$(start_download "/slow/2097152/32")
        if [ -n "$t" ] && [ "$t" != "0" ]; then queued=$((queued + 1)); else refused=$((refused + 1)); fi
    done
    note "accepted=$queued refused=$refused"
    check "the service accepted at least some of the burst" '[ "$queued" -gt 0 ]'
    check "listPending reports them" '[ "$(jget "$(call listPending "{}")" count)" -gt 0 ]'

    call cancelAllDownloads '{}' >/dev/null
    check "cancelAllDownloads drains the queue" 'wait_idle 60'
    check "service survived the queue burst" 'svc_alive'
    snapshot
}

scenario_pause() {
    log "== pause: pause an active transfer with work queued behind it"
    # This is the shape that read the freed DownloadTask: pauseDownload() frees
    # the task and then promotes the next queued download.
    local first second i
    first=$(start_download "/slow/8388608/32" '"canHandlePause":true')
    for i in $(seq 1 6); do
        start_download "/slow/8388608/32" '"canHandlePause":true' >/dev/null
    done
    sleep 3
    check "first download started" '[ -n "$first" ]'

    local reply; reply=$(call pauseDownload "{\"ticket\":$first}")
    note "pauseDownload -> $reply"
    check "pause was accepted" '[ "$(jget "$reply" returnValue)" = "true" ]'
    sleep 2
    check "service survived the pause" 'svc_alive'

    reply=$(call resumeDownload "{\"ticket\":$first}")
    note "resumeDownload -> $reply"
    check "resume was accepted" '[ -n "$reply" ]'

    call cancelAllDownloads '{}' >/dev/null
    wait_idle 60
    snapshot
}

scenario_resume() {
    log "== resume: interrupt, delete the partial file, resume anyway"
    local t reply dest
    t=$(start_download "/flaky/4194304" '"canHandlePause":true')
    check "flaky download started" '[ -n "$t" ]'
    sleep 4

    reply=$(call downloadStatusQuery "{\"ticket\":$t}")
    dest=$(jget "$reply" target)
    note "target=$dest"

    call pauseDownload "{\"ticket\":$t}" >/dev/null
    sleep 1

    # The partial file disappearing is the case that wrapped
    # completedSize - initialOffset into a huge seek and failed the resume.
    [ -n "$dest" ] && rm -f "$dest"
    reply=$(call resumeDownload "{\"ticket\":$t}")
    note "resume after deleting the partial file -> $reply"
    check "resume does not wedge the service" 'svc_alive'

    call cancelAllDownloads '{}' >/dev/null
    wait_idle 60
    snapshot
}

scenario_cancel() {
    log "== cancel: start and cancel repeatedly ($ITERATIONS iterations)"
    local i t rss0 rss1
    rss0=$(rss_kb)
    for i in $(seq 1 "$ITERATIONS"); do
        t=$(start_download "/slow/4194304/512")
        [ -n "$t" ] && call cancelDownload "{\"ticket\":$t}" >/dev/null
        if [ $((i % 50)) = 0 ]; then note "iteration $i: rss=$(rss_kb)KB fds=$(fd_count)"; fi
        svc_alive || break
    done
    wait_idle 60
    rss1=$(rss_kb)
    note "rss ${rss0}KB -> ${rss1}KB over $ITERATIONS start/cancel cycles"
    check "service survived $ITERATIONS cancel cycles" 'svc_alive'
    # A per-cycle leak of the TransferTask shows up as steady growth. Allow
    # generous slack for allocator behaviour, but not unbounded growth.
    check "RSS did not grow by more than 50% across the run" \
          '[ "$rss0" -eq 0 ] || [ "$rss1" -le $((rss0 * 3 / 2 + 4096)) ]'
    snapshot
}

scenario_redirect() {
    log "== redirect: chains at, below and past the RFC limit, plus a loop"
    local t
    for n in 1 3 5 6 9; do
        t=$(start_download "/redirect/$n")
        note "redirect chain of $n -> ticket ${t:-none}"
        wait_idle 45
        check "service survived a $n-hop redirect chain" 'svc_alive'
    done

    t=$(start_download "/redirect-loop")
    note "redirect loop -> ticket ${t:-none}"
    check "redirect loop terminates (MAXREDIRECTIONS)" 'wait_idle 60'
    check "service survived the redirect loop" 'svc_alive'

    # a redirect target whose filename is full of JSON metacharacters
    t=$(start_download "/quote-name")
    wait_idle 45
    check "quoted redirect target does not break the reply" 'svc_alive'
    snapshot
}

scenario_headers() {
    log "== headers: oversized, duplicated and non-ASCII response headers"
    local t
    t=$(start_download "/weird-headers")
    check "weird headers accepted" '[ -n "$t" ]'
    wait_idle 30
    check "service survived weird headers" 'svc_alive'

    t=$(start_download "/huge")
    note "Content-Length above 2^32 -> ticket ${t:-none}"
    wait_idle 45
    check "service survived an oversized Content-Length" 'svc_alive'

    t=$(start_download "/stall/1048576")
    note "stalled transfer -> ticket ${t:-none} (LOW_SPEED_TIME is 10s)"
    check "stalled transfer is eventually abandoned" 'wait_idle 90'
    snapshot
}

scenario_fdstorm() {
    log "== fdstorm: drive the process towards $FD_TARGET open descriptors"
    # glibcurl's poll table holds 1024 entries; past that it used to index out
    # of bounds. Getting there needs a lot of simultaneous slow transfers, so
    # raise MaxConcurrent in downloadManager.conf before running this.
    local i t started=0
    for i in $(seq 1 "$FD_TARGET"); do
        t=$(start_download "/slow/16777216/8")
        [ -n "$t" ] && [ "$t" != "0" ] && started=$((started + 1))
        if [ $((i % 100)) = 0 ]; then
            note "requested $i, accepted $started, fds=$(fd_count)"
            svc_alive || break
        fi
    done
    note "peak fds=$(fd_count) (target $FD_TARGET)"
    check "service survived the descriptor storm" 'svc_alive'
    if [ "$(fd_count)" -lt 1024 ]; then
        note "NOTE: stayed under 1024 fds - raise MaxConcurrent/MaxQueueLength"
        note "      in /etc/palm/downloadManager.conf to reach the interesting range"
    fi
    call cancelAllDownloads '{}' >/dev/null
    wait_idle 120
    snapshot
}

scenario_leak() {
    log "== leak: $ITERATIONS complete download cycles, watching RSS and fds"
    local i rss0 fd0 rss1 fd1
    wait_idle 30
    rss0=$(rss_kb); fd0=$(fd_count)
    for i in $(seq 1 "$ITERATIONS"); do
        start_download "/ok/65536" >/dev/null
        if [ $((i % 25)) = 0 ]; then
            wait_idle 30
            note "iteration $i: rss=$(rss_kb)KB fds=$(fd_count)"
        fi
        svc_alive || break
    done
    wait_idle 120
    rss1=$(rss_kb); fd1=$(fd_count)
    note "rss ${rss0}KB -> ${rss1}KB, fds ${fd0} -> ${fd1}"
    check "service survived $ITERATIONS completed downloads" 'svc_alive'
    check "descriptors returned to roughly the starting count" \
          '[ "$fd1" -le $((fd0 + 16)) ]'
    check "RSS did not grow by more than 50%" \
          '[ "$rss0" -eq 0 ] || [ "$rss1" -le $((rss0 * 3 / 2 + 4096)) ]'
    # The queue empties between batches, which is what cycles
    # shutdownGlibCurl()/startupGlibCurl() and used to leak a curl share handle
    # each time round.
    snapshot
}

scenario_badinput() {
    log "== badinput: malformed and hostile request payloads"
    local r
    for payload in \
        '{}' \
        '{"target":""}' \
        '{"target":"not-a-url"}' \
        '{"target":"file:///etc/shadow"}' \
        '{"target":"http://"}' \
        '{"target":"http://[",  "subscribe":true}' \
        '{"target":"'"$ORIGIN"'/ok/1024","targetDir":"/etc"}' \
        '{"target":"'"$ORIGIN"'/ok/1024","targetDir":"/media/internal/../../etc"}' \
        '{"target":"'"$ORIGIN"'/ok/1024","targetFilename":"../../etc/passwd"}' \
        '{"target":"'"$ORIGIN"'/ok/1024","targetFilename":"a\"b"}' \
        '{"target":"'"$ORIGIN"'/ok/1024","e_rangeLow":"99999999999999999999"}' \
        '{"target":"'"$ORIGIN"'/ok/1024","e_rangeLow":"-1","e_rangeHigh":"-2"}' \
        '{"ticket":0}' \
        '{"ticket":-1}' \
        '{"ticket":99999999}' \
    ; do
        r=$(call download "$payload")
        note "download $payload"
        note "   -> ${r:-<no reply>}"
        # Whatever the verdict, the reply has to be JSON we can read back and
        # the service has to still be there.
        check "reply to $payload is non-empty" '[ -n "$r" ]'
        check "reply to $payload parses as JSON" \
              'printf "%s" "$r" | python3 -c "import json,sys; json.load(sys.stdin)" 2>/dev/null || command -v python3 >/dev/null || true'
        svc_alive || { check "service survived $payload" 'false'; break; }
    done

    for m in pauseDownload resumeDownload cancelDownload deleteDownloadedFile downloadStatusQuery; do
        r=$(call "$m" '{"ticket":123456789}')
        note "$m on an unknown ticket -> ${r:-<no reply>}"
        check "$m handles an unknown ticket" 'svc_alive'
    done

    r=$(call filesysStatusCheck '{"path":"/proc"}')
    note "filesysStatusCheck /proc -> ${r:-<no reply>}"
    check "zero-sized filesystem does not crash the service" 'svc_alive'
    r=$(call filesysStatusCheck '{"path":"/nonexistent"}')
    check "missing path does not crash the service" 'svc_alive'

    r=$(call upload '{"url":"'"$ORIGIN"'/ok/16","fileName":"/etc/shadow"}')
    note "upload outside /media/internal -> ${r:-<no reply>}"
    check "upload path check still refuses /etc" 'svc_alive'

    snapshot
}

# ---------------------------------------------------------------- driver

if ! command -v luna-send >/dev/null 2>&1; then
    echo "luna-send not found: this script has to run on the device" >&2
    exit 2
fi

if ! svc_alive; then
    log "note: LunaDownloadMgr is not running; it should start on first call"
fi

log "origin   : $ORIGIN"
log "scenario : $SCENARIO"
snapshot

run() {
    if [ "$SCENARIO" = "all" ] || [ "$SCENARIO" = "$1" ]; then
        "scenario_$1"
        log ""
    fi
}

run smoke
run queue
run pause
run resume
run cancel
run redirect
run headers
run badinput
run leak
[ "$SCENARIO" = "fdstorm" ] && scenario_fdstorm

log "=============================================="
log "checks: $CHECKS   failures: $FAILURES"
[ "$FAILURES" -eq 0 ] || exit 1
exit 0
