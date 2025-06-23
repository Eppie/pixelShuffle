assembly=-masm=intel -fverbose-asm -Wa,-ahldnc
PROFILE_FLAGS = -DENABLE_PROFILING

all: rgb lab

rgb:
	g++ -DOUTPUT -DANIMATION -fno-stack-protector -march=native -mtune=native -g -std=c++11 -Ofast pngRGB.cpp -o pngRGB $(assembly) -lpng > pngRGB.s

lab:
	g++ -DOUTPUT -DANIMATION -fno-stack-protector -march=native -mtune=native -g -std=c++11 -Ofast pngLAB.cpp -o pngLAB $(assembly) -lpng > pngLAB.s

rgb-profiled:
	g++ $(PROFILE_FLAGS) -DOUTPUT -DANIMATION -fno-stack-protector -march=native -mtune=native -g -std=c++11 -Ofast pngRGB.cpp -o pngRGB $(assembly) -lpng > pngRGB.s

lab-profiled:
	g++ $(PROFILE_FLAGS) -DOUTPUT -DANIMATION -fno-stack-protector -march=native -mtune=native -g -std=c++11 -Ofast pngLAB.cpp -o pngLAB $(assembly) -lpng > pngLAB.s

debug:
	g++ -march=native -mtune=native -g -std=c++11 -O0 pngLAB.cpp -o pngLAB $(assembly) -lpng > pngLAB.s
	g++ -march=native -mtune=native -g -std=c++11 -O0 pngRGB.cpp -o pngRGB $(assembly) -lpng > pngRGB.s

clean:
	rm -rf pngLAB pngRGB *out* test* *.s

# The existing profile target uses valgrind, which is different from the performance tracing requested.
# I'll leave it as is, but the new profiling is via lab-profiled or rgb-profiled
profile: lab
	valgrind -v --tool=callgrind --log-fd=1 --dump-instr=yes --collect-jumps=yes --cache-sim=yes --branch-sim=yes --simulate-wb=yes --simulate-hwpref=yes --cacheuse=yes ./pngLAB images/mona.png images/gothic.png test.png

