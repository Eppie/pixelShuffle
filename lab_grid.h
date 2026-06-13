#pragma once
// Shared Gen-2 (assignment/transport family) scaffolding, extracted verbatim from the
// pngLAB_* variants where it was byte-identical across all seven. Per-variant code
// (OwnedRows, XorShift64Star + its seed, totalCost, the optimization strategy itself)
// stays in each .cpp because it genuinely differs between variants.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <png.h>

struct Geometry {
	int width = 0;
	int height = 0;

	size_t pixelCount() const {
		return static_cast<size_t>( width ) * static_cast<size_t>( height );
	}

	size_t strideBytes() const {
		return static_cast<size_t>( width ) * 4u;
	}
};

struct LabColor {
	float l = 0.0f;
	float a = 0.0f;
	float b = 0.0f;
};

struct Pixel {
	LabColor lab;
	std::array<png_byte, 4> rgba;
	uint64_t key = 0;
};

inline float srgbToLinear( const float value ) {
	return value > 0.04045f ? std::pow( ( value + 0.055f ) / 1.055f, 2.4f ) : value / 12.92f;
}

inline LabColor rgbaToLab( const png_bytep px ) {
	float r = srgbToLinear( px[0] / 255.0f ) * 100.0f;
	float g = srgbToLinear( px[1] / 255.0f ) * 100.0f;
	float b = srgbToLinear( px[2] / 255.0f ) * 100.0f;

	float x = ( r * 0.4124f ) + ( g * 0.3576f ) + ( b * 0.1805f );
	float y = ( r * 0.2126f ) + ( g * 0.7152f ) + ( b * 0.0722f );
	float z = ( r * 0.0193f ) + ( g * 0.1192f ) + ( b * 0.9505f );

	x /= 95.047f;
	y /= 100.0f;
	z /= 108.883f;

	x = x > 0.008856f ? std::cbrt( x ) : ( x * 7.787f ) + ( 16.0f / 116.0f );
	y = y > 0.008856f ? std::cbrt( y ) : ( y * 7.787f ) + ( 16.0f / 116.0f );
	z = z > 0.008856f ? std::cbrt( z ) : ( z * 7.787f ) + ( 16.0f / 116.0f );

	return {
		( y * 116.0f ) - 16.0f,
		( x - y ) * 500.0f,
		( y - z ) * 200.0f
	};
}

inline std::vector<LabColor> imageToLab( const Geometry& geometry, png_bytep* rows ) {
	std::vector<LabColor> result( geometry.pixelCount() );
	for( int y = 0; y < geometry.height; ++y ) {
		png_bytep row = rows[y];
		const size_t rowOffset = static_cast<size_t>( y ) * static_cast<size_t>( geometry.width );
		for( int x = 0; x < geometry.width; ++x ) {
			result[rowOffset + static_cast<size_t>( x )] = rgbaToLab( &row[x * 4] );
		}
	}
	return result;
}

inline float squaredLabDistance( const LabColor& lhs, const LabColor& rhs ) {
	const float dl = lhs.l - rhs.l;
	const float da = lhs.a - rhs.a;
	const float db = lhs.b - rhs.b;
	return ( dl * dl ) + ( da * da ) + ( db * db );
}

inline uint32_t quantize( const float value, const float low, const float high ) {
	const float normalized = std::clamp( ( value - low ) / ( high - low ), 0.0f, 1.0f );
	return static_cast<uint32_t>( normalized * 1023.0f + 0.5f );
}

inline uint64_t colorKey( const LabColor& color ) {
	const uint32_t l = quantize( color.l, 0.0f, 100.0f );
	const uint32_t a = quantize( color.a, -128.0f, 128.0f );
	const uint32_t b = quantize( color.b, -128.0f, 128.0f );

	uint64_t key = 0;
	for( int bit = 9; bit >= 0; --bit ) {
		key <<= 3u;
		key |= ( ( l >> bit ) & 1u ) << 2u;
		key |= ( ( a >> bit ) & 1u ) << 1u;
		key |= ( b >> bit ) & 1u;
	}
	return key;
}

inline bool labLess( const LabColor& lhs, const LabColor& rhs ) {
	if( lhs.l != rhs.l ) {
		return lhs.l < rhs.l;
	}
	if( lhs.a != rhs.a ) {
		return lhs.a < rhs.a;
	}
	return lhs.b < rhs.b;
}

