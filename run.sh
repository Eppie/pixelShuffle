#!/bin/bash

outFileName="${1}TO${2}"
time ./pngLAB images/$1.png images/$2.png LAB/$outFileName.png

for k in {00001..00023}; do
	cp out${outFileName}00000.png out${outFileName}${k}.png
done

for k in {00274..00299}; do
	cp out${outFileName}00273.png out${outFileName}${k}.png
done

time png2yuv -I p -f 24 -b 0 -n 300 -j out${outFileName}%05d.png > $outFileName.yuv

time vpxenc -v $outFileName.yuv -o LAB/$outFileName.webm --codec=vp8 --passes=2 --threads=6 --best --target-bitrate=10000 --end-usage=vbr --auto-alt-ref=1 --minsection-pct=5 --maxsection-pct=800 --lag-in-frames=16 --kf-min-dist=0 --kf-max-dist=360 --token-parts=2 --static-thresh=0 --drop-frame=0 --min-q=0 --max-q=60

time optipng -v -o3 LAB/$outFileName.png

rm -f out* $outFileName.yuv

xdg-open LAB/$1TO$2.webm

