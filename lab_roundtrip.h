#pragma once
// Shared Gen-1 (round-trip swap family) Lab<->RGB color core, extracted verbatim
// from pngLAB_clean/pngLAB_pretty where it was byte-identical. The traversal,
// seeding, image<->buffer conversion, and swap strategy stay per-variant.
#include <cstdint>

#include <png.h>

#include "fmath.hpp"

// Internal LAB/XYZ/RGB working pixel. The padded 16-byte size is required for aligned NEON loads.
struct alignas( 16 ) LabColor {
	float l;
	float a;
	float b;
	float pad;
};
static_assert( sizeof( LabColor ) == 16, "LabColor must stay 16-byte aligned for the NEON path" );
inline LabColor xyzToRgb( const LabColor& px ) {
	static PowGenerator gamma( 1.0 / 2.4 );

	const float x = px.l / 100.0f;
	const float y = px.a / 100.0f;
	const float z = px.b / 100.0f;

	float r = x * 3.2406f + y * -1.5372f + z * -0.4986f;
	float g = x * -0.9689f + y * 1.8758f + z * 0.0415f;
	float b = x * 0.0557f + y * -0.2040f + z * 1.0570f;

	r = r > 0.0031308f ? 1.055f * gamma.get( r ) - 0.055f : r * 12.92f;
	g = g > 0.0031308f ? 1.055f * gamma.get( g ) - 0.055f : g * 12.92f;
	b = b > 0.0031308f ? 1.055f * gamma.get( b ) - 0.055f : b * 12.92f;

	return { r * 255.0f, g * 255.0f, b * 255.0f, 0.0f };
}
inline LabColor rgbToXyz( png_bytep px ) {
	static PowGenerator gamma( 2.4f );

	float r = px[0] / 255.0f;
	float g = px[1] / 255.0f;
	float b = px[2] / 255.0f;

	r = r > 0.04045f ? gamma.get( ( r + 0.055f ) / 1.055f ) : r / 12.92f;
	g = g > 0.04045f ? gamma.get( ( g + 0.055f ) / 1.055f ) : g / 12.92f;
	b = b > 0.04045f ? gamma.get( ( b + 0.055f ) / 1.055f ) : b / 12.92f;

	r *= 100.0f;
	g *= 100.0f;
	b *= 100.0f;

	return {
		( r * 0.4124f ) + ( g * 0.3576f ) + ( b * 0.1805f ),
		( r * 0.2126f ) + ( g * 0.7152f ) + ( b * 0.0722f ),
		( r * 0.0193f ) + ( g * 0.1192f ) + ( b * 0.9505f ),
		0.0f
	};
}
inline LabColor xyzToLab( const LabColor& px ) {
	static PowGenerator cubicRoot( 1.0 / 3.0 );

	float x = px.l / 95.047f;
	float y = px.a / 100.0f;
	float z = px.b / 108.883f;

	x = x > 0.008856f ? cubicRoot.get( x ) : ( x * 7.787f ) + ( 16.0f / 116.0f );
	y = y > 0.008856f ? cubicRoot.get( y ) : ( y * 7.787f ) + ( 16.0f / 116.0f );
	z = z > 0.008856f ? cubicRoot.get( z ) : ( z * 7.787f ) + ( 16.0f / 116.0f );

	return {
		( y * 116.0f ) - 16.0f,
		( x - y ) * 500.0f,
		( y - z ) * 200.0f,
		0.0f
	};
}
inline LabColor labToXyz( const LabColor& px ) {

	float y = ( px.l + 16.0f ) / 116.0f;
	float x = px.a / 500.0f + y;
	float z = y - px.b / 200.0f;

	x = x * x * x > 0.008856f ? x * x * x : ( x - 16.0f / 116.0f ) / 7.787f;
	y = y * y * y > 0.008856f ? y * y * y : ( y - 16.0f / 116.0f ) / 7.787f;
	z = z * z * z > 0.008856f ? z * z * z : ( z - 16.0f / 116.0f ) / 7.787f;

	return { x * 95.047f, y * 100.0f, z * 108.883f, 0.0f };
}
inline LabColor rgbaToLab( png_bytep px ) {
	return xyzToLab( rgbToXyz( px ) );
}
inline __attribute__((always_inline)) void advanceOrderedState( const uint32_t* nextTable, const uint32_t* carryTable, const uint32_t limit, uint32_t& major, uint32_t& minor ) {
	const uint32_t oldMinor = minor;
	minor = nextTable[oldMinor];
	major += carryTable[oldMinor];
	while( major >= limit ) {
		major -= limit;
	}
}
