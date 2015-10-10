#!/bin/bash

time ./pngRGB images/$1.png images/$2.png results/$1TO$2.png
time convert -delay 100 first.png -delay 4 out* -delay 200 results/$1TO$2.png -delay 100 images/$2.png -loop 0 results/$1TO$2.gif
xdg-open results/$1TO$2.gif
rm out* first.png
