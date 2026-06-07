#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <vector>

#if defined(PNG_LAB_ENABLE_NEON) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "pngReadWrite.h"
#include "fmath.hpp"
#include "profiler.h"
#include "profile_stats.h"
#include "pmu_profile.h"

using namespace std;

namespace {

constexpr int kOrderedLoopCount = 150;
constexpr int kRandomLoopCount = 100;
constexpr uint64_t kNeonWorkingSetLimitBytes = 8ULL * 1024ULL * 1024ULL;

#if defined(PNG_LAB_ENABLE_NEON) && defined(__ARM_NEON)
constexpr bool kUseNeonSwapKernel = true;
#else
constexpr bool kUseNeonSwapKernel = false;
#endif

#define HOT_INLINE inline __attribute__((always_inline))

#ifdef PROFILE_STATS
#define PROFILE_SAMPLED_BEGIN(counter_name)                                          \
	static uint64_t counter_name = 0;                                               \
	++counter_name;                                                                 \
	const bool sampled = ( counter_name & 0xFFFu ) == 0;                            \
	const uint64_t startNs = sampled ? ProfileStats::nowNs() : 0
#define PROFILE_SAMPLED_END(event_id)                                                 \
	if( sampled ) {                                                                  \
		ProfileStats::add( event_id, ProfileStats::nowNs() - startNs, 1, 1 );      \
	}
#else
#define PROFILE_SAMPLED_BEGIN(counter_name)
#define PROFILE_SAMPLED_END(event_id)
#endif

struct alignas( 16 ) Color {
	float A;
	float B;
	float C;
	float D;
};

static_assert( sizeof( Color ) == 16, "Color must stay vector-sized" );

string filePrefix;
constexpr uint64_t kInitialRandomSeed = 0x8E588AFE51D8B00DULL;
uint64_t x = kInitialRandomSeed;
bool useNeonSwapKernel = false;

template <typename T>
T* allocateAlignedArray( size_t count ) {
	void* ptr = nullptr;
	if( posix_memalign( &ptr, alignof( T ), sizeof( T ) * count ) != 0 ) {
		abort();
	}
	return static_cast<T*>( ptr );
}

struct AdvanceTables {
	int dimension = 0;
	int stepCount = 0;
	vector<uint32_t> next;
	vector<uint32_t> carry;

	AdvanceTables() = default;

	AdvanceTables( int dim, int steps )
		: dimension( dim ),
		  stepCount( steps ),
		  next( static_cast<size_t>( dim ) * static_cast<size_t>( steps ) ),
		  carry( static_cast<size_t>( dim ) * static_cast<size_t>( steps ) ) {
		for( int stepIndex = 0; stepIndex < stepCount; ++stepIndex ) {
			const int step = stepIndex + 1;
			for( int state = 0; state < dimension; ++state ) {
				int value = state + step;
				uint32_t wraps = 0;
				while( value >= dimension ) {
					value -= dimension;
					++wraps;
				}

				const size_t index = static_cast<size_t>( stepIndex ) * static_cast<size_t>( dimension ) + static_cast<size_t>( state );
				next[index] = static_cast<uint32_t>( value );
				carry[index] = wraps;
			}
		}
	}

	HOT_INLINE const uint32_t* nextForStep( int stepIndex ) const {
		return next.data() + static_cast<size_t>( stepIndex ) * static_cast<size_t>( dimension );
	}

	HOT_INLINE const uint32_t* carryForStep( int stepIndex ) const {
		return carry.data() + static_cast<size_t>( stepIndex ) * static_cast<size_t>( dimension );
	}
};

struct KernelTables {
	vector<Color*> srcRows;
	vector<const Color*> dstRows;
	AdvanceTables orderedHeight;
	AdvanceTables orderedWidth;
	vector<uint32_t> modHeight;
	vector<uint32_t> modWidth;

	KernelTables( Color* srcPixels, const Color* dstPixels )
		: srcRows( static_cast<size_t>( dHeight ) ),
		  dstRows( static_cast<size_t>( dHeight ) ),
		  orderedHeight( dHeight, kOrderedLoopCount ),
		  orderedWidth( dWidth, kOrderedLoopCount ),
		  modHeight( 1u << 16 ),
		  modWidth( 1u << 16 ) {
		for( int y = 0; y < dHeight; ++y ) {
			srcRows[static_cast<size_t>( y )] = srcPixels + static_cast<size_t>( y ) * static_cast<size_t>( dWidth );
			dstRows[static_cast<size_t>( y )] = dstPixels + static_cast<size_t>( y ) * static_cast<size_t>( dWidth );
		}

		for( uint32_t value = 0; value < ( 1u << 16 ); ++value ) {
			modHeight[value] = dHeight == 0 ? 0 : value % static_cast<uint32_t>( dHeight );
			modWidth[value] = dWidth == 0 ? 0 : value % static_cast<uint32_t>( dWidth );
		}
	}
};

const locale& userLocale() {
	static const locale value( "" );
	return value;
}

HOT_INLINE uint64_t xorshift64star() {
	PROFILE_FUNCTION();
	PROFILE_SAMPLED_BEGIN( sampleCounter );
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	const uint64_t result = x * 2685821657736338717ULL;
	PROFILE_SAMPLED_END( ProfileStats::EventId::XorShiftSample );
	return result;
}

inline Color XYZToRGB( const Color& px ) {
	PROFILE_FUNCTION();
	static PowGenerator f( 1.0 / 2.4 );

	float X = px.A / 100.0;
	float Y = px.B / 100.0;
	float Z = px.C / 100.0;

	float R = X * 3.2406 + Y * -1.5372 + Z * -0.4986;
	float G = X * -0.9689 + Y * 1.8758 + Z * 0.0415;
	float B = X * 0.0557 + Y * -0.2040 + Z * 1.0570;

	R = R > 0.0031308 ? 1.055 * f.get( R ) - 0.055 : R * 12.92;
	G = G > 0.0031308 ? 1.055 * f.get( G ) - 0.055 : G * 12.92;
	B = B > 0.0031308 ? 1.055 * f.get( B ) - 0.055 : B * 12.92;

	R = R * 255.0;
	G = G * 255.0;
	B = B * 255.0;

	return { R, G, B, 0.0f };
}

inline Color RGBToXYZ( png_bytep px ) {
	PROFILE_FUNCTION();
	static PowGenerator f( 2.4 );
	float R = px[0] / 255.0;
	float G = px[1] / 255.0;
	float B = px[2] / 255.0;

	R = R > 0.04045 ? f.get( ( R + 0.055 ) / 1.055 ) : R / 12.92;
	G = G > 0.04045 ? f.get( ( G + 0.055 ) / 1.055 ) : G / 12.92;
	B = B > 0.04045 ? f.get( ( B + 0.055 ) / 1.055 ) : B / 12.92;

	R *= 100.0;
	G *= 100.0;
	B *= 100.0;

	const float X = ( R * 0.4124 ) + ( G * 0.3576 ) + ( B * 0.1805 );
	const float Y = ( R * 0.2126 ) + ( G * 0.7152 ) + ( B * 0.0722 );
	const float Z = ( R * 0.0193 ) + ( G * 0.1192 ) + ( B * 0.9505 );

	return { X, Y, Z, 0.0f };
}

inline Color XYZToLab( const Color& px ) {
	PROFILE_FUNCTION();
	static PowGenerator f( 1.0 / 3.0 );
	float X = px.A / 95.047;
	float Y = px.B / 100.0;
	float Z = px.C / 108.883;

	X = X > 0.008856 ? f.get( X ) : ( X * 7.787 ) + ( 16.0 / 116.0 );
	Y = Y > 0.008856 ? f.get( Y ) : ( Y * 7.787 ) + ( 16.0 / 116.0 );
	Z = Z > 0.008856 ? f.get( Z ) : ( Z * 7.787 ) + ( 16.0 / 116.0 );

	const float L = ( Y * 116.0 ) - 16.0;
	const float a = ( X - Y ) * 500.0;
	const float b = ( Y - Z ) * 200.0;

	return { L, a, b, 0.0f };
}

inline Color LabToXYZ( const Color& px ) {
	PROFILE_FUNCTION();
	float Y = ( px.A + 16.0 ) / 116.0;
	float X = px.B / 500.0 + Y;
	float Z = Y - px.C / 200.0;

	X = X * X * X > 0.008856 ? X * X * X : ( X - 16.0 / 116.0 ) / 7.787;
	Y = Y * Y * Y > 0.008856 ? Y * Y * Y : ( Y - 16.0 / 116.0 ) / 7.787;
	Z = Z * Z * Z > 0.008856 ? Z * Z * Z : ( Z - 16.0 / 116.0 ) / 7.787;

	X *= 95.047;
	Y *= 100.0;
	Z *= 108.883;

	return { X, Y, Z, 0.0f };
}

inline Color RGBToLab( png_bytep px ) {
	PROFILE_FUNCTION();
	return XYZToLab( RGBToXYZ( px ) );
}

HOT_INLINE float pixelDiffValue( const Color* px1, const Color* px2 ) {
	const float diffL = px1->A - px2->A;
	const float diffa = px1->B - px2->B;
	const float diffb = px1->C - px2->C;
	return ( diffL * diffL ) + ( diffa * diffa ) + ( diffb * diffb );
}

HOT_INLINE float pixelDiff( const Color* px1, const Color* px2 ) {
	PROFILE_FUNCTION();
	PROFILE_SAMPLED_BEGIN( sampleCounter );
	const float result = pixelDiffValue( px1, px2 );
	PROFILE_SAMPLED_END( ProfileStats::EventId::PixelDiffSample );
	return result;
}

#if defined(PNG_LAB_ENABLE_NEON) && defined(__ARM_NEON)
HOT_INLINE float pixelDiffNeonVectorReduce( const float32x4_t lhs, const float32x4_t rhs ) {
	const float32x4_t diff = vsubq_f32( lhs, rhs );
	const float32x4_t sq = vmulq_f32( diff, diff );
	return vgetq_lane_f32( sq, 0 ) + vgetq_lane_f32( sq, 1 ) + vgetq_lane_f32( sq, 2 );
}

HOT_INLINE float pixelDiffNeonScalarReduce( const float32x4_t lhs, const float32x4_t rhs ) {
	const float32x4_t diff = vsubq_f32( lhs, rhs );
	const float diffL = vgetq_lane_f32( diff, 0 );
	const float diffa = vgetq_lane_f32( diff, 1 );
	const float diffb = vgetq_lane_f32( diff, 2 );
	return ( diffL * diffL ) + ( diffa * diffa ) + ( diffb * diffb );
}
#endif

struct SwapBreakdown {
	float swap12;
	float swap21;
	float keep11;
	float keep22;
	float swapCost;
	float keepCost;

	HOT_INLINE bool decision() const {
		return swapCost < keepCost;
	}
};

HOT_INLINE SwapBreakdown scalarSwapBreakdown( const Color* sPx1, const Color* sPx2, const Color* dPx1, const Color* dPx2 ) {
	SwapBreakdown result;
	result.swap12 = pixelDiffValue( sPx1, dPx2 );
	result.swap21 = pixelDiffValue( sPx2, dPx1 );
	result.keep11 = pixelDiffValue( sPx1, dPx1 );
	result.keep22 = pixelDiffValue( sPx2, dPx2 );
	result.swapCost = result.swap12 + result.swap21;
	result.keepCost = result.keep11 + result.keep22;
	return result;
}

#if defined(PNG_LAB_ENABLE_NEON) && defined(__ARM_NEON)
HOT_INLINE SwapBreakdown neonVectorSwapBreakdown( const Color* sPx1, const Color* sPx2, const Color* dPx1, const Color* dPx2 ) {
	const float32x4_t src1 = vld1q_f32( &sPx1->A );
	const float32x4_t src2 = vld1q_f32( &sPx2->A );
	const float32x4_t dst1 = vld1q_f32( &dPx1->A );
	const float32x4_t dst2 = vld1q_f32( &dPx2->A );

	SwapBreakdown result;
	result.swap12 = pixelDiffNeonVectorReduce( src1, dst2 );
	result.swap21 = pixelDiffNeonVectorReduce( src2, dst1 );
	result.keep11 = pixelDiffNeonVectorReduce( src1, dst1 );
	result.keep22 = pixelDiffNeonVectorReduce( src2, dst2 );
	result.swapCost = result.swap12 + result.swap21;
	result.keepCost = result.keep11 + result.keep22;
	return result;
}

HOT_INLINE SwapBreakdown neonScalarReduceSwapBreakdown( const Color* sPx1, const Color* sPx2, const Color* dPx1, const Color* dPx2 ) {
	const float32x4_t src1 = vld1q_f32( &sPx1->A );
	const float32x4_t src2 = vld1q_f32( &sPx2->A );
	const float32x4_t dst1 = vld1q_f32( &dPx1->A );
	const float32x4_t dst2 = vld1q_f32( &dPx2->A );

	SwapBreakdown result;
	result.swap12 = pixelDiffNeonScalarReduce( src1, dst2 );
	result.swap21 = pixelDiffNeonScalarReduce( src2, dst1 );
	result.keep11 = pixelDiffNeonScalarReduce( src1, dst1 );
	result.keep22 = pixelDiffNeonScalarReduce( src2, dst2 );
	result.swapCost = result.swap12 + result.swap21;
	result.keepCost = result.keep11 + result.keep22;
	return result;
}
#endif

template <bool UseNeon>
HOT_INLINE bool shouldSwapImpl( const Color* sPx1, const Color* sPx2, const Color* dPx1, const Color* dPx2 ) {
	PROFILE_FUNCTION();
	PROFILE_SAMPLED_BEGIN( sampleCounter );
#if defined(PNG_LAB_ENABLE_NEON) && defined(__ARM_NEON)
	const SwapBreakdown breakdown = [] ( const Color* leftSrc, const Color* rightSrc, const Color* leftDst, const Color* rightDst ) {
		if constexpr( UseNeon ) {
			return neonScalarReduceSwapBreakdown( leftSrc, rightSrc, leftDst, rightDst );
		}
		return scalarSwapBreakdown( leftSrc, rightSrc, leftDst, rightDst );
	}( sPx1, sPx2, dPx1, dPx2 );
#else
	const SwapBreakdown breakdown = scalarSwapBreakdown( sPx1, sPx2, dPx1, dPx2 );
#endif

	PROFILE_SAMPLED_END( ProfileStats::EventId::SwapDecisionSample );
	return breakdown.decision();
}

HOT_INLINE void swapPixels( Color* px1, Color* px2 ) {
	PROFILE_FUNCTION();
	PROFILE_SAMPLED_BEGIN( sampleCounter );
#if defined(PNG_LAB_ENABLE_NEON) && defined(__ARM_NEON)
	const float32x4_t lhs = vld1q_f32( &px1->A );
	const float32x4_t rhs = vld1q_f32( &px2->A );
	vst1q_f32( &px1->A, rhs );
	vst1q_f32( &px2->A, lhs );
#else
	const Color tmp = *px1;
	*px1 = *px2;
	*px2 = tmp;
#endif
	PROFILE_SAMPLED_END( ProfileStats::EventId::SwapPixelsSample );
}

Color* imageToLab( png_bytep* image ) {
	PROFILE_FUNCTION();
	PNGLAB_PMU_SCOPE( "image_to_lab" );
#ifdef PROFILE_STATS
	ProfileStats::ScopedTimer timer( ProfileStats::EventId::ImageToLab, static_cast<uint64_t>( dWidth ) * static_cast<uint64_t>( dHeight ) );
#endif
	const size_t pixelCount = static_cast<size_t>( dWidth ) * static_cast<size_t>( dHeight );
	Color* result = allocateAlignedArray<Color>( pixelCount );

	for( int y = 0; y < dHeight; ++y ) {
		png_bytep oldRow = image[y];
		Color* newRow = result + static_cast<size_t>( y ) * static_cast<size_t>( dWidth );
		for( int x = 0; x < dWidth; ++x ) {
			newRow[x] = RGBToLab( &oldRow[x * 4] );
		}
	}

	return result;
}

void labToImage( const Color* lab, png_bytep* image ) {
	PROFILE_FUNCTION();
	PNGLAB_PMU_SCOPE( "lab_to_image" );
#ifdef PROFILE_STATS
	ProfileStats::ScopedTimer timer( ProfileStats::EventId::LabToImage, static_cast<uint64_t>( dWidth ) * static_cast<uint64_t>( dHeight ) );
#endif
	for( int y = 0; y < dHeight; ++y ) {
		png_bytep newRow = image[y];
		const Color* oldRow = lab + static_cast<size_t>( y ) * static_cast<size_t>( dWidth );
		for( int x = 0; x < dWidth; ++x ) {
			const Color rgb = XYZToRGB( LabToXYZ( oldRow[x] ) );
			newRow[( x * 4 ) + 0] = int( rgb.A + 0.5 );
			newRow[( x * 4 ) + 1] = int( rgb.B + 0.5 );
			newRow[( x * 4 ) + 2] = int( rgb.C + 0.5 );
		}
	}
}

float totalDiff( const Color* src, const Color* dst ) {
	PROFILE_FUNCTION();
	PNGLAB_PMU_SCOPE( "total_diff" );
	const int length = dHeight * dWidth;
#ifdef PROFILE_STATS
	ProfileStats::ScopedTimer timer( ProfileStats::EventId::TotalDiff, static_cast<uint64_t>( length ) );
#endif
	float diff = 0.0f;
	for( int i = 0; i < length; ++i ) {
		const size_t row = static_cast<size_t>( ( i / dHeight ) % dHeight );
		const size_t col = static_cast<size_t>( i % dWidth );
		diff += pixelDiff( src + row * static_cast<size_t>( dWidth ) + col, dst + row * static_cast<size_t>( dWidth ) + col );
	}
	return diff;
}

HOT_INLINE void advanceOrderedState( const uint32_t* nextTable, const uint32_t* carryTable, const uint32_t limit, uint32_t& major, uint32_t& minor ) {
	const uint32_t oldMinor = minor;
	minor = nextTable[oldMinor];
	major += carryTable[oldMinor];
	while( major >= limit ) {
		major -= limit;
	}
}

void printIterationStatus( int iteration, float diff, int numSwaps, float denominator, const char* phase ) {
#ifdef OUTPUT
	ostringstream out;
	out.imbue( userLocale() );
		out << "Iteration #" << iteration
			<< ", Diff: " << fixed << diff
			<< ", Swaps: " << numSwaps << " ( " << ( numSwaps / denominator * 100.0 ) << "% )"
			<< " (" << phase << ")\n";
	cout << out.str();
#else
	( void ) iteration;
	( void ) diff;
	( void ) numSwaps;
	( void ) denominator;
	( void ) phase;
#endif
}

template <bool UseNeon>
void processPNGFileImpl( Color* __restrict src, const Color* __restrict dst, png_bytep* rowPointersNew, const KernelTables& tables ) {
	PROFILE_FUNCTION();
	PNGLAB_PMU_SCOPE( "process_png_file" );

	const int innerOrderedLoopCount = 300000 * ( dWidth / 320 ) * ( dWidth / 320 );

#ifdef PROFILE_STATS
	const uint64_t orderedCandidates = static_cast<uint64_t>( kOrderedLoopCount ) * static_cast<uint64_t>( innerOrderedLoopCount );
	const uint64_t randomCandidates = 100000ULL * static_cast<uint64_t>( kRandomLoopCount - 1 ) * static_cast<uint64_t>( kRandomLoopCount ) / 2ULL;
	ProfileStats::ScopedTimer processTimer( ProfileStats::EventId::ProcessPNG, orderedCandidates + randomCandidates );
#endif

#ifdef PROFILE_STATS
	{
		ProfileStats::ScopedTimer orderedTimer( ProfileStats::EventId::OrderedLoop, orderedCandidates );
#endif
		PNGLAB_PMU_SCOPE( "ordered_loop" );
		for( int j = 0; j < kOrderedLoopCount; ++j ) {
			int numSwaps = 0;
			uint32_t row1 = 0;
			uint32_t row2 = 0;
			uint32_t col1 = 0;
			uint32_t col2 = 0;
			const uint32_t* nextRows = tables.orderedHeight.nextForStep( j );
			const uint32_t* carryRows = tables.orderedHeight.carryForStep( j );
			const uint32_t* nextCols = tables.orderedWidth.nextForStep( j );
			const uint32_t* carryCols = tables.orderedWidth.carryForStep( j );

			for( int i = 0; i < innerOrderedLoopCount; ++i ) {
				Color* const sRow1 = tables.srcRows[row1];
				Color* const sRow2 = tables.srcRows[row2];
				const Color* const dRow1 = tables.dstRows[row1];
				const Color* const dRow2 = tables.dstRows[row2];

				Color* const sPx1 = sRow1 + col1;
				Color* const sPx2 = sRow2 + col2;
				const Color* const dPx1 = dRow1 + col1;
				const Color* const dPx2 = dRow2 + col2;

				if( shouldSwapImpl<UseNeon>( sPx1, sPx2, dPx1, dPx2 ) ) {
					swapPixels( sPx1, sPx2 );
					++numSwaps;
				}

				advanceOrderedState( nextRows, carryRows, static_cast<uint32_t>( dHeight ), row1, row2 );
				advanceOrderedState( nextCols, carryCols, static_cast<uint32_t>( dWidth ), col1, col2 );
			}

#ifdef OUTPUT
			const float diff = totalDiff( src, dst );
			printIterationStatus( j, diff, numSwaps, static_cast<float>( innerOrderedLoopCount ), "ordered" );
#endif

#ifdef ANIMATION
			ostringstream ss;
			ss << setw( 5 ) << setfill( '0' ) << 24 + j;
			const string newFilename = filePrefix + ss.str() + ".png";
			labToImage( src, rowPointersNew );
			writePNGFile( newFilename.c_str(), rowPointersNew, dWidth, dHeight );
#endif
		}
#ifdef PROFILE_STATS
	}
#endif

#ifdef PROFILE_STATS
	{
		ProfileStats::ScopedTimer randomTimer( ProfileStats::EventId::RandomLoop, randomCandidates );
#endif
		PNGLAB_PMU_SCOPE( "random_loop" );
		for( int j = 0; j < kRandomLoopCount; ++j ) {
			int numSwaps = 0;
			for( int i = 0; i < j * 100000; ++i ) {
				const uint64_t r = xorshift64star();
				const uint32_t r1 = static_cast<uint32_t>( r & 0xFFFFULL );
				const uint32_t r2 = static_cast<uint32_t>( ( r >> 16 ) & 0xFFFFULL );
				const uint32_t r3 = static_cast<uint32_t>( ( r >> 32 ) & 0xFFFFULL );
				const uint32_t r4 = static_cast<uint32_t>( ( r >> 48 ) & 0xFFFFULL );

				const uint32_t y1 = tables.modHeight[r1];
				const uint32_t y2 = tables.modHeight[r2];
				const uint32_t x1 = tables.modWidth[r3];
				const uint32_t x2 = tables.modWidth[r4];

				Color* const sRow1 = tables.srcRows[y1];
				Color* const sRow2 = tables.srcRows[y2];
				const Color* const dRow1 = tables.dstRows[y1];
				const Color* const dRow2 = tables.dstRows[y2];

				Color* const sPx1 = sRow1 + x1;
				Color* const sPx2 = sRow2 + x2;
				const Color* const dPx1 = dRow1 + x1;
				const Color* const dPx2 = dRow2 + x2;

				if( shouldSwapImpl<UseNeon>( sPx1, sPx2, dPx1, dPx2 ) ) {
					swapPixels( sPx1, sPx2 );
					++numSwaps;
				}
			}

#ifdef OUTPUT
			const float diff = totalDiff( src, dst );
			printIterationStatus( j + kOrderedLoopCount, diff, numSwaps, static_cast<float>( j * 100000 ), "random" );
#endif

#ifdef ANIMATION
			ostringstream ss;
			ss << setw( 5 ) << setfill( '0' ) << 24 + j + kOrderedLoopCount;
			const string newFilename = filePrefix + ss.str() + ".png";
			labToImage( src, rowPointersNew );
			writePNGFile( newFilename.c_str(), rowPointersNew, dWidth, dHeight );
#endif
		}
#ifdef PROFILE_STATS
	}
#endif
}

void processPNGFile( Color* __restrict src, const Color* __restrict dst, png_bytep* rowPointersNew, const KernelTables& tables ) {
#if defined(PNG_LAB_ENABLE_NEON) && defined(__ARM_NEON)
	if( useNeonSwapKernel ) {
		processPNGFileImpl<true>( src, dst, rowPointersNew, tables );
		return;
	}
#endif
	processPNGFileImpl<false>( src, dst, rowPointersNew, tables );
}

string split( string& s ) {
	PROFILE_FUNCTION();
	stringstream ss( s );
	string result;
	getline( ss, result, '/' );
	getline( ss, result, '.' );
	return result;
}

void seedFromExactPalettePixels( png_bytep* rowPointersDst, png_bytep* rowPointersSrc ) {
	PNGLAB_PMU_SCOPE( "seed_copy" );
#ifdef PROFILE_STATS
	ProfileStats::ScopedTimer seedCopyTimer( ProfileStats::EventId::SeedCopy, static_cast<uint64_t>( dWidth ) * static_cast<uint64_t>( dHeight ) );
#endif
	int dstY = 0;
	int dstX = 0;
	for( int srcY = 0; srcY < sHeight; ++srcY ) {
		const png_bytep srcRow = rowPointersSrc[srcY];
		for( int srcX = 0; srcX < sWidth; ++srcX ) {
			memcpy( &rowPointersDst[dstY][dstX * 4], &srcRow[srcX * 4], 4 );
			++dstX;
			if( dstX == dWidth ) {
				dstX = 0;
				++dstY;
			}
		}
	}
}

#ifdef SHOULD_SWAP_AB_HARNESS
HOT_INLINE uint32_t floatBits( const float value ) {
	uint32_t bits = 0;
	memcpy( &bits, &value, sizeof( bits ) );
	return bits;
}

HOT_INLINE uint32_t ulpDistance( const float lhs, const float rhs ) {
	const uint32_t lhsBits = floatBits( lhs );
	const uint32_t rhsBits = floatBits( rhs );
	return lhsBits > rhsBits ? lhsBits - rhsBits : rhsBits - lhsBits;
}

bool swapBreakdownMatchesBitwise( const SwapBreakdown& lhs, const SwapBreakdown& rhs ) {
	return floatBits( lhs.swap12 ) == floatBits( rhs.swap12 )
		&& floatBits( lhs.swap21 ) == floatBits( rhs.swap21 )
		&& floatBits( lhs.keep11 ) == floatBits( rhs.keep11 )
		&& floatBits( lhs.keep22 ) == floatBits( rhs.keep22 )
		&& floatBits( lhs.swapCost ) == floatBits( rhs.swapCost )
		&& floatBits( lhs.keepCost ) == floatBits( rhs.keepCost );
}

void printSwapFieldComparison( const char* fieldName, const float scalarValue, const float otherValue ) {
	cout << "  " << fieldName
		<< ": scalar=" << setprecision( 9 ) << scalarValue
		<< " [0x" << hex << floatBits( scalarValue ) << dec << "]"
		<< ", other=" << setprecision( 9 ) << otherValue
		<< " [0x" << hex << floatBits( otherValue ) << dec << "]"
		<< ", ulp=" << ulpDistance( scalarValue, otherValue ) << '\n';
}

void reportHarnessDrift(
	const char* implementation,
	const char* driftType,
	const char* phase,
	const int iteration,
	const int candidate,
	const uint32_t row1,
	const uint32_t col1,
	const uint32_t row2,
	const uint32_t col2,
	const SwapBreakdown& scalar,
	const SwapBreakdown& other
) {
	cout << implementation << " " << driftType
		<< " at phase=" << phase
		<< ", iteration=" << iteration
		<< ", candidate=" << candidate
		<< ", src1=(" << row1 << ',' << col1 << ')'
		<< ", src2=(" << row2 << ',' << col2 << ")\n";
	printSwapFieldComparison( "swap12", scalar.swap12, other.swap12 );
	printSwapFieldComparison( "swap21", scalar.swap21, other.swap21 );
	printSwapFieldComparison( "keep11", scalar.keep11, other.keep11 );
	printSwapFieldComparison( "keep22", scalar.keep22, other.keep22 );
	printSwapFieldComparison( "swapCost", scalar.swapCost, other.swapCost );
	printSwapFieldComparison( "keepCost", scalar.keepCost, other.keepCost );
	cout << "  scalarDecision=" << scalar.decision()
		<< ", otherDecision=" << other.decision() << '\n';
}

int runShouldSwapHarness( int argc, char* argv[] ) {
#if !defined(PNG_LAB_ENABLE_NEON) || !defined(__ARM_NEON)
	( void ) argc;
	( void ) argv;
	cerr << "shouldSwap_ab_harness requires an ARM NEON build" << endl;
	return 1;
#else
	if( argc != 3 ) {
		cout << "Usage: " << argv[0] << " <palette image> <source image>" << endl;
		return 1;
	}

	x = kInitialRandomSeed;

	png_bytep* rowPointersSrc = nullptr;
	png_bytep* rowPointersDst = nullptr;
	png_bytep* rowPointersNew = nullptr;

	readPNGFile( argv[1], &rowPointersSrc, &sWidth, &sHeight );
	readPNGFile( argv[2], &rowPointersDst, &dWidth, &dHeight );
	useNeonSwapKernel = kUseNeonSwapKernel
		&& ( static_cast<uint64_t>( dWidth ) * static_cast<uint64_t>( dHeight ) * sizeof( Color ) * 2ULL ) <= kNeonWorkingSetLimitBytes;

	const size_t stride = static_cast<size_t>( dWidth ) * 4;
	png_bytep rowBuffer = static_cast<png_bytep>( malloc( stride * static_cast<size_t>( dHeight ) ) );
	rowPointersNew = static_cast<png_bytep*>( malloc( sizeof( png_bytep ) * static_cast<size_t>( dHeight ) ) );
	for( int y = 0; y < dHeight; ++y ) {
		rowPointersNew[y] = rowBuffer + static_cast<size_t>( y ) * stride;
	}

	if( static_cast<uint64_t>( sWidth ) * static_cast<uint64_t>( sHeight ) != static_cast<uint64_t>( dWidth ) * static_cast<uint64_t>( dHeight ) ) {
		cerr << "Palette and target must contain the same number of pixels. Got "
			<< sWidth << 'x' << sHeight
			<< " vs "
			<< dWidth << 'x' << dHeight
			<< '.' << endl;
		return 1;
	}
	seedFromExactPalettePixels( rowPointersNew, rowPointersSrc );

	for( int y = 0; y < sHeight; ++y ) {
		free( rowPointersSrc[y] );
	}
	free( rowPointersSrc );

	Color* srcLab = imageToLab( rowPointersNew );
	Color* dstLab = imageToLab( rowPointersDst );
	const KernelTables tables( srcLab, dstLab );

	const int innerOrderedLoopCount = 300000 * ( dWidth / 320 ) * ( dWidth / 320 );
	bool vectorNumericReported = false;
	bool vectorDecisionReported = false;
	bool scalarReduceNumericReported = false;
	bool scalarReduceDecisionReported = false;

	const auto inspectCandidate = [&]( const char* phase, const int iteration, const int candidate, const uint32_t row1, const uint32_t col1, const uint32_t row2, const uint32_t col2, Color* sPx1, Color* sPx2, const Color* dPx1, const Color* dPx2 ) {
		const SwapBreakdown scalar = scalarSwapBreakdown( sPx1, sPx2, dPx1, dPx2 );
		const SwapBreakdown vector = neonVectorSwapBreakdown( sPx1, sPx2, dPx1, dPx2 );
		const SwapBreakdown scalarReduce = neonScalarReduceSwapBreakdown( sPx1, sPx2, dPx1, dPx2 );

		if( !vectorNumericReported && !swapBreakdownMatchesBitwise( scalar, vector ) ) {
			reportHarnessDrift( "neon-vector-reduce", "numeric drift", phase, iteration, candidate, row1, col1, row2, col2, scalar, vector );
			vectorNumericReported = true;
		}

		if( !scalarReduceNumericReported && !swapBreakdownMatchesBitwise( scalar, scalarReduce ) ) {
			reportHarnessDrift( "neon-scalar-reduce", "numeric drift", phase, iteration, candidate, row1, col1, row2, col2, scalar, scalarReduce );
			scalarReduceNumericReported = true;
		}

		if( !vectorDecisionReported && scalar.decision() != vector.decision() ) {
			reportHarnessDrift( "neon-vector-reduce", "decision drift", phase, iteration, candidate, row1, col1, row2, col2, scalar, vector );
			vectorDecisionReported = true;
		}

		if( !scalarReduceDecisionReported && scalar.decision() != scalarReduce.decision() ) {
			reportHarnessDrift( "neon-scalar-reduce", "decision drift", phase, iteration, candidate, row1, col1, row2, col2, scalar, scalarReduce );
			scalarReduceDecisionReported = true;
		}

		if( scalar.decision() ) {
			swapPixels( sPx1, sPx2 );
		}
	};

	for( int j = 0; j < kOrderedLoopCount; ++j ) {
		uint32_t row1 = 0;
		uint32_t row2 = 0;
		uint32_t col1 = 0;
		uint32_t col2 = 0;
		const uint32_t* nextRows = tables.orderedHeight.nextForStep( j );
		const uint32_t* carryRows = tables.orderedHeight.carryForStep( j );
		const uint32_t* nextCols = tables.orderedWidth.nextForStep( j );
		const uint32_t* carryCols = tables.orderedWidth.carryForStep( j );

		for( int i = 0; i < innerOrderedLoopCount; ++i ) {
			Color* const sRow1 = tables.srcRows[row1];
			Color* const sRow2 = tables.srcRows[row2];
			const Color* const dRow1 = tables.dstRows[row1];
			const Color* const dRow2 = tables.dstRows[row2];

			Color* const sPx1 = sRow1 + col1;
			Color* const sPx2 = sRow2 + col2;
			const Color* const dPx1 = dRow1 + col1;
			const Color* const dPx2 = dRow2 + col2;

			inspectCandidate( "ordered", j, i, row1, col1, row2, col2, sPx1, sPx2, dPx1, dPx2 );

			advanceOrderedState( nextRows, carryRows, static_cast<uint32_t>( dHeight ), row1, row2 );
			advanceOrderedState( nextCols, carryCols, static_cast<uint32_t>( dWidth ), col1, col2 );
		}
	}

	for( int j = 0; j < kRandomLoopCount; ++j ) {
		for( int i = 0; i < j * 100000; ++i ) {
			const uint64_t r = xorshift64star();
			const uint32_t r1 = static_cast<uint32_t>( r & 0xFFFFULL );
			const uint32_t r2 = static_cast<uint32_t>( ( r >> 16 ) & 0xFFFFULL );
			const uint32_t r3 = static_cast<uint32_t>( ( r >> 32 ) & 0xFFFFULL );
			const uint32_t r4 = static_cast<uint32_t>( ( r >> 48 ) & 0xFFFFULL );

			const uint32_t y1 = tables.modHeight[r1];
			const uint32_t y2 = tables.modHeight[r2];
			const uint32_t x1 = tables.modWidth[r3];
			const uint32_t x2 = tables.modWidth[r4];

			Color* const sRow1 = tables.srcRows[y1];
			Color* const sRow2 = tables.srcRows[y2];
			const Color* const dRow1 = tables.dstRows[y1];
			const Color* const dRow2 = tables.dstRows[y2];

			Color* const sPx1 = sRow1 + x1;
			Color* const sPx2 = sRow2 + x2;
			const Color* const dPx1 = dRow1 + x1;
			const Color* const dPx2 = dRow2 + x2;

			inspectCandidate( "random", j + kOrderedLoopCount, i, y1, x1, y2, x2, sPx1, sPx2, dPx1, dPx2 );
		}
	}

	if( !vectorNumericReported ) {
		cout << "neon-vector-reduce: no numeric drift across full run\n";
	}
	if( !vectorDecisionReported ) {
		cout << "neon-vector-reduce: no decision drift across full run\n";
	}
	if( !scalarReduceNumericReported ) {
		cout << "neon-scalar-reduce: no numeric drift across full run\n";
	}
	if( !scalarReduceDecisionReported ) {
		cout << "neon-scalar-reduce: no decision drift across full run\n";
	}

	free( dstLab );

	for( int y = 0; y < dHeight; ++y ) {
		free( rowPointersDst[y] );
	}
	free( rowPointersDst );
	free( rowBuffer );
	free( rowPointersNew );
	free( srcLab );

	return scalarReduceDecisionReported ? 3 : 0;
#endif
}
#endif

} // namespace

#ifndef SHOULD_SWAP_AB_HARNESS
int main( int argc, char* argv[] ) {
	PROFILE_FUNCTION();
#ifdef PROFILE_STATS
	ProfileStats::ScopedTimer mainTimer( ProfileStats::EventId::ProgramTotal );
#endif
	if( argc != 4 ) {
		cout << "Usage: " << argv[0] << " <palette image> <source image> <output image>" << endl;
		exit( 1 );
	}
	primePnglabCpuCounters();
	PNGLAB_PMU_SCOPE( "program_total" );

	png_bytep* rowPointersSrc = nullptr;
	png_bytep* rowPointersDst = nullptr;
	png_bytep* rowPointersNew = nullptr;

	readPNGFile( argv[1], &rowPointersSrc, &sWidth, &sHeight );
	readPNGFile( argv[2], &rowPointersDst, &dWidth, &dHeight );
	if( static_cast<uint64_t>( sWidth ) * static_cast<uint64_t>( sHeight ) != static_cast<uint64_t>( dWidth ) * static_cast<uint64_t>( dHeight ) ) {
		cerr << "Palette and target must contain the same number of pixels. Got "
			<< sWidth << 'x' << sHeight
			<< " vs "
			<< dWidth << 'x' << dHeight
			<< '.' << endl;
		for( int y = 0; y < sHeight; ++y ) {
			free( rowPointersSrc[y] );
		}
		free( rowPointersSrc );
		for( int y = 0; y < dHeight; ++y ) {
			free( rowPointersDst[y] );
		}
		free( rowPointersDst );
		return 1;
	}

	const size_t stride = static_cast<size_t>( dWidth ) * 4;
	png_bytep rowBuffer = static_cast<png_bytep>( malloc( stride * static_cast<size_t>( dHeight ) ) );
	rowPointersNew = static_cast<png_bytep*>( malloc( sizeof( png_bytep ) * static_cast<size_t>( dHeight ) ) );
	for( int y = 0; y < dHeight; ++y ) {
		rowPointersNew[y] = rowBuffer + static_cast<size_t>( y ) * stride;
	}

	seedFromExactPalettePixels( rowPointersNew, rowPointersSrc );

	for( int y = 0; y < sHeight; ++y ) {
		free( rowPointersSrc[y] );
	}
	free( rowPointersSrc );

	Color* srcLab = imageToLab( rowPointersNew );
	Color* dstLab = imageToLab( rowPointersDst );
	const KernelTables tables( srcLab, dstLab );

#ifdef ANIMATION
	ostringstream ss;
	ss << argv[3];
	filePrefix = ss.str();
	filePrefix = "out" + split( filePrefix );
	writePNGFile( string( filePrefix + "00000.png" ).c_str(), rowPointersNew, dWidth, dHeight );
#endif

	processPNGFile( srcLab, dstLab, rowPointersNew, tables );

	labToImage( srcLab, rowPointersNew );
	writePNGFile( argv[3], rowPointersNew, dWidth, dHeight );

	free( dstLab );

	for( int y = 0; y < dHeight; ++y ) {
		free( rowPointersDst[y] );
	}
	free( rowPointersDst );

	free( rowBuffer );
	free( rowPointersNew );
	free( srcLab );

	return 0;
}
#else
int main( int argc, char* argv[] ) {
	return runShouldSwapHarness( argc, argv );
}
#endif
