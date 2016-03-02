#include <iostream>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <locale>
#include <string.h> // for memcpy
#include "pngReadWrite.h"
#include "fmath.hpp"

using namespace std;

float diffL;
float diffa;
float diffb;
string filePrefix;

struct Color {
	float A;
	float B;
	float C;
};

png_bytep* rowPointersSrc = ( png_bytep* ) malloc( sizeof( png_bytep ) * 5000 );
png_bytep* rowPointersNew = ( png_bytep* ) malloc( sizeof( png_bytep ) * 5000 );
png_bytep* rowPointersDst = ( png_bytep* ) malloc( sizeof( png_bytep ) * 5000 );

png_bytep** srcPtr = &rowPointersSrc;
png_bytep** newPtr = &rowPointersNew;
png_bytep** dstPtr = &rowPointersDst;

uint64_t x = 0x8E588AFE51D8B00D;

inline uint64_t xorshift64star() {
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	return x * 2685821657736338717ULL;
}

inline Color XYZToRGB( Color px ) {
	static fmath::PowGenerator f( 1.0 / 2.4 );

	float X = px.A / 100.0;
	float Y = px.B / 100.0;
	float Z = px.C / 100.0;

	float R = X *  3.2406 + Y * -1.5372 + Z * -0.4986;
	float G = X * -0.9689 + Y *  1.8758 + Z *  0.0415;
	float B = X *  0.0557 + Y * -0.2040 + Z *  1.0570;

	R = R > 0.0031308 ? 1.055 * f.get( R ) - 0.055 : R * 12.92;
	G = G > 0.0031308 ? 1.055 * f.get( G ) - 0.055 : G * 12.92;
	B = B > 0.0031308 ? 1.055 * f.get( B ) - 0.055 : B * 12.92;
	
	R = R * 255.0;
	G = G * 255.0;
	B = B * 255.0;

	Color result = { R, G, B };
	return result;
}

inline Color RGBToXYZ( png_bytep px ) {
	static fmath::PowGenerator f( 2.4 );
	float R = px[0] / 255.0;
	float G = px[1] / 255.0;
	float B = px[2] / 255.0;

	R = R > 0.04045 ? f.get( ( ( R + 0.055 ) / 1.055 ) ) : R / 12.92;
	G = G > 0.04045 ? f.get( ( ( G + 0.055 ) / 1.055 ) ) : G / 12.92;
	B = B > 0.04045 ? f.get( ( ( B + 0.055 ) / 1.055 ) ) : B / 12.92;

	R *= 100.0;
	G *= 100.0;
	B *= 100.0;

	float X = ( R * 0.4124 ) + ( G * 0.3576 ) + ( B * 0.1805 );
	float Y = ( R * 0.2126 ) + ( G * 0.7152 ) + ( B * 0.0722 );
	float Z = ( R * 0.0193 ) + ( G * 0.1192 ) + ( B * 0.9505 );

	Color result = { X, Y, Z };
	return result;
}

inline Color XYZToLab( Color px ) {
	static fmath::PowGenerator f( 1.0 / 3.0 );
	float X = px.A / 95.047;
	float Y = px.B / 100.0;
	float Z = px.C / 108.883;

	X = X > 0.008856 ? f.get( X ) : ( X * 7.787 ) + ( 16.0 / 116.0 );
	Y = Y > 0.008856 ? f.get( Y ) : ( Y * 7.787 ) + ( 16.0 / 116.0 );
	Z = Z > 0.008856 ? f.get( Z ) : ( Z * 7.787 ) + ( 16.0 / 116.0 );

	float L = ( Y * 116.0 ) - 16.0;
	float a = ( X - Y ) * 500.0;
	float b = ( Y - Z ) * 200.0;

	Color result = { L, a, b };
	return result;
}

inline Color LabToXYZ( Color px ) {
	float Y = ( px.A + 16.0 ) / 116.0;
	float X = px.B / 500.0 + Y;
	float Z = Y - px.C / 200.0;

	X = X * X * X > 0.008856 ? X * X * X : ( X - 16.0 / 116.0 ) / 7.787;
	Y = Y * Y * Y > 0.008856 ? Y * Y * Y : ( Y - 16.0 / 116.0 ) / 7.787;
	Z = Z * Z * Z > 0.008856 ? Z * Z * Z : ( Z - 16.0 / 116.0 ) / 7.787;

	X *= 95.047;
	Y *= 100.0;
	Z *= 108.883;

	Color result = { X, Y, Z };
	return result;
}

inline Color RGBToLab( png_bytep px ) {
	return XYZToLab( RGBToXYZ( px ) );
}

inline float pixelDiff( Color* px1, Color* px2 ) {
	diffL = px1->A - px2->A;
	diffa = px1->B - px2->B;
	diffb = px1->C - px2->C;
	return ( diffL * diffL ) + ( diffa * diffa ) + ( diffb * diffb );
}

inline void swapPixels( Color* px1, Color* px2 ) {
	swap( px1->A, px2->A );
	swap( px1->B, px2->B );
	swap( px1->C, px2->C );
}

inline Color** imageToLab( png_bytep* image ) {
	Color lab;
	png_bytep oldRow;
	Color* newRow;
	png_bytep px;
	Color** result = ( Color** ) malloc( sizeof( Color* ) * dHeight );

	for( int y = 0; y < dHeight; y++ ) {
		result[y] = ( Color* ) malloc( sizeof( Color ) * dWidth );
		newRow = result[y];
		oldRow = image[y];

		for( int x = 0; x < dWidth; x++ ) {
			px = &oldRow[x * 4];
			lab = RGBToLab( px );
			newRow[x] = lab;
		}
	}

	return result;
}

inline void labToImage( Color** lab, png_bytep* image ) {
	png_bytep newRow;
	Color* oldRow;
	Color RGB;

	for( int y = 0; y < dHeight; y++ ) {
		oldRow = lab[y];
		newRow = image[y];

		for( int x = 0; x < dWidth; x++ ) {
			RGB = XYZToRGB( LabToXYZ( oldRow[x] ) );

			// int() simply truncates. Thus:
			// int( 1.999 ) == 1
			// int( 1.999 + .5 ) == 2
			// int( 2.001 ) == 2
			// int( 2.001 + 0.5 ) == 2
			newRow[( x * 4 ) + 0] = int( RGB.A + 0.5 );
			newRow[( x * 4 ) + 1] = int( RGB.B + 0.5 );
			newRow[( x * 4 ) + 2] = int( RGB.C + 0.5 );
		}
	}
}

float totalDiff( Color** src, Color** dst ) {
	float totalDiff = 0;
	Color* rowSrc;
	Color* rowDst;
	Color* sPx;
	Color* dPx;
	int length = dHeight * dWidth;

	for( int i = 0; i < length; i++ ) {
		rowSrc = src[( i / dHeight ) % dHeight];
		rowDst = dst[( i / dHeight ) % dHeight];
		sPx = &( rowSrc[( i % dWidth )] );
		dPx = &( rowDst[( i % dWidth )] );
		totalDiff += pixelDiff( sPx, dPx );
	}

	return totalDiff;
}

void processPNGFile( Color** src, Color** dst ) {
	unsigned long long k;

	int x1, x2, y1, y2;

	Color* sy1, *sy2, *dy1, *dy2;
	Color* sPx1, *sPx2, *dPx1, *dPx2;

	float t;
	int orderedLoopCount = 150;
	int randomLoopCount = 100;
	int innerOrderedLoopCount = 3e5 * ( dWidth / 320 ) * ( dWidth / 320 );

	for( int j = 0; j < orderedLoopCount; j++ ) {
		int numSwaps = 0;
		for( int i = 0; i < innerOrderedLoopCount; i++ ) {
			k = i + ( j * i );

			sy1 = src[( k / dHeight ) % dHeight];
			sy2 = src[k % dHeight];
			dy1 = dst[( k / dHeight ) % dHeight];
			dy2 = dst[k % dHeight];

			sPx1 = &( sy1[( ( k / dWidth ) % dWidth )] );
			sPx2 = &( sy2[( k % dWidth )] );
			dPx1 = &( dy1[( ( k / dWidth ) % dWidth )] );
			dPx2 = &( dy2[( k % dWidth )] );

			if( pixelDiff( sPx1, dPx2 ) + pixelDiff( sPx2, dPx1 ) < pixelDiff( sPx1, dPx1 ) + pixelDiff( sPx2, dPx2 ) ) {
				swapPixels( sPx1, sPx2 );
				numSwaps++;
			}
		}
#ifdef OUTPUT
		t = totalDiff( src, dst );
		stringstream out;
		out.imbue( locale( "" ) );
		out << "Iteration #" << j
			<< ", Diff: " << fixed << t
			<< ", Swaps: " << numSwaps << " ( " << ( numSwaps / static_cast<float>( innerOrderedLoopCount ) * 100.0 ) << "% )"
			<< " (ordered)" << endl;
		cout << out.str();
#endif // OUTPUT
#ifdef ANIMATION
		ostringstream ss;
		ss << setw( 5 ) << setfill( '0' ) << 24 + j;
		string s2( ss.str() );
		string newFilename = filePrefix + s2 + ".png";
		labToImage( src, *newPtr );
		writePNGFile( newFilename.c_str(), *newPtr );
#endif // ANIMATION
	}

	for( int j = 0; j < randomLoopCount; j++ ) {
		int numSwaps = 0;
		for( int i = 0; i < j * 1e5; i++ ) {
			uint64_t r = xorshift64star();
			int r1 = r & 0xFFFF;
			int r2 = ( r & ( 0xFFFFULL << 16 ) ) >> 16;
			int r3 = ( r & ( 0xFFFFULL << 32 ) ) >> 32;
			int r4 = ( r & ( 0xFFFFULL << 48 ) ) >> 48;

			y1 = r1 % dHeight;
			y2 = r2 % dHeight;
			x1 = r3 % dWidth;
			x2 = r4 % dWidth;

			sy1 = src[y1];
			sy2 = src[y2];
			dy1 = dst[y1];
			dy2 = dst[y2];

			sPx1 = &( sy1[x1] );
			sPx2 = &( sy2[x2] );
			dPx1 = &( dy1[x1] );
			dPx2 = &( dy2[x2] );

			if( pixelDiff( sPx1, dPx2 ) + pixelDiff( sPx2, dPx1 ) < pixelDiff( sPx1, dPx1 ) + pixelDiff( sPx2, dPx2 ) ) {
				swapPixels( sPx1, sPx2 );
				numSwaps++;
			}
		}
#ifdef OUTPUT
		t = totalDiff( src, dst );
		stringstream out;
		out.imbue( locale( "" ) );
		out << "Iteration #" << j + orderedLoopCount
			<< ", Diff: " << fixed << t
			<< ", Swaps: " << numSwaps << " ( " << ( numSwaps / static_cast<float>( j * 1e5 ) * 100.0 ) << "% )"
			<< " (random)" << endl;
		cout << out.str();
#endif // OUTPUT
#ifdef ANIMATION
		ostringstream ss;
		ss << setw( 5 ) << setfill( '0' ) << 24 + j + orderedLoopCount;
		string s2( ss.str() );
		string newFilename = filePrefix + s2 + ".png";
		labToImage( src, *newPtr );
		writePNGFile( newFilename.c_str(), *newPtr );
#endif // ANIMATION
	}
}

string split( string &s ) {
	stringstream ss( s );
	string result;
	getline( ss, result, '/' );
	getline( ss, result, '.' );
	return result;
}

int main( int argc, char* argv[] ) {
	if( argc != 4 ) {
		cout << "Usage: " << argv[0] << " <palette image> <source image> <output image>"  << endl;
		exit( 1 );
	}

	readPNGFile( argv[1], *srcPtr, &sWidth, &sHeight ); // "palette"
	readPNGFile( argv[2], *dstPtr, &dWidth, &dHeight ); // "source"

	// Output shape should be that of the dst image.
	rowPointersNew = ( png_bytep* ) realloc( rowPointersNew, sizeof( png_bytep ) * dHeight );

	for( int y = 0; y < dHeight; y++ ) {
		rowPointersNew[y] = ( png_bytep ) malloc( sizeof( png_bytep ) * dWidth * 4 );
	}

	for( int i = 0; i < dHeight * dWidth; i++ ) {
		memcpy( &rowPointersNew[i / dWidth][( ( i % dWidth ) * 4 )], &rowPointersSrc[i / sWidth][( ( i % sWidth ) * 4 )], 4 );
	}

	for( int y = 0; y < sHeight; y++ ) {
		free( rowPointersSrc[y] );
	}

	free( rowPointersSrc );

	Color** srcLab = imageToLab( *newPtr );
	Color** dstLab = imageToLab( *dstPtr );
#ifdef ANIMATION
	ostringstream ss;
	ss << argv[3];
	filePrefix = ss.str();
	filePrefix = "out" + split( filePrefix );
	writePNGFile( string( filePrefix + "00000.png" ).c_str(), *newPtr );
#endif // ANIMATION
	processPNGFile( srcLab, dstLab );

	labToImage( srcLab, *newPtr );
	writePNGFile( argv[3], *newPtr, true );

	return 0;
}
