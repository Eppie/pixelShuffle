#!/bin/bash

function generateImage {
	outFileName="${2}TO${3}"
	time ./png$1 images/$2.png images/$3.png $1/$outFileName.png

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
		cp images/$3.png out${outFileName}${k}.png
	done

	# Combine all the pngs into a raw video
	time png2yuv -I p -f 24 -b 0 -n 325 -j out${outFileName}%05d.png > $outFileName.yuv

	# Convert the raw video into a webm
	time vpxenc -v $outFileName.yuv -o $1/$outFileName.webm --codec=vp8 --passes=2 --threads=12 --target-bitrate=30000 --end-usage=vbr --auto-alt-ref=1 --minsection-pct=5 --maxsection-pct=800 --lag-in-frames=16 --kf-min-dist=0 --kf-max-dist=360 --token-parts=3 --static-thresh=0 --drop-frame=0 --min-q=0 --max-q=30

	# Reduce the filesize of the output png
	time optipng -v -o3 $1/$outFileName.png

	rm -f out${outFileName}*.png $outFileName.yuv
}

function processArray {
	declare -a arr=("${!1}")
	echo "${arr[@]}"
	for i in ${arr[@]}; do
		for j in ${arr[@]}; do
			if [ $i != $j ]; then
				echo "Now starting on ${i}TO${j}"
				generateImage $2 $i $j &
			fi
		done
		wait
	done
}

small=(balls dali gothic mona nikolai scream starry stream)
big=(b2w daliBig gothicBig monaBig nikolaiBig starryBig rgbGradient1)
huge=(monaHuge gothicHuge starryHuge)
gradient1=(b2w w2b)
gradient2=(rgbGradient1 rgbGradient2)

processArray small[@] RGB
processArray big[@] RGB
processArray huge[@] RGB
processArray gradient1[@] RGB
processArray gradient2[@] RGB

