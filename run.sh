#!/bin/bash
# Render one palette->target animation and open the result.
# Usage: ./run.sh <variant-suffix> <palette-stem> <target-stem>   e.g. ./run.sh LAB mona gothic
source "$(dirname "$0")/animate.sh"

generateImage "$1" "$2" "$3"

xdg-open ${1}/$2TO$3.webm
