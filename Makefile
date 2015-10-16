assembly=-masm=intel -fverbose-asm -Wa,-ahldnc

rgb:
	g++ -march=native -mtune=native -g -std=c++11 -Ofast -lpng pngRGB.cpp -o pngRGB $(assembly) > pngRGB.s

lab:
	g++ -march=native -mtune=native -g -std=c++11 -Ofast -lpng pngLAB.cpp -o pngLAB $(assembly) > pngLAB.s

debug:
	g++ -march=native -mtune=native -g -std=c++11 -O0 -lpng pngLAB.cpp -o pngLAB $(assembly) > pngLAB.s
	g++ -march=native -mtune=native -g -std=c++11 -O0 -lpng pngRGB.cpp -o pngRGB $(assembly) > pngRGB.s

clean:
	rm -rf pngLAB pngRGB *out* test* *.s

profile: rgb
	valgrind -v --tool=callgrind --log-fd=1 --dump-instr=yes --collect-jumps=yes --cache-sim=yes --branch-sim=yes --simulate-wb=yes --simulate-hwpref=yes --cacheuse=yes ./pngRGB images/mona.png images/gothic.png test.png

