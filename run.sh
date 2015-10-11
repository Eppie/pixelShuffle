#!/bin/bash

time ./pngRGB images/$1.png images/$2.png results/$1TO$2.png

time png2yuv -I p -f 24 -b 0 -n 250 -j out%05d.png > $1TO$2.yuv

time vpxenc $1TO$2.yuv -o results/$1TO$2.webm --codec=vp8 --passes=2 --threads=6 --best --target-bitrate=10000 --end-usage=vbr --auto-alt-ref=1 -v --minsection-pct=5 --maxsection-pct=800 --lag-in-frames=16 --kf-min-dist=0 --kf-max-dist=360 --token-parts=2 --static-thresh=0 --drop-frame=0 --min-q=0 --max-q=60

time optipng -v -o3 results/$1TO$2.png

xdg-open results/$1TO$2.webm

rm -f out* first.png $1TO$2.yuv
