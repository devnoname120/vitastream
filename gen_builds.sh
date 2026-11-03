#!/usr/bin/env bash

set -euo pipefail

# Use a dedicated directory so earlier builds and test receipts are preserved.
output_dir=${1:-builds/release}
mkdir -p "$output_dir"
output_dir=$(cd "$output_dir" && pwd)

build_variant() {
	local artifact=$1
	shift
	make clean DEBUG=1 DIAGNOSTIC=1
	# Match the bounded single-buffer configuration tested on retail hardware.
	make vitastream.elf vitastream.skprx VIDEO_KERNEL_POOL=1 "$@"
	cp vitastream.skprx "$output_dir/$artifact.skprx"
	cp vitastream.elf "$output_dir/$artifact.elf"
}

build_variant vitastream
build_variant vitastream_oled_off DISPLAY_OFF_OLED=1
build_variant vitastream_lcd_off DISPLAY_OFF_LCD=1
build_variant vitastream_diagnostic DIAGNOSTIC=1
cp vitastream.cfg "$output_dir/vitastream.cfg"

make clean DEBUG=1 DIAGNOSTIC=1
