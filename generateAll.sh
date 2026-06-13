#!/bin/bash
# Batch-render every ordered pair within each image set.
source "$(dirname "$0")/animate.sh"

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
