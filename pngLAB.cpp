#include <iostream>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <string.h> // for memcpy
#include "pngReadWrite.h"

using namespace std;

double diffL;
double diffa;
double diffb;
string filePrefix;

struct Color {
	double A;
	double B;
	double C;
	png_byte r;
	png_byte g;
	png_byte b;
};

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

inline Color RGBToXYZ( png_bytep px ) {
	double R = px[0] / 255.0;
	double G = px[1] / 255.0;
	double B = px[2] / 255.0;

	R = R > 0.04045 ? pow( ( ( R + 0.055 ) / 1.055 ), 2.4 ) : R / 12.92;
	G = G > 0.04045 ? pow( ( ( G + 0.055 ) / 1.055 ), 2.4 ) : G / 12.92;
	B = B > 0.04045 ? pow( ( ( B + 0.055 ) / 1.055 ), 2.4 ) : B / 12.92;

	R *= 100.0;
	G *= 100.0;
	B *= 100.0;

	double X = (R * 0.4124) + (G * 0.3576) + (B * 0.1805);
	double Y = (R * 0.2126) + (G * 0.7152) + (B * 0.0722);
	double Z = (R * 0.0193) + (G * 0.1192) + (B * 0.9505);

	Color result = { X, Y, Z, px[0], px[1], px[2] };
	return result;
}

inline Color XYZToLab( Color px ) {
	double X = px.A / 95.047;
	double Y = px.B / 100.0;
	double Z = px.C / 108.883;

	X = X > 0.008856 ? pow( X, 1.0 / 3.0 ) : ( X * 7.787 ) + ( 16.0 / 116.0 );
	Y = Y > 0.008856 ? pow( Y, 1.0 / 3.0 ) : ( Y * 7.787 ) + ( 16.0 / 116.0 );
	Z = Z > 0.008856 ? pow( Z, 1.0 / 3.0 ) : ( Z * 7.787 ) + ( 16.0 / 116.0 );

	double L = ( Y * 116.0 ) - 16.0;
	double a = ( X - Y ) * 500.0;
	double b = ( Y - Z ) * 200.0;

	Color result = { L, a, b, px.r, px.g, px.b };
	return result;
}

inline Color RGBToLab( png_bytep px ) {
	Color XYZ = RGBToXYZ( px );
	Color Lab = XYZToLab( XYZ );
	return Lab;
}

inline double pixelDiff( Color* px1, Color* px2 ) {
	diffL = px1->A - px2->A;
	diffa = px1->B - px2->B;
	diffb = px1->C - px2->C;
	return (diffL * diffL) + (diffa * diffa) + (diffb * diffb);
}

inline void swapPixels( Color* px1, Color* px2 ) {
	swap( px1->A, px2->A );
	swap( px1->B, px2->B );
	swap( px1->C, px2->C );
	swap( px1->r, px2->r );
	swap( px1->g, px2->g );
	swap( px1->b, px2->b );
}

inline Color** imageToLab( png_bytep *image ) {
	Color lab;
	png_bytep oldRow;
	Color* newRow;
	png_bytep px;
	Color **result = (Color**) malloc( sizeof( Color* ) * dHeight );
	for( int y = 0; y < dHeight; y++ ) {
		result[y] = (Color*) malloc( sizeof( Color ) * dWidth );
		newRow = result[y];
		oldRow = image[y];
		for( int x = 0; x < dWidth; x++ ) {
			px = &oldRow[x*4];
			lab = RGBToLab( px );
			newRow[x] = lab;
		}
	}
	return result;
}

inline void labToImage( Color **lab, png_bytep *image ) {
	png_bytep newRow;
	Color* oldRow;
	Color RGB;
	for( int y = 0; y < dHeight; y++ ) {
		oldRow = lab[y];
		newRow = image[y];
		for( int x = 0; x < dWidth; x++ ) {
			RGB = oldRow[x];
			newRow[(x*4)+0] = RGB.r;
			newRow[(x*4)+1] = RGB.g;
			newRow[(x*4)+2] = RGB.b;
		}
	}
}

unsigned long long totalDiff( Color **src, Color **dst ) {
	unsigned long long totalDiff = 0;
	Color* rowSrc;
	Color* rowDst;
	Color* sPx;
	Color* dPx;
	int length = dHeight * dWidth;

	for( int i = 0; i < length; i++ ) {
		rowSrc = src[(i / dHeight) % dHeight];
		rowDst = dst[(i / dHeight) % dHeight];
		sPx = &(rowSrc[( i % dWidth )]);
		dPx = &(rowDst[( i % dWidth )]);
		totalDiff += pixelDiff( sPx, dPx );
	}
	return totalDiff;
}

void processPNGFile( Color** src, Color** dst ) {
	int k;

	int x1;
	int x2;
	int y1;
	int y2;

	Color *sy1, *sy2, *dy1, *dy2;
	Color *sPx1, *sPx2, *dPx1, *dPx2;
	
	unsigned long long t;
	int orderedLoopCount = 150;
	int randomLoopCount = 100;

	srand( time( NULL ) );

	for( int l = 0; l < 1; l++ ) {
		for( int j = 0; j < orderedLoopCount; j++ ) {
			for( int i = 0; i < 3e5; i++ ) {
				k = i + (j*i);

				sy1 = src[(k/dHeight)%dHeight];
				sy2 = src[k%dHeight];
				dy1 = dst[(k/dHeight)%dHeight];
				dy2 = dst[k%dHeight];

				sPx1 = &(sy1[((k/dWidth)%dWidth)]);
				sPx2 = &(sy2[(k%dWidth)]);
				dPx1 = &(dy1[((k/dWidth)%dWidth)]);
				dPx2 = &(dy2[(k%dWidth)]);
				if( pixelDiff( sPx1, dPx2 ) + pixelDiff( sPx2, dPx1 ) < pixelDiff( sPx1, dPx1 ) + pixelDiff( sPx2, dPx2 ) ) {
					swapPixels( sPx1, sPx2 );
				}
			}
			t = totalDiff( src, dst );
			cout << "Iteration #" << j + ( ( orderedLoopCount + randomLoopCount ) * l ) << ", Diff: " << t << " (ordered)" << endl;
			ostringstream ss;
			ss << setw(5) << setfill('0') << 24 + j + ( ( orderedLoopCount + randomLoopCount ) * l );
			string s2(ss.str());
			string newFilename = filePrefix + s2 + ".png";
			labToImage( src, *newPtr );
			writePNGFile( newFilename.c_str(), *newPtr );
		}

		for( int j = 0; j < randomLoopCount; j++ ) {
			for( int i = 0; i < j*1e5; i++ ) {
				uint64_t r = xorshift64star();
				int r1 = r & 0xFFFF;
				int r2 = (r & (0xFFFFULL << 16)) >> 16;
				int r3 = (r & (0xFFFFULL << 32)) >> 32;
				int r4 = (r & (0xFFFFULL << 48)) >> 48;

				y1 = r1 % dHeight;
				y2 = r2 % dHeight;
				x1 = r3 % dWidth;
				x2 = r4 % dWidth;

				sy1 = src[y1];
				sy2 = src[y2];
				dy1 = dst[y1];
				dy2 = dst[y2];

				sPx1 = &(sy1[x1]);
				sPx2 = &(sy2[x2]);
				dPx1 = &(dy1[x1]);
				dPx2 = &(dy2[x2]);
				if( pixelDiff( sPx1, dPx2 ) + pixelDiff( sPx2, dPx1 ) < pixelDiff( sPx1, dPx1 ) + pixelDiff( sPx2, dPx2 ) ) {
					swapPixels( sPx1, sPx2 );
				}
			}
			t = totalDiff( src, dst );
			cout << "Iteration #" << j + ( ( orderedLoopCount + randomLoopCount ) * l ) + orderedLoopCount << ", Diff: " << t << " (random)" << endl;
			ostringstream ss;
			ss << setw(5) << setfill('0') << 24 + j + ( ( orderedLoopCount + randomLoopCount ) * l ) + orderedLoopCount;
			string s2(ss.str());
			string newFilename = filePrefix + s2 + ".png";
			labToImage( src, *newPtr );
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
		rowPointersNew[y] = (png_bytep) malloc( sizeof( png_bytep ) * dWidth * 4 );
	}

	for( int i = 0; i < dHeight * dWidth; i++ ) {
		memcpy( &rowPointersNew[i / dWidth][((i % dWidth)*4)], &rowPointersSrc[i / sWidth][((i % sWidth)*4)], 4 );
	}

	Color** srcLab = imageToLab( *newPtr );
	Color** dstLab = imageToLab( *dstPtr );
	
	ostringstream ss;
	ss << argv[3];
	filePrefix = ss.str();
	filePrefix = "out" + split( filePrefix );
	writePNGFile( string( filePrefix + "00000.png" ).c_str(), *newPtr );

	processPNGFile( srcLab, dstLab );
	
	labToImage( srcLab, *newPtr );
	writePNGFile( argv[3], *newPtr, true );
	
	return 0;
}
