#!/bin/bash

function generateImage {
	outFileName="${1}TO${2}"
	time ./pngRGB images/$1.png images/$2.png results/$outFileName.png

	for k in {00001..00023}; do
		cp out${outFileName}00000.png out${outFileName}${k}.png
	done

	for k in {00274..00299}; do
		cp out${outFileName}00273.png out${outFileName}${k}.png
	done

	time png2yuv -I p -f 24 -b 0 -n 300 -j out${outFileName}%05d.png > $outFileName.yuv

	time vpxenc -v $outFileName.yuv -o results/$outFileName.webm --codec=vp8 --passes=2 --threads=6 --best --target-bitrate=10000 --end-usage=vbr --auto-alt-ref=1 --minsection-pct=5 --maxsection-pct=800 --lag-in-frames=16 --kf-min-dist=0 --kf-max-dist=360 --token-parts=2 --static-thresh=0 --drop-frame=0 --min-q=0 --max-q=60

	time optipng -v -o3 results/$outFileName.png

	rm -f out* $outFileName.yuv
}

#small=(balls gothic mona scream starry stream)

#for i in ${small[@]}; do
	#for j in ${small[@]}; do
		#if [ $i != $j ]; then
			#echo "Now starting on ${i}TO${j}"
			#generateImage $i $j &
		#fi
	#done
	#wait
#done

#big=(b2w gothicBig monaBig starryBig)
big=(b2w w2b)

for i in ${big[@]}; do
	for j in ${big[@]}; do
		if [ $i != $j ]; then
			echo "Now starting on ${i}TO${j}"
			generateImage $i $j &
		fi
	done
	wait
done

