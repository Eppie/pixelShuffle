#include <png.h>
#include <stdlib.h>
#include <time.h>
#include <iostream>
#include <cmath>
#include <string>
#include <sstream>
#include <iomanip>

using namespace std;

int sWidth;
int sHeight;
int dWidth;
int dHeight;
int diffR;
int diffG;
int diffB;

png_bytep *rowPointersSrc = (png_bytep*) malloc( sizeof( png_bytep ) * 1000 );
png_bytep *rowPointersDst = (png_bytep*) malloc( sizeof( png_bytep ) * 1000 );

png_bytep **srcPtr = &rowPointersSrc;
png_bytep **dstPtr = &rowPointersDst;

void readPNGFile( char *filename, png_bytep *rowPointers, int* width, int* height ) {
	png_byte bit_depth;
	png_byte color_type;
	FILE *fp = fopen( filename, "rb" );

	png_structp png = png_create_read_struct( PNG_LIBPNG_VER_STRING, NULL, NULL, NULL );
	if( !png ) {
		abort();
	}

	png_infop info = png_create_info_struct( png );
	if( !info ) {
		abort();
	}

	if( setjmp( png_jmpbuf( png ) ) ) {
		abort();
	}

	png_init_io( png, fp );

	png_read_info( png, info );

	*width      = png_get_image_width( png, info );
	*height     = png_get_image_height( png, info );
	color_type = png_get_color_type( png, info );
	bit_depth  = png_get_bit_depth( png, info );

	// Read any color_type into 8bit depth, RGBA format.
	// See http://www.libpng.org/pub/png/libpng-manual.txt

	if( bit_depth == 16 ) {
		png_set_strip_16( png );
	}

	if( color_type == PNG_COLOR_TYPE_PALETTE ) {
		png_set_palette_to_rgb( png );
	}

	// PNG_COLOR_TYPE_GRAY_ALPHA is always 8 or 16bit depth.
	if( color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8 ) {
		png_set_expand_gray_1_2_4_to_8( png );
	}

	if( png_get_valid( png, info, PNG_INFO_tRNS ) ) {
		png_set_tRNS_to_alpha( png );
	}

	// These color_type don't have an alpha channel then fill it with 0xff.
	if( color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE ) {
		png_set_filler( png, 0xFF, PNG_FILLER_AFTER );
	}

	if( color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA ) {
		png_set_gray_to_rgb( png );
	}

	png_read_update_info( png, info );

	rowPointers = (png_bytep*) realloc( rowPointers, sizeof( png_bytep ) * *height );
	for( int y = 0; y < *height; y++ ) {
		rowPointers[y] = (png_byte*) malloc( png_get_rowbytes( png, info ) );
	}

	png_read_image( png, rowPointers );

	fclose( fp );
}

void writePNGFile( const char *filename, png_bytep *rowPointers, bool done = false ) {
	FILE *fp = fopen(filename, "wb");
	if( !fp ) {
		abort();
	}

	png_structp png = png_create_write_struct( PNG_LIBPNG_VER_STRING, NULL, NULL, NULL );
	if( !png ) {
		abort();
	}

	png_infop info = png_create_info_struct(png);
	if( !info ) {
		abort();
	}

	if( setjmp( png_jmpbuf( png ) ) ) {
		abort();
	}

	png_init_io( png, fp );

	// Output is 8bit depth, RGBA format.
	png_set_IHDR(
		png,
		info,
		sWidth, sHeight,
		8,
		PNG_COLOR_TYPE_RGBA,
		PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT,
		PNG_FILTER_TYPE_DEFAULT
	);

	png_write_info( png, info );

	png_write_image( png, rowPointers );
	png_write_end( png, NULL );

	if( done ) {
		for( int y = 0; y < sHeight; y++ ) {
			free( rowPointers[y] );
		}
		free( rowPointers );
	}

	fclose(fp);
}

int pixelDiff( png_bytep px1, png_bytep px2 ) {
	diffR = px1[0] - px2[0];
	diffG = px1[1] - px2[1];
	diffB = px1[2] - px2[2];
	return (diffR * diffR) + (diffG * diffG) + (diffB * diffB);
}

void swapPixels( png_bytep px1, png_bytep px2 ) {
	swap( px1[0], px2[0] );
	swap( px1[1], px2[1] );
	swap( px1[2], px2[2] );
}

bool determineSwap( png_bytep sPx1, png_bytep sPx2, png_bytep dPx1, png_bytep dPx2 ) {
	return pixelDiff( sPx1, dPx2 ) + pixelDiff( sPx2, dPx1 ) < pixelDiff( sPx1, dPx1 ) + pixelDiff( sPx2, dPx2 );
}

unsigned long long totalDiff( png_bytep *src, png_bytep *dst ) {
	unsigned long long totalDiff = 0;
	png_bytep rowSrc;
	png_bytep rowDst;
	png_bytep sPx;
	png_bytep dPx;

	for( int y = 0; y < sHeight; y++ ) {
		rowSrc = src[y];
		rowDst = dst[y];
		for( int x = 0; x < sWidth; x++ ) {
			sPx = &(rowSrc[x * 4]);
			dPx = &(rowDst[x * 4]);
			totalDiff += pixelDiff( sPx, dPx );
		}
	}
	return totalDiff;
}

void processPNGFile( png_bytep *src, png_bytep *dst ) {
	int x1;
	int x2;
	int y1;
	int y2;
	
	int k = 0;
	int srcX;
	int srcY;
	int dstX;
	int dstY;

	png_bytep sy1;
	png_bytep sy2;
	png_bytep dy1;
	png_bytep dy2;
	png_bytep sPx1;
	png_bytep sPx2;
	png_bytep dPx1;
	png_bytep dPx2;

	unsigned long long t;
	int orderedLoopCount = 200;
	int randomLoopCount = 50;

	srand( time( NULL ) );
	for( int l = 0; l < 2; l++ ) {
		for( int j = 0; j < orderedLoopCount; j++ ) {
			for( int i = 0; i < 3e5; i++ ) {
				k = i + (j*i);

				sy1 = src[(k/sHeight)%sHeight];
				sy2 = src[k%sHeight];
				dy1 = dst[(k/sHeight)%sHeight];
				dy2 = dst[k%sHeight];

				sPx1 = &(sy1[((k/sWidth)%sWidth)*4]);
				sPx2 = &(sy2[(k%sWidth)*4]);
				dPx1 = &(dy1[((k/sWidth)%sWidth)*4]);
				dPx2 = &(dy2[(k%sWidth)*4]);
				if( determineSwap( sPx1, sPx2, dPx1, dPx2 ) ) {
					swapPixels( sPx1, sPx2 );
				}
			}
			t = totalDiff( src, dst );
			cout << "Iteration #" << j + ( ( orderedLoopCount + randomLoopCount ) * l ) << ", Diff: " << t << " (ordered)" << endl;
			ostringstream ss;
			ss << setw(5) << setfill('0') << j + ( ( orderedLoopCount + randomLoopCount ) * l );
			string s2(ss.str());
			string newFilename = "out" + s2 + ".png";
			writePNGFile( newFilename.c_str(), *srcPtr );
		}

		for( int j = 0; j < randomLoopCount; j++ ) {
			for( int i = 0; i < 1e6; i++ ) {
				y1 = (rand() % (int)(sHeight));
				y2 = (rand() % (int)(sHeight));
				x1 = (rand() % (int)(sWidth));
				x2 = (rand() % (int)(sWidth));

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
			ss << setw(5) << setfill('0') << j + ( ( orderedLoopCount + randomLoopCount ) * l ) + orderedLoopCount;
			string s2(ss.str());
			string newFilename = "out" + s2 + ".png";
			writePNGFile( newFilename.c_str(), *srcPtr );
		}
	}
}

int main( int argc, char *argv[] ) {

	readPNGFile( argv[1], *srcPtr, &sWidth, &sHeight );
	readPNGFile( argv[2], *dstPtr, &dWidth, &dHeight );
	processPNGFile( *srcPtr, *dstPtr );
	writePNGFile( argv[3], *srcPtr, true );

	return 0;
}
