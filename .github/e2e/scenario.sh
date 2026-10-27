#!/usr/bin/env bash
set -uo pipefail

MODE=$1
BIN=${2:-}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d /tmp/crossfade.XXXXXX)
chmod 755 "$WORK"
LOG=$WORK/relay.log
PASS=$(od -An -tx1 -N16 /dev/urandom | tr -d ' \n')
FAILED=0
SUMMARY=""

note() { SUMMARY+="$1"$'\n'; echo "$1"; }
ok() { note "ok   $1"; }
bad() { note "FAIL $1"; FAILED=$((FAILED + 1)); }
expect() { if eval "$2"; then ok "$1"; else bad "$1 ($3)"; fi; }
escape() { local s; s=$(sed 's/\x1b\[[0-9;]*m//g' <<<"$1"); s=${s//'%'/'%25'}; s=${s//$'\r'/}; s=${s//$'\n'/'%0A'}; printf '%s' "$s"; }

relay_start() {
	if [ "$MODE" = docker ]; then
		docker run -d --name cf --network host --read-only --memory 1g --cap-drop ALL --security-opt no-new-privileges=true -v "$WORK:$WORK:ro" crossfade:test "$WORK/crossfade.conf" >/dev/null
	else
		"$BIN" "$WORK/crossfade.conf" >"$LOG" 2>&1 &
		RELAY=$!
	fi
}
relay_signal() { if [ "$MODE" = docker ]; then docker kill --signal="$1" cf >/dev/null; else kill -"$1" "$RELAY"; fi; }
relay_log() { if [ "$MODE" = docker ]; then docker logs cf 2>&1; else cat "$LOG"; fi; }
relay_health() { if [ "$MODE" = docker ]; then docker exec cf /crossfade --health "$1"; else "$BIN" --health "$1"; fi; }
relay_stop() {
	relay_signal TERM
	if [ "$MODE" = docker ]; then
		STATUS=$(timeout 15 docker wait cf || echo timeout)
	else
		timeout 15 tail --pid="$RELAY" -f /dev/null && wait "$RELAY"
		STATUS=$?
	fi
}
wait_log() {
	local i
	for i in $(seq 1 $(($2 * 10))); do
		relay_log | grep -q -- "$1" && return 0
		sleep 0.1
	done
	return 1
}
lines() { relay_log | grep -c -- "$1"; }

publish() {
	ffmpeg -v error -nostdin -re -f lavfi -i "testsrc2=size=1280x720:rate=30" -f lavfi -i "sine=frequency=440:sample_rate=48000" -t "$1" \
		-c:v libx264 -preset veryfast -g 30 -bf 0 -pix_fmt yuv420p -b:v 3M -c:a aac -b:a 128k -f mpegts \
		"srt://127.0.0.1:8890?streamid=${2:-vrc}&passphrase=${3:-$PASS}&pbkeylen=32&latency=200000" >>"$WORK/publisher.log" 2>&1
}
config() {
	cat >"$WORK/crossfade.conf" <<EOF
[general]
workers 2
buffer 16
lag 1000
clients 8
[srt]
listen [::]:8890
latency 200
[ts]
listen 127.0.0.1:8080
[rtsp]
listen [::]:8554
[offline]
* 60 $1
[streams]
$2 $PASS
EOF
}
streams() { for i in $(seq "$2"); do echo "s$i $PASS"; done >>"$1"; }
huge() { sed 's/^listen \[::\]:8890$/listen 127.0.0.1:8894/; s/^listen 127.0.0.1:8080$/listen 127.0.0.1:8084/; s/^listen \[::\]:8554$/listen 127.0.0.1:8558/; s/^buffer 16$/buffer '"$2"'/' "$WORK/crossfade.conf" >"$WORK/huge.conf"; streams "$WORK/huge.conf" "$1"; }
tscheck() { python3 "$HERE/tscheck.py" "$1"; }
keyframe() { ffprobe -v error -select_streams v -skip_frame nokey -show_entries frame=pts_time -of csv=p=0 -read_intervals %+5 "$1" 2>/dev/null | head -1; }
decode() { local start; start=$(keyframe "$1"); ffmpeg -v error -nostdin -ss "${start:-0}" -i "$1" -f null - 2>&1 | grep -v -e "non monotonically increasing dts" -e "Last message repeated" | head -5; }
steps() { ffmpeg -v error -nostdin -i "$1" -f null - 2>&1 | grep -c "non monotonically increasing dts"; }
duration() { ffprobe -v error -show_entries format=duration -of csv=p=0 "$1" 2>/dev/null | cut -d. -f1; }

ffmpeg -v error -nostdin -f lavfi -i "testsrc2=size=640x360:rate=30" -f lavfi -i "sine=frequency=880:sample_rate=48000" -t 6 \
	-c:v libx264 -preset veryfast -g 30 -bf 0 -pix_fmt yuv420p -c:a aac -b:a 96k -f mpegts "$WORK/offline.ts"
truncate -s 70M "$WORK/big.ts"
config "$WORK/offline.ts" vrc
relay_start
expect "relay starts" 'wait_log " ready$" 10' "no startup line"
if [ "$MODE" = docker ]; then
	expect "the container's memory sets the room for streams" 'wait_log "Streams.* (up to 14)$" 1' "$(relay_log | grep Streams)"
	huge 3 64
	timeout 30 docker run --rm --network host --read-only --memory 256m -v "$WORK:$WORK:ro" crossfade:test "$WORK/huge.conf" >"$WORK/huge.log" 2>&1
else
	huge 15 1024
	timeout 10 "$BIN" "$WORK/huge.conf" >"$WORK/huge.log" 2>&1
fi
refused=$?
expect "streams that do not fit in memory are refused at startup" '[ "$refused" = 1 ] && grep -q "not fit in memory" "$WORK/huge.log"' "exit $refused: $(tail -2 "$WORK/huge.log")"

sleep 1
code=$(curl -s --max-time 10 -o /dev/null -w '%{http_code}' http://127.0.0.1:8080/health)
expect "health is 200 with OBS offline" '[ "$code" = 200 ]' "got $code"
health=$(relay_health "$WORK/crossfade.conf" 2>&1)
probed=$?
expect "--health over MPEG-TS exits 0" '[ "$probed" = 0 ]' "exit $probed: $health"
sed 's/^\[ts\]$/[ts]\nenabled no/' "$WORK/crossfade.conf" >"$WORK/rtsp.conf"
health=$(relay_health "$WORK/rtsp.conf" 2>&1)
probed=$?
expect "--health over RTSP exits 0" '[ "$probed" = 0 ]' "exit $probed: $health"
{ printf '# %0600d\n' 0; sed 's/^listen 127.0.0.1:8080$/listen 127.0.0.1:8099/' "$WORK/crossfade.conf"; } >"$WORK/long.conf"
health=$(relay_health "$WORK/long.conf" 2>&1)
probed=$?
expect "--health reads past a line too long" '[ "$probed" = 1 ] && grep -q "8099" <<<"$health"' "exit $probed: $health"
expect "the probe is no viewer" '[ "$(lines "without X-Forwarded-For")" = 0 ]' "noted as a .ts viewer"
sed 's/^\[rtsp\]$/[rstp]/' "$WORK/crossfade.conf" >"$WORK/typo.conf"
health=$(relay_health "$WORK/typo.conf" 2>&1)
probed=$?
expect "--health skips a misspelled section" '[ "$probed" = 0 ]' "exit $probed: $health"
if [ "$MODE" = native ]; then
	sed 's/^listen \[::\]:8890$/listen 127.0.0.1:8891/; s/^listen 127.0.0.1:8080$/listen 127.0.0.1:8081/; s/^listen \[::\]:8554$/listen 127.0.0.1:8555/' "$WORK/crossfade.conf" >"$WORK/full.conf"
	(ulimit -n 96 && exec "$BIN" "$WORK/full.conf") >"$WORK/full.log" 2>&1 &
	FULL=$!
	for i in $(seq 50); do grep -q " viewers)" "$WORK/full.log" && break; sleep 0.1; done
	seats=$(grep -o 'up to [0-9]* viewers' "$WORK/full.log" | cut -d' ' -f3)
	held=()
	for i in $(seq "${seats:-0}"); do exec {fd}<>/dev/tcp/127.0.0.1/8555 && held+=("$fd"); done
	sleep 0.5
	health=$("$BIN" --health "$WORK/full.conf" 2>&1)
	probed=$?
	code=$(curl -s --max-time 2 -o /dev/null -w '%{http_code}' http://127.0.0.1:8081/nope.ts)
	described=""
	exec {rtsp}<>/dev/tcp/127.0.0.1/8555
	printf 'DESCRIBE rtsp://127.0.0.1:8555/nope RTSP/1.0\r\nCSeq: 1\r\n\r\n' >&"$rtsp"
	read -r -t 3 described <&"$rtsp"
	exec {rtsp}>&-
	for fd in "${held[@]}"; do exec {fd}>&-; done
	expect "a full relay still passes --health" '[ -n "$seats" ] && [ "$probed" = 0 ]' "${seats:-no} seats, exit $probed: $health"
	expect "a full relay turns .ts away with 503" '[ "$code" = 503 ]' "got $code"
	expect "a full relay turns RTSP away with 503" '[[ "$described" == *" 503 "* ]]' "got ${described:-nothing}"
	kill "$FULL"
	timeout 15 tail --pid="$FULL" -f /dev/null && wait "$FULL"
	stopped=$?
	expect "a full relay stops cleanly" '[ "$stopped" = 0 ]' "status $stopped"
fi

code=$(curl -s --max-time 10 -o /dev/null -w '%{http_code}' http://127.0.0.1:8080/nope.ts)
expect "unknown .ts is 404" '[ "$code" = 404 ]' "got $code"
silence=$(python3 "$HERE/probe.py" silence 127.0.0.1 8554)
expect "a silent connection is closed after 5 s" '[ "$(jq -r ".closed_after // 99 | . >= 4.5 and . < 8" <<<"$silence")" = true ]' "$silence"
waited=$(python3 "$HERE/probe.py" wait 127.0.0.1 8554 vrc)
expect "wait counts from the DESCRIBE" '[ "$(jq -r "(.describe | test(\"503\")) and .seconds >= 4.5" <<<"$waited")" = true ]' "$waited"

publish 30 &
PUBLISHER=$!
expect "publisher goes live" 'wait_log "vrc: live" 15' "no live line"
curl -s --max-time 42 http://127.0.0.1:8080/vrc.ts -o "$WORK/http.ts" &
CURL=$!
curl -s --limit-rate 60K --max-time 25 http://127.0.0.1:8080/vrc.ts -o "$WORK/slow.ts" &
SLOW=$!
sleep 3
timeout 25 ffmpeg -v error -nostdin -rtsp_transport tcp -i rtsp://127.0.0.1:8554/vrc -t 8 -c copy -f mpegts "$WORK/rtsp1.ts" 2>"$WORK/rtsp1.log"
errors=$(decode "$WORK/rtsp1.ts")
length=$(duration "$WORK/rtsp1.ts")
expect "RTSP decodes cleanly" '[ -z "$errors" ] && [ "${length:-0}" -ge 7 ]' "${length:-0} s, $errors $(head -3 "$WORK/rtsp1.log")"
note "info rtsp1.ts: $(steps "$WORK/rtsp1.ts") dts steps (sender report mapping)"
limited=$(python3 "$HERE/probe.py" limit 198.51.100.1 8554 12)
expect "RTSP connections per address are capped" '[ "$(jq -r ".alive == 8 and .refused == 4" <<<"$limited")" = true ]' "$limited"
loopback=$(python3 "$HERE/probe.py" limit 127.0.0.1 8554 12)
expect "local addresses are not capped" '[ "$(jq -r ".alive == 12" <<<"$loopback")" = true ]' "$loopback"
sleep 1
shared=$(python3 "$HERE/probe.py" shared 198.51.100.1 8554 8 8080)
expect "RTSP and .ts share the limit of an address" '[ "$(jq -r "(.same | test(\"429\")) and (.other | test(\"200\"))" <<<"$shared")" = true ]' "$shared"
expect "the frontend's own X-Forwarded-For line wins over the client's" '[ "$(jq -r ".lines | test(\"429\")" <<<"$shared")" = true ]' "$shared"
expect "a long X-Forwarded-For still names its last address" '[ "$(jq -r ".long | test(\"429\")" <<<"$shared")" = true ]' "$shared"
expect "a .ts request without X-Forwarded-For is noted" 'wait_log "without X-Forwarded-For" 2' "no note"
timeout 6 ffmpeg -v quiet -nostdin -f lavfi -i testsrc2=size=320x240:rate=10 -t 1 -c:v libx264 -f mpegts \
	"srt://127.0.0.1:8890?streamid=vrc&passphrase=$PASS&pbkeylen=32&connect_timeout=3000" >/dev/null 2>&1
expect "a second publisher waits while one is live" 'wait_log "connection refused from .*, publisher .* is live" 3' "no refusal line"
for i in 1 2 3 4 5 6 7; do
	timeout 6 ffmpeg -v quiet -nostdin -f lavfi -i testsrc2=size=320x240:rate=10 -t 1 -c:v libx264 -f mpegts \
		"srt://127.0.0.1:8890?streamid=vrc&passphrase=wrong-passphrase-$i&pbkeylen=32&connect_timeout=3000" >/dev/null 2>&1
done
expect "failing SRT callers back off" 'wait_log "blocked for up to 10 s" 5' "no backoff line"
expect "each wrong passphrase is logged with its address" '[ "$(lines "connection refused from 127.0.0.1, wrong or missing passphrase")" = 5 ]' "$(lines "wrong or missing passphrase") lines"
expect "the live publisher is kept" '[ "$(lines "publisher .* replaced")" = 0 ]' "replaced"
wait "$PUBLISHER"
expect "offline video after OBS stops" 'wait_log "showing the offline video for 60 s" 10' "no offline line"
expect "the cap is logged" 'wait_log "from 198.51.100.1 refused in the last 10 s" 11' "no refusal line"
wait "$SLOW"
wait "$CURL"

report=$(tscheck "$WORK/http.ts")
note "http.ts: $report"
expect ".ts starts with the SDT then the PAT" '[ "$(jq -r ".first[0:2] == [\"0x11\", \"0x0\"]" <<<"$report")" = true ]' "$report"
expect ".ts stays aligned" '[ "$(jq -r ".sync and .tail == 0" <<<"$report")" = true ]' "$report"
expect ".ts never has a packet dropped as a repeat" '[ "$(jq -r ".dropped" <<<"$report")" = 0 ]' "$report"
expect ".ts skips one count per track at the switch" '[ "$(jq -c ".gaps" <<<"$report")" = "{\"0x102\":1,\"0x101\":1}" ] || [ "$(jq -c ".gaps" <<<"$report")" = "{\"0x101\":1,\"0x102\":1}" ]' "$report"
expect ".ts time never goes back" '[ "$(jq -r ".backwards" <<<"$report")" = 0 ] && [ "$(jq -r ".video_gap_ms >= 300 and .video_gap_ms < 600 and .seconds >= 38" <<<"$report")" = true ]' "$report"
expect "the offline video loops without a cut" '[ "$(jq -r ".cuts" <<<"$report")" = 1 ]' "$report"
expect "the sound joins at every pass of the offline video" '[ "$(jq -r ".seams" <<<"$report")" = 0 ]' "$report"
slow=$(tscheck "$WORK/slow.ts")
note "slow.ts: $slow"
expect "a slow viewer stays aligned and loses no unit start to a repeat" '[ "$(jq -r ".sync and .harmful == 0" <<<"$slow")" = true ]' "$slow"
expect "the slow viewer skips ahead" '[ "$(lines "fell behind and skipped ahead")" -gt 0 ]' "no skip logged"
expect "jumps send their fillers" '[ "$(jq -r ".fillers > 0" <<<"$slow")" = true ]' "$slow"

offline=$(lines "offline video")
relay_signal HUP
expect "an unchanged reload is applied" 'wait_log "configuration reloaded" 5' "no reload line"
sleep 1
expect "an unchanged video is kept" '[ "$(lines "offline video")" = "$offline" ]' "offline video lines went from $offline to $(lines "offline video")"
config "$WORK/big.ts" vrc
relay_signal HUP
expect "a video over 64 MiB is refused" 'wait_log "larger than 64 MiB" 5 && wait_log "configuration kept" 5' "no refusal"
config "$WORK/offline.ts" vrc
kept=$(lines "configuration kept")
if [ "$MODE" = docker ]; then
	streams "$WORK/crossfade.conf" 14
else
	sed -i 's/^buffer 16$/buffer 1024/' "$WORK/crossfade.conf"
	streams "$WORK/crossfade.conf" 15
fi
relay_signal HUP
expect "a reload adding streams that do not fit in memory is refused" 'wait_log "not fit in memory" 5 && sleep 0.5 && [ "$(lines "configuration kept")" -gt "$kept" ]' "$(relay_log | tail -2)"
config "$WORK/offline.ts" vrc
if [ "$MODE" = docker ]; then
	sed -i 's/^buffer 16$/buffer 4/' "$WORK/crossfade.conf"
	streams "$WORK/crossfade.conf" 14
	relay_signal HUP
	expect "a refused reload says when a restart would fit the streams" 'wait_log "they fit after a restart" 5' "$(relay_log | tail -2)"
	config "$WORK/offline.ts" vrc
fi

curl -s --max-time 15 http://127.0.0.1:8080/vrc.ts -o "$WORK/http2.ts" &
CURL=$!
sleep 3
publish 12 &
PUBLISHER=$!
expect "publisher comes back" 'wait_log "vrc: live again" 15' "no live again line"
sleep 1
timeout 15 ffmpeg -v error -nostdin -rtsp_transport tcp -i rtsp://127.0.0.1:8554/vrc -t 5 -c copy -f mpegts "$WORK/rtsp2.ts" 2>"$WORK/rtsp2.log"
errors=$(decode "$WORK/rtsp2.ts")
expect "RTSP decodes cleanly after the switch" '[ -z "$errors" ] && [ "$(duration "$WORK/rtsp2.ts")" -ge 4 ]' "$errors $(head -3 "$WORK/rtsp2.log")"
wait "$CURL"
report=$(tscheck "$WORK/http2.ts")
note "http2.ts: $report"
expect ".ts across offline to live" '[ "$(jq -r ".sync and .harmful == 0 and .fillers == 0 and .backwards == 0 and .seams == 0 and .video_gap_ms >= 300 and .video_gap_ms < 600" <<<"$report")" = true ]' "$report"

curl -s --max-time 20 http://127.0.0.1:8080/vrc.ts -o "$WORK/http3.ts" &
CURL=$!
sleep 2
config "$WORK/offline.ts" vrc2
started=$SECONDS
relay_signal HUP
expect "the renamed stream takes the slot" 'wait_log "vrc: removed" 5 && wait_log "vrc2: new stream added" 5' "no rename lines"
wait "$CURL"
expect "its viewers are cut at once" '[ $((SECONDS - started)) -lt 10 ]' "$((SECONDS - started)) s"
code=$(curl -s --max-time 10 -o /dev/null -w '%{http_code}' http://127.0.0.1:8080/vrc.ts)
expect "the old name is gone" '[ "$code" = 404 ]' "got $code"
code=$(curl -s -o /dev/null --max-time 10 -w '%{http_code}' http://127.0.0.1:8080/vrc2.ts)
expect "the new stream waits for its publisher" '[ "$code" = 503 ]' "got $code"
wait "$PUBLISHER"
config none vrc2
relay_signal HUP
expect "a reload names how long .ts viewers wait without a video" 'wait_log "vrc2: no offline video, .ts viewers kept 60 s" 5' "no line"
publish 4 vrc2 &
PUBLISHER=$!
expect "the stream without a video goes live" 'wait_log "vrc2: live" 15' "no live line"
idled=$(curl -s -Y 100 -y 9 --max-time 14 -o /dev/null -w '%{time_total}' http://127.0.0.1:8080/vrc2.ts)
expect "a .ts viewer keeps receiving while OBS is away without a video" '[ "${idled%.*}" -ge 13 ]' "curl ended after ${idled} s"
wait "$PUBLISHER"

relay_stop
expect "relay stops cleanly" '[ "$STATUS" = 0 ] && wait_log "stopped" 2' "status $STATUS"
sanitizer=$({ relay_log; cat "$WORK/full.log" "$WORK/huge.log" 2>/dev/null; } | grep -E -m 20 -A 12 "ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:|(WARNING|ERROR): ThreadSanitizer")
expect "no sanitizer report" '[ -z "$sanitizer" ]' "see the relay log"

echo "::notice title=$MODE summary::$(escape "$SUMMARY")"
echo "::notice title=$MODE relay log::$(escape "$(relay_log | tail -n 80)")"
if [ -n "$sanitizer" ]; then
	echo "::error title=$MODE sanitizer::$(escape "$sanitizer")"
fi
if [ "$MODE" = docker ]; then
	docker rm -f cf >/dev/null 2>&1
fi
[ "$FAILED" = 0 ]
