#!/bin/bash
# Encode MJPEG video for the WonderMedia WM8505
#
# Usage: [RES=WIDTHxHEIGHT] wmt-encode.sh INPUT [OUTPUT]
# Output defaults to INPUT with an .avi extension, beside the source.
#
# Copyright (C) 2026 Logan Russell <me@lrussell.net>

set -eu

die() { echo "wmt-encode: $*" >&2; exit 1; }

[ $# -eq 1 ] || [ $# -eq 2 ] || die "usage: wmt-encode.sh INPUT [OUTPUT]"
in=$1 out=${2:-${1%.*}.avi}
[ -f "$in" ] || die "no such file: $in"
[ "$out" = "$in" ] && out=${1%.*}.wmt.avi

RES=${RES:-800x480}
w=${RES%x*} h=${RES#*x}

# Convert matrix to BT.601 for the hardware color converters
exec ffmpeg -nostdin -hide_banner -y -i "$in" \
	-vf "scale='min($w,iw)':'min($h,ih)':force_original_aspect_ratio=decrease:flags=lanczos+accurate_rnd+full_chroma_int:out_color_matrix=bt601,format=yuv420p" \
	-c:v mjpeg -b:v 20M -maxrate 20M -bufsize 2M \
	-c:a pcm_s16le -ar 48000 -ac 2 "$out"
