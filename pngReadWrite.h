#pragma once
#include <cstdio>
#include <cstdlib>

#include <png.h>

#include "profile_stats.h"
#include "pmu_profile.h"

// `inline` so these single header-defined definitions are ODR-safe if the
// header is ever included into more than one translation unit that links
// together (today each binary is a single TU, but the shared-core refactor
// relies on this holding).
inline int sWidth;
inline int sHeight;
inline int dWidth;
inline int dHeight;

inline void readPNGFile( const char* filename, png_bytep** rowPointers, int* width, int* height ) { // Changed to png_bytep**
	PNGLAB_PMU_SCOPE( "read_png" );
#ifdef PROFILE_STATS
	const uint64_t startNs = ProfileStats::nowNs();
#endif
	png_byte bit_depth;
	png_byte color_type;
	FILE* fp = fopen( filename, "rb" );

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

	*width = png_get_image_width( png, info );
	*height = png_get_image_height( png, info );
	color_type = png_get_color_type( png, info );
	bit_depth = png_get_bit_depth( png, info );

	if( bit_depth == 16 ) {
		png_set_strip_16( png );
	}

	if( color_type == PNG_COLOR_TYPE_PALETTE ) {
		png_set_palette_to_rgb( png );
	}

	if( color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8 ) {
		png_set_expand_gray_1_2_4_to_8( png );
	}

	if( png_get_valid( png, info, PNG_INFO_tRNS ) ) {
		png_set_tRNS_to_alpha( png );
	}

	if( color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE ) {
		png_set_filler( png, 0xFF, PNG_FILLER_AFTER );
	}

	if( color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA ) {
		png_set_gray_to_rgb( png );
	}

	png_read_update_info( png, info );

    // Allocate memory for the array of row pointers
	*rowPointers = ( png_bytep* ) malloc( sizeof( png_bytep ) * *height );

	for( int y = 0; y < *height; y++ ) {
        // Allocate memory for each row
		( *rowPointers )[y] = ( png_byte* ) malloc( png_get_rowbytes( png, info ) );
	}

    // Read the image using the dereferenced rowPointers
	png_read_image( png, *rowPointers );

	png_destroy_read_struct( &png, &info, NULL );
	fclose( fp );
#ifdef PROFILE_STATS
	ProfileStats::add(
		ProfileStats::EventId::ReadPNG,
		ProfileStats::nowNs() - startNs,
		1,
		static_cast<uint64_t>( *width ) * static_cast<uint64_t>( *height )
	);
#endif
}

inline void writePNGFile( const char* filename, png_bytep* rowPointers, int width, int height, bool hasAlpha = true, bool done = false ) {
	PNGLAB_PMU_SCOPE( "write_png" );
#ifdef PROFILE_STATS
	const uint64_t startNs = ProfileStats::nowNs();
#endif
	FILE* fp = fopen( filename, "wb" );

	if( !fp ) {
		abort();
	}

	png_structp png = png_create_write_struct( PNG_LIBPNG_VER_STRING, NULL, NULL, NULL );

	if( !png ) {
		abort();
	}

#ifdef ANIMATION
	// Compression is ~1/3 of the total running time of the program. If we are writing a bunch of files,
	// disable compression at the expense of ~50% greater file sizes. If we are only writing the result file,
	// compress as per normal.
	png_set_compression_level( png, 0 );
#endif // ANIMATION

	png_infop info = png_create_info_struct( png );

	if( !info ) {
		abort();
	}

	if( setjmp( png_jmpbuf( png ) ) ) {
		abort();
	}

	png_init_io( png, fp );

	png_set_IHDR( png, info, width, height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT );

	png_write_info( png, info );

	png_write_image( png, rowPointers );
	png_write_end( png, NULL );

	if( done ) {
		for( int y = 0; y < dHeight; y++ ) {
			free( rowPointers[y] );
		}

		free( rowPointers );
	}

	fclose( fp );
	// png_free_data( png, info, PNG_FREE_ALL, -1 ); // This is problematic after png is destroyed or if info is handled by destroy
	png_destroy_write_struct( &png, &info ); // Pass &info to free it along with png struct
#ifdef PROFILE_STATS
	ProfileStats::add(
		ProfileStats::EventId::WritePNG,
		ProfileStats::nowNs() - startNs,
		1,
		static_cast<uint64_t>( width ) * static_cast<uint64_t>( height )
	);
#endif
}
