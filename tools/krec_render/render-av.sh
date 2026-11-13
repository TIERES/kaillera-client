#!/usr/bin/env bash
# Prototype pipeline on the broadcaster VM:
#   krec_render.exe (Wine, no X) --video on stdout--> ffmpeg (H.264 + AAC) -> file or RTMP
#                                --audio on a FIFO-->
# Env: KREC, FROM (first output frame), TO (last frame), OUT (file or rtmp:// URL),
#      RE_PACE=1 to pace the output at real time (-re, for a live RTMP push), LV (ffmpeg log level).
set -u
BASE=${BASE:-/home/opc/wecamp-playback}
R=$BASE/run/render
cd "$BASE"
unset DISPLAY
export WINEPREFIX=$BASE/wineprefix WINEDEBUG=-all
K=${KREC:-records/PCSX_ReARMed_BR_2012.bin_ANDERSHOW_Jefferson_Coimbra_SemMCard.krec}
FROM=${FROM:-7200}; TO=${TO:-10800}; OUT=${OUT:-$R/sample-av.mp4}; LV=${LV:-error}
FMT=mp4; case "$OUT" in rtmp*) FMT=flv ;; esac
RE=(); [ "${RE_PACE:-0}" = 1 ] && RE=(-re)

rm -f "$R/a.fifo"; mkfifo "$R/a.fifo"
# Wine accepts forward slashes; a relative path avoids shell-quoting of backslashes.
wine64 run/render/krec_render.exe --game BR2012.bin --krec "$K" --video --every 2 \
    --video-from "$FROM" --frames "$TO" --report 1800 --audio run/render/a.fifo 2> "$R/av-render.log" |
  LD_LIBRARY_PATH=$BASE/run/codec/usr/lib64 ffmpeg -hide_banner -nostdin -loglevel "$LV" -stats \
    "${RE[@]}" -probesize 32 -analyzeduration 0 -thread_queue_size 64 -f rawvideo -pix_fmt rgb565le -s 640x480 -framerate 30 -i - \
    "${RE[@]}" -probesize 32 -analyzeduration 0 -thread_queue_size 1024 -f s16le -ar 44100 -ac 2 -i "$R/a.fifo" \
    -map 0:v -map 1:a -c:v libopenh264 -profile:v main -rc_mode 1 -b:v 1500k -maxrate 2000k -bufsize 3000k -g 60 \
    -pix_fmt yuv420p -c:a aac -b:a 128k -ar 44100 -f "$FMT" -y "$OUT" 2> "$R/av-ffmpeg.log"
rm -f "$R/a.fifo"
grep -v -E "^[0-9a-f]{4}:" "$R/av-render.log" | tail -4
tr "\r" "\n" < "$R/av-ffmpeg.log" | grep -v "^\s*$" | tail -2
