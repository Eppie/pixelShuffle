#!/bin/bash

outFileName="${1}TO${2}"
time ./pngLAB images/$1.png images/$2.png LAB/$outFileName.png

# Display the starting image for 1 second before beginning animation (24 fps)
for k in {00001..00023}; do
	cp out${outFileName}00000.png out${outFileName}${k}.png
done

# Display our final result for 1 second
for k in {00274..00299}; do
	cp out${outFileName}00273.png out${outFileName}${k}.png
done

# Display the original palette image for 1 second for comparison
for k in {00300..00324}; do
	cp images/$2.png out${outFileName}${k}.png
done

# Combine all the pngs into a raw video
time png2yuv -I p -f 24 -b 0 -n 325 -j out${outFileName}%05d.png > $outFileName.yuv

# Convert the raw video into a webm
time vpxenc -v $outFileName.yuv -o LAB/$outFileName.webm --codec=vp8 --passes=2 --threads=12 --target-bitrate=30000 --end-usage=vbr --auto-alt-ref=1 --minsection-pct=5 --maxsection-pct=800 --lag-in-frames=16 --kf-min-dist=0 --kf-max-dist=360 --token-parts=3 --static-thresh=0 --drop-frame=0 --min-q=0 --max-q=30

# Reduce the filesize of the output png
time optipng -v -o3 LAB/$outFileName.png

rm -f out${outFileName}*.png $outFileName.yuv

xdg-open LAB/$1TO$2.webm

