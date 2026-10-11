#!/usr/bin/env bash

rm -rf builds
mkdir -p builds

make
cp vitastream.skprx builds/vitastream.skprx

make clean
make DISPLAY_OFF_OLED=1
cp vitastream.skprx builds/vitastream_oled_off.skprx

make clean
make DISPLAY_OFF_LCD=1
cp vitastream.skprx builds/vitastream_lcd_off.skprx

make clean
