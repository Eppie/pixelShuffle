#include <iostream> // for cout
#include <string>
#include <sstream>
#include <iomanip>
#include <string.h> // for memcpy
#include "pngReadWrite.h"

using namespace std;

int diffR;
int diffG;
int diffB;
string filePrefix;

png_bytep *rowPointersSrc = (png_bytep*) malloc( sizeof( png_bytep ) * 5000 );
png_bytep *rowPointersNew = (png_bytep*) malloc( sizeof( png_bytep ) * 5000 );
png_bytep *rowPointersDst = (png_bytep*) malloc( sizeof( png_bytep ) * 5000 );

png_bytep **srcPtr = &rowPointersSrc;
png_bytep **newPtr = &rowPointersNew;
png_bytep **dstPtr = &rowPointersDst;

uint64_t x = 0x8E588AFE51D8B00D;

inline uint64_t xorshift64star() {
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	return x * 2685821657736338717ULL;
}

inline int pixelDiff( png_bytep px1, png_bytep px2 ) {
	diffR = px1[0] - px2[0];
	diffG = px1[1] - px2[1];
	diffB = px1[2] - px2[2];
	return (diffR * diffR) + (diffG * diffG) + (diffB * diffB);
}

inline void swapPixels( png_bytep px1, png_bytep px2 ) {
	swap( px1[0], px2[0] );
	swap( px1[1], px2[1] );
	swap( px1[2], px2[2] );
}

inline bool determineSwap( png_bytep sPx1, png_bytep sPx2, png_bytep dPx1, png_bytep dPx2 ) {
	return pixelDiff( sPx1, dPx2 ) + pixelDiff( sPx2, dPx1 ) < pixelDiff( sPx1, dPx1 ) + pixelDiff( sPx2, dPx2 );
}

unsigned long long totalDiff( png_bytep *src, png_bytep *dst ) {
	unsigned long long totalDiff = 0;
	png_bytep rowSrc;
	png_bytep rowDst;
	png_bytep sPx;
	png_bytep dPx;
	int length = dHeight * dWidth;

	for( int i = 0; i < length; i++ ) {
		rowSrc = src[(i / dHeight) % dHeight];
		rowDst = dst[(i / dHeight) % dHeight];
		sPx = &(rowSrc[( i % dWidth ) * 4]);
		dPx = &(rowDst[( i % dWidth ) * 4]);
		totalDiff += pixelDiff( sPx, dPx );
	}
	return totalDiff;
}

void processPNGFile( png_bytep *src, png_bytep *dst ) {
	int k;

	int x1;
	int x2;
	int y1;
	int y2;

	png_bytep sy1;
	png_bytep sy2;
	png_bytep dy1;
	png_bytep dy2;
	png_bytep sPx1;
	png_bytep sPx2;
	png_bytep dPx1;
	png_bytep dPx2;

	unsigned long long t;
	int orderedLoopCount = 150;
	int randomLoopCount = 100;

	for( int l = 0; l < 1; l++ ) {
		for( int j = 0; j < orderedLoopCount; j++ ) {
			for( int i = 0; i < 3e5; i++ ) {
				k = i + (j*i);

				sy1 = src[(k/dHeight)%dHeight];
				sy2 = src[k%dHeight];
				dy1 = dst[(k/dHeight)%dHeight];
				dy2 = dst[k%dHeight];

				sPx1 = &(sy1[((k/dWidth)%dWidth)*4]);
				sPx2 = &(sy2[(k%dWidth)*4]);
				dPx1 = &(dy1[((k/dWidth)%dWidth)*4]);
				dPx2 = &(dy2[(k%dWidth)*4]);
				if( determineSwap( sPx1, sPx2, dPx1, dPx2 ) ) {
					swapPixels( sPx1, sPx2 );
				}
			}
			t = totalDiff( src, dst );
			cout << "Iteration #" << j + ( ( orderedLoopCount + randomLoopCount ) * l ) << ", Diff: " << t << " (ordered)" << endl;
			ostringstream ss;
			ss << setw(5) << setfill('0') << 24 + j + ( ( orderedLoopCount + randomLoopCount ) * l );
			string s2(ss.str());
			string newFilename = filePrefix + s2 + ".png";
			writePNGFile( newFilename.c_str(), *newPtr );
		}

		for( int j = 0; j < randomLoopCount; j++ ) {
			for( int i = 0; i < j*1e5; i++ ) {
				uint64_t r = xorshift64star();
				int r1 = r & 0xFFFF;
				int r2 = (r & (0xFFFFULL << 16)) >> 16;
				int r3 = (r & (0xFFFFULL << 32)) >> 32;
				int r4 = (r & (0xFFFFULL << 48)) >> 48;
				//cout << hex << r4 << r3 << r2 << r1 << endl;

				y1 = r1 % dHeight;
				y2 = r2 % dHeight;
				x1 = r3 % dWidth;
				x2 = r4 % dWidth;

				sy1 = src[y1];
				sy2 = src[y2];
				dy1 = dst[y1];
				dy2 = dst[y2];

				sPx1 = &(sy1[x1*4]);
				sPx2 = &(sy2[x2*4]);
				dPx1 = &(dy1[x1*4]);
				dPx2 = &(dy2[x2*4]);
				if( determineSwap( sPx1, sPx2, dPx1, dPx2 ) ) {
					swapPixels( sPx1, sPx2 );
				}
			}
			t = totalDiff( src, dst );
			cout << "Iteration #" << j + ( ( orderedLoopCount + randomLoopCount ) * l ) + orderedLoopCount << ", Diff: " << t << " (random)" << endl;
			ostringstream ss;
			ss << setw(5) << setfill('0') << 24 + j + ( ( orderedLoopCount + randomLoopCount ) * l ) + orderedLoopCount;
			string s2(ss.str());
			string newFilename = filePrefix + s2 + ".png";
			writePNGFile( newFilename.c_str(), *newPtr );
		}
	}
}

string split( string &s ) {
	stringstream ss( s );
	string result;
	getline( ss, result, '/' );
	getline( ss, result, '.' );
	return result;
}

int main( int argc, char *argv[] ) {

	readPNGFile( argv[1], *srcPtr, &sWidth, &sHeight );
	readPNGFile( argv[2], *dstPtr, &dWidth, &dHeight );
	
	// Output shape should be that of the dst image.
	rowPointersNew = (png_bytep*) realloc( rowPointersNew, sizeof( png_bytep ) * dHeight );
	for( int y = 0; y < dHeight; y++ ) {
		rowPointersNew[y] = (png_byte*) malloc( sizeof( png_bytep ) * dWidth * 4 );
	}

	for( int i = 0; i < dHeight * dWidth; i++ ) {
		memcpy( &rowPointersNew[i / dWidth][((i % dWidth)*4)], &rowPointersSrc[i / sWidth][((i % sWidth)*4)], 4 );
	}

	ostringstream ss;
	ss << argv[3];
	filePrefix = ss.str();
	filePrefix = "out" + split( filePrefix );
	writePNGFile( string( filePrefix + "00000.png" ).c_str(), *newPtr );
	
	processPNGFile( *newPtr, *dstPtr );
	writePNGFile( argv[3], *newPtr, true );

	return 0;
}
