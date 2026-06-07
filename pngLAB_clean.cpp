#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

#if defined(PNG_LAB_ENABLE_NEON) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "fmath.hpp"
#include "pngReadWrite.h"
#include "profile_stats.h"
#include "profiler.h"

namespace {

constexpr int kOrderedLoopCount = 150;
constexpr int kRandomLoopCount = 100;
constexpr uint64_t kInitialRandomSeed = 0x8E588AFE51D8B00DULL;
constexpr uint64_t kNeonWorkingSetLimitBytes = 8ULL * 1024ULL * 1024ULL;

#define HOT_INLINE inline __attribute__((always_inline))

#ifdef PROFILE_STATS
#define PROFILE_SAMPLED_BEGIN(counter_name)                                          \
	static uint64_t counter_name = 0;                                               \
	++counter_name;                                                                 \
	const bool sampled = ( counter_name & 0xFFFu ) == 0;                            \
	const uint64_t startNs = sampled ? ProfileStats::nowNs() : 0
#define PROFILE_SAMPLED_END(event_id)                                                \
	if( sampled ) {                                                                  \
		ProfileStats::add( event_id, ProfileStats::nowNs() - startNs, 1, 1 );      \
	}
#else
#define PROFILE_SAMPLED_BEGIN(counter_name)
#define PROFILE_SAMPLED_END(event_id)
#endif

// Shared image dimensions used by the copy, conversion, and kernel phases.
struct Geometry {
	int width = 0;
	int height = 0;

	// Convenience helper for instrumentation and allocation sizing.
	HOT_INLINE size_t pixelCount() const {
		return static_cast<size_t>( width ) * static_cast<size_t>( height );
	}

	// PNG rows are stored as packed RGBA bytes, so each pixel is 4 bytes.
	HOT_INLINE size_t rgbaStrideBytes() const {
		return static_cast<size_t>( width ) * 4u;
	}
};

// Internal LAB/XYZ/RGB working pixel. The padded 16-byte size is required for aligned NEON loads.
struct alignas( 16 ) LabColor {
	float l;
	float a;
	float b;
	float pad;
};

static_assert( sizeof( LabColor ) == 16, "LabColor must stay 16-byte aligned for the NEON path" );

// Small aligned allocator used by the hot LAB buffers so the kernel can rely on vector-friendly addresses.
template <typename T>
T* allocateAlignedArray( const size_t count ) {
	void* ptr = nullptr;
	if( posix_memalign( &ptr, alignof( T ), sizeof( T ) * count ) != 0 ) {
		std::abort();
	}
	return static_cast<T*>( ptr );
}

// Owns PNG row-pointer storage. It supports both libpng-style per-row allocation and our contiguous output buffer.
class OwnedRows {
public:
	OwnedRows() = default;

	// Move-only because destruction frees the underlying row storage.
	OwnedRows( OwnedRows&& other ) noexcept
		: rows_( other.rows_ ),
		  contiguousBuffer_( other.contiguousBuffer_ ),
		  height_( other.height_ ),
		  contiguous_( other.contiguous_ ) {
		other.rows_ = nullptr;
		other.contiguousBuffer_ = nullptr;
		other.height_ = 0;
		other.contiguous_ = false;
	}

	// Transfers ownership without copying the row buffer.
	OwnedRows& operator=( OwnedRows&& other ) noexcept {
		if( this != &other ) {
			reset();
			rows_ = other.rows_;
			contiguousBuffer_ = other.contiguousBuffer_;
			height_ = other.height_;
			contiguous_ = other.contiguous_;
			other.rows_ = nullptr;
			other.contiguousBuffer_ = nullptr;
			other.height_ = 0;
			other.contiguous_ = false;
		}
		return *this;
	}

	OwnedRows( const OwnedRows& ) = delete;
	OwnedRows& operator=( const OwnedRows& ) = delete;

	// Frees either the libpng-style scattered rows or the contiguous RGBA backing store.
	~OwnedRows() {
		reset();
	}

	// Reads a PNG through the existing helper and captures its geometry alongside the row pointers.
	static OwnedRows read( const char* filename, Geometry& geometry ) {
		OwnedRows image;
		readPNGFile( filename, &image.rows_, &geometry.width, &geometry.height );
		image.height_ = geometry.height;
		return image;
	}

	// Allocates a contiguous RGBA image because the output/animation path rewrites every row repeatedly.
	static OwnedRows allocateContiguous( const Geometry& geometry ) {
		OwnedRows image;
		image.contiguous_ = true;
		image.height_ = geometry.height;
		image.contiguousBuffer_ = static_cast<png_bytep>( std::malloc( geometry.rgbaStrideBytes() * static_cast<size_t>( geometry.height ) ) );
		image.rows_ = static_cast<png_bytep*>( std::malloc( sizeof( png_bytep ) * static_cast<size_t>( geometry.height ) ) );
		if( image.contiguousBuffer_ == nullptr || image.rows_ == nullptr ) {
			std::abort();
		}
		for( int y = 0; y < geometry.height; ++y ) {
			image.rows_[y] = image.contiguousBuffer_ + static_cast<size_t>( y ) * geometry.rgbaStrideBytes();
		}
		return image;
	}

	// Mutable row-pointer view used by libpng and the conversion routines.
	HOT_INLINE png_bytep* rows() {
		return rows_;
	}

	// Const-qualified accessor that still returns raw row pointers for libpng-style APIs.
	HOT_INLINE png_bytep* rows() const {
		return rows_;
	}

	// Initializes the working image by reshaping the source pixels into the target geometry without changing the pixel multiset.
	void seedFromExactPalettePixels( const OwnedRows& palette, const Geometry& paletteGeometry, const Geometry& targetGeometry ) {
#ifdef PROFILE_STATS
		ProfileStats::ScopedTimer timer( ProfileStats::EventId::SeedCopy, static_cast<uint64_t>( targetGeometry.pixelCount() ) );
#endif
		int dstY = 0;
		int dstX = 0;
		for( int srcY = 0; srcY < paletteGeometry.height; ++srcY ) {
			const png_bytep srcRow = palette.rows_[srcY];
			for( int srcX = 0; srcX < paletteGeometry.width; ++srcX ) {
				std::memcpy( &rows_[dstY][dstX * 4], &srcRow[srcX * 4], 4 );
				++dstX;
				if( dstX == targetGeometry.width ) {
					dstX = 0;
					++dstY;
				}
			}
		}
	}

private:
	// Centralized cleanup keeps the move-only ownership rules easy to audit.
	void reset() {
		if( rows_ == nullptr ) {
			return;
		}
		if( contiguous_ ) {
			std::free( contiguousBuffer_ );
			std::free( rows_ );
		} else {
			for( int y = 0; y < height_; ++y ) {
				std::free( rows_[y] );
			}
			std::free( rows_ );
		}
		rows_ = nullptr;
		contiguousBuffer_ = nullptr;
		height_ = 0;
		contiguous_ = false;
	}

	png_bytep* rows_ = nullptr;
	png_bytep contiguousBuffer_ = nullptr;
	int height_ = 0;
	bool contiguous_ = false;
};

// Owns the hot LAB pixel buffer. This stays contiguous and aligned so the swap kernel sees predictable addresses.
class LabBuffer {
public:
	// Allocates the entire LAB image in one block to keep row addressing cheap.
	explicit LabBuffer( const Geometry& geometry )
		: geometry_( geometry ),
		  pixels_( allocateAlignedArray<LabColor>( geometry.pixelCount() ) ) {
	}

	// Move support lets main build and return temporary buffers without copying image data.
	LabBuffer( LabBuffer&& other ) noexcept
		: geometry_( other.geometry_ ),
		  pixels_( other.pixels_ ) {
		other.geometry_ = {};
		other.pixels_ = nullptr;
	}

	// Rebinds ownership to a new LAB allocation.
	LabBuffer& operator=( LabBuffer&& other ) noexcept {
		if( this != &other ) {
			std::free( pixels_ );
			geometry_ = other.geometry_;
			pixels_ = other.pixels_;
			other.geometry_ = {};
			other.pixels_ = nullptr;
		}
		return *this;
	}

	LabBuffer( const LabBuffer& ) = delete;
	LabBuffer& operator=( const LabBuffer& ) = delete;

	// Releases the aligned LAB storage.
	~LabBuffer() {
		std::free( pixels_ );
	}

	// Raw pixel pointer used by the kernel and table builders.
	HOT_INLINE LabColor* data() {
		return pixels_;
	}

	// Const raw pixel pointer for read-only kernel inputs.
	HOT_INLINE const LabColor* data() const {
		return pixels_;
	}

	// Row helper to avoid repeating width multiplies in callers.
	HOT_INLINE LabColor* row( const int y ) {
		return pixels_ + static_cast<size_t>( y ) * static_cast<size_t>( geometry_.width );
	}

	// Const row helper for read-only consumers.
	HOT_INLINE const LabColor* row( const int y ) const {
		return pixels_ + static_cast<size_t>( y ) * static_cast<size_t>( geometry_.width );
	}

private:
	Geometry geometry_;
	LabColor* pixels_ = nullptr;
};

// Precomputes exact ordered-loop state transitions so the kernel does not divide in the hot path.
struct AdvanceTables {
	int dimension = 0;
	int stepCount = 0;
	std::vector<uint32_t> next;
	std::vector<uint32_t> carry;

	AdvanceTables() = default;

	AdvanceTables( const int dim, const int steps )
		: dimension( dim ),
		  stepCount( steps ),
		  next( static_cast<size_t>( dim ) * static_cast<size_t>( steps ) ),
		  carry( static_cast<size_t>( dim ) * static_cast<size_t>( steps ) ) {
		for( int stepIndex = 0; stepIndex < stepCount; ++stepIndex ) {
			const int step = stepIndex + 1;
			for( int state = 0; state < dimension; ++state ) {
				const uint32_t value = static_cast<uint32_t>( state + step );
				const size_t index = static_cast<size_t>( stepIndex ) * static_cast<size_t>( dimension ) + static_cast<size_t>( state );
				next[index] = value % static_cast<uint32_t>( dimension );
				carry[index] = value / static_cast<uint32_t>( dimension );
			}
		}
	}

	// Returns the per-state "next minor index" table for one ordered-loop step.
	HOT_INLINE const uint32_t* nextForStep( const int stepIndex ) const {
		return next.data() + static_cast<size_t>( stepIndex ) * static_cast<size_t>( dimension );
	}

	// Returns the carry table that advances the paired major index when the minor wraps.
	HOT_INLINE const uint32_t* carryForStep( const int stepIndex ) const {
		return carry.data() + static_cast<size_t>( stepIndex ) * static_cast<size_t>( dimension );
	}
};

// All lookup tables and row bases needed by the swap kernel. Built once, then read-only during the run.
struct KernelTables {
	std::vector<LabColor*> srcRows;
	std::vector<const LabColor*> dstRows;
	AdvanceTables orderedRows;
	AdvanceTables orderedCols;
	std::vector<uint32_t> modRows;
	std::vector<uint32_t> modCols;

	KernelTables( const Geometry& geometry, LabColor* srcPixels, const LabColor* dstPixels )
		: srcRows( static_cast<size_t>( geometry.height ) ),
		  dstRows( static_cast<size_t>( geometry.height ) ),
		  orderedRows( geometry.height, kOrderedLoopCount ),
		  orderedCols( geometry.width, kOrderedLoopCount ),
		  modRows( 1u << 16 ),
		  modCols( 1u << 16 ) {
		for( int y = 0; y < geometry.height; ++y ) {
			srcRows[static_cast<size_t>( y )] = srcPixels + static_cast<size_t>( y ) * static_cast<size_t>( geometry.width );
			dstRows[static_cast<size_t>( y )] = dstPixels + static_cast<size_t>( y ) * static_cast<size_t>( geometry.width );
		}

		for( uint32_t value = 0; value < ( 1u << 16 ); ++value ) {
			modRows[value] = value % static_cast<uint32_t>( geometry.height );
			modCols[value] = value % static_cast<uint32_t>( geometry.width );
		}
	}
};

// Exact RNG used by the original algorithm. The state and bit extraction order must not change.
class XorShift64Star {
public:
	// Generates one 64-bit sample and records sampled timing when profiling is enabled.
	HOT_INLINE uint64_t next() {
		PROFILE_FUNCTION();
		PROFILE_SAMPLED_BEGIN( sampleCounter );
		state_ ^= state_ >> 12;
		state_ ^= state_ << 25;
		state_ ^= state_ >> 27;
		const uint64_t result = state_ * 2685821657736338717ULL;
		PROFILE_SAMPLED_END( ProfileStats::EventId::XorShiftSample );
		return result;
	}

private:
	uint64_t state_ = kInitialRandomSeed;
};

// Lazily constructs the user locale once so iteration logging can reuse it cheaply.
const std::locale& userLocale() {
	static const std::locale value( "" );
	return value;
}

// Mirrors the historical animation naming behavior: keep the basename, strip the extension, prefix with "out".
std::string animationPrefixFor( const char* outputPath ) {
	const std::string path( outputPath );
	const size_t slash = path.find_last_of( '/' );
	const size_t begin = slash == std::string::npos ? 0 : slash + 1;
	size_t end = path.find_last_of( '.' );
	if( end == std::string::npos || end < begin ) {
		end = path.size();
	}
	return "out" + path.substr( begin, end - begin );
}

// Converts XYZ-space working values back to display RGB bytes.
inline LabColor xyzToRgb( const LabColor& px ) {
	PROFILE_FUNCTION();
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

// Converts one PNG RGBA pixel to XYZ as the first half of the RGB -> LAB transform.
inline LabColor rgbToXyz( png_bytep px ) {
	PROFILE_FUNCTION();
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

// Converts XYZ values to CIE LAB, which is the metric space the swap kernel operates in.
inline LabColor xyzToLab( const LabColor& px ) {
	PROFILE_FUNCTION();
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

// Inverse LAB -> XYZ transform used when writing the final image or animation frames.
inline LabColor labToXyz( const LabColor& px ) {
	PROFILE_FUNCTION();

	float y = ( px.l + 16.0f ) / 116.0f;
	float x = px.a / 500.0f + y;
	float z = y - px.b / 200.0f;

	x = x * x * x > 0.008856f ? x * x * x : ( x - 16.0f / 116.0f ) / 7.787f;
	y = y * y * y > 0.008856f ? y * y * y : ( y - 16.0f / 116.0f ) / 7.787f;
	z = z * z * z > 0.008856f ? z * z * z : ( z - 16.0f / 116.0f ) / 7.787f;

	return { x * 95.047f, y * 100.0f, z * 108.883f, 0.0f };
}

// Convenience entry point for the full PNG RGBA -> LAB conversion.
inline LabColor rgbaToLab( png_bytep px ) {
	PROFILE_FUNCTION();
	return xyzToLab( rgbToXyz( px ) );
}

// Baseline squared LAB distance used for exact comparisons and diagnostics.
HOT_INLINE float squaredLabDistanceScalar( const LabColor* lhs, const LabColor* rhs ) {
	const float dl = lhs->l - rhs->l;
	const float da = lhs->a - rhs->a;
	const float db = lhs->b - rhs->b;
	return ( dl * dl ) + ( da * da ) + ( db * db );
}

#if defined(PNG_LAB_ENABLE_NEON) && defined(__ARM_NEON)
// NEON-assisted distance that preserves scalar reduction order so swap decisions remain exact.
HOT_INLINE float squaredLabDistanceNeonExact( const LabColor* lhs, const LabColor* rhs ) {
	const float32x4_t diff = vsubq_f32( vld1q_f32( &lhs->l ), vld1q_f32( &rhs->l ) );
	const float dl = vgetq_lane_f32( diff, 0 );
	const float da = vgetq_lane_f32( diff, 1 );
	const float db = vgetq_lane_f32( diff, 2 );
	return ( dl * dl ) + ( da * da ) + ( db * db );
}
#endif

// Thin profiling wrapper around the selected scalar or exact-safe NEON distance implementation.
template <bool UseNeon>
HOT_INLINE float pixelDiff( const LabColor* lhs, const LabColor* rhs ) {
	PROFILE_FUNCTION();
	PROFILE_SAMPLED_BEGIN( sampleCounter );
#if defined(PNG_LAB_ENABLE_NEON) && defined(__ARM_NEON)
	float result = 0.0f;
	if constexpr( UseNeon ) {
		result = squaredLabDistanceNeonExact( lhs, rhs );
	} else {
		result = squaredLabDistanceScalar( lhs, rhs );
	}
#else
	const float result = squaredLabDistanceScalar( lhs, rhs );
#endif
	PROFILE_SAMPLED_END( ProfileStats::EventId::PixelDiffSample );
	return result;
}

// Core swap predicate: compare "crossed" cost versus "kept" cost for one candidate pair.
template <bool UseNeon>
HOT_INLINE bool shouldSwap( const LabColor* src1, const LabColor* src2, const LabColor* dst1, const LabColor* dst2 ) {
	PROFILE_FUNCTION();
	PROFILE_SAMPLED_BEGIN( sampleCounter );
	const float swapCost = pixelDiff<UseNeon>( src1, dst2 ) + pixelDiff<UseNeon>( src2, dst1 );
	const float keepCost = pixelDiff<UseNeon>( src1, dst1 ) + pixelDiff<UseNeon>( src2, dst2 );
	PROFILE_SAMPLED_END( ProfileStats::EventId::SwapDecisionSample );
	return swapCost < keepCost;
}

// Swaps the two source pixels, using a full-width NEON move when that path is active.
template <bool UseNeon>
HOT_INLINE void swapPixels( LabColor* lhs, LabColor* rhs ) {
	PROFILE_FUNCTION();
	PROFILE_SAMPLED_BEGIN( sampleCounter );
#if defined(PNG_LAB_ENABLE_NEON) && defined(__ARM_NEON)
	if constexpr( UseNeon ) {
		const float32x4_t left = vld1q_f32( &lhs->l );
		const float32x4_t right = vld1q_f32( &rhs->l );
		vst1q_f32( &lhs->l, right );
		vst1q_f32( &rhs->l, left );
	} else {
		const LabColor tmp = *lhs;
		*lhs = *rhs;
		*rhs = tmp;
	}
#else
	const LabColor tmp = *lhs;
	*lhs = *rhs;
	*rhs = tmp;
#endif
	PROFILE_SAMPLED_END( ProfileStats::EventId::SwapPixelsSample );
}

// Converts an RGBA image into the aligned LAB working buffer used by the kernel.
LabBuffer imageToLab( const Geometry& geometry, png_bytep* image ) {
	PROFILE_FUNCTION();
#ifdef PROFILE_STATS
	ProfileStats::ScopedTimer timer( ProfileStats::EventId::ImageToLab, static_cast<uint64_t>( geometry.pixelCount() ) );
#endif
	LabBuffer result( geometry );
	for( int y = 0; y < geometry.height; ++y ) {
		png_bytep srcRow = image[y];
		LabColor* dstRow = result.row( y );
		for( int x = 0; x < geometry.width; ++x ) {
			dstRow[x] = rgbaToLab( &srcRow[x * 4] );
		}
	}
	return result;
}

// Converts the LAB working image back to RGBA for final output or animation snapshots.
void labToImage( const Geometry& geometry, const LabColor* lab, png_bytep* image ) {
	PROFILE_FUNCTION();
#ifdef PROFILE_STATS
	ProfileStats::ScopedTimer timer( ProfileStats::EventId::LabToImage, static_cast<uint64_t>( geometry.pixelCount() ) );
#endif
	for( int y = 0; y < geometry.height; ++y ) {
		png_bytep dstRow = image[y];
		const LabColor* srcRow = lab + static_cast<size_t>( y ) * static_cast<size_t>( geometry.width );
		for( int x = 0; x < geometry.width; ++x ) {
			const LabColor rgb = xyzToRgb( labToXyz( srcRow[x] ) );
			dstRow[( x * 4 ) + 0] = static_cast<int>( rgb.l + 0.5f );
			dstRow[( x * 4 ) + 1] = static_cast<int>( rgb.a + 0.5f );
			dstRow[( x * 4 ) + 2] = static_cast<int>( rgb.b + 0.5f );
		}
	}
}

// Diagnostic-only full-image diff used for logging. This is intentionally separate from the hot swap kernel.
float totalDiff( const Geometry& geometry, const LabColor* src, const LabColor* dst ) {
	PROFILE_FUNCTION();
#ifdef PROFILE_STATS
	ProfileStats::ScopedTimer timer( ProfileStats::EventId::TotalDiff, static_cast<uint64_t>( geometry.pixelCount() ) );
#endif
	float diff = 0.0f;
	for( int y = 0; y < geometry.height; ++y ) {
		const LabColor* srcRow = src + static_cast<size_t>( y ) * static_cast<size_t>( geometry.width );
		const LabColor* dstRow = dst + static_cast<size_t>( y ) * static_cast<size_t>( geometry.width );
		for( int x = 0; x < geometry.width; ++x ) {
			diff += squaredLabDistanceScalar( &srcRow[x], &dstRow[x] );
		}
	}
	return diff;
}

// Advances one ordered-loop index pair using the precomputed next/carry tables instead of division/modulo.
HOT_INLINE void advanceOrderedState( const uint32_t* nextTable, const uint32_t* carryTable, const uint32_t limit, uint32_t& major, uint32_t& minor ) {
	const uint32_t oldMinor = minor;
	minor = nextTable[oldMinor];
	major += carryTable[oldMinor];
	while( major >= limit ) {
		major -= limit;
	}
}

// Formats the progress line emitted after each ordered/random outer iteration.
void printIterationStatus( const int iteration, const float diff, const int swaps, const float denominator, const char* phase ) {
#ifdef OUTPUT
	std::ostringstream out;
	out.imbue( userLocale() );
	const float swapPercent = denominator > 0.0f ? ( static_cast<float>( swaps ) / denominator ) * 100.0f : 0.0f;
	out << "Iteration #" << iteration
		<< ", Diff: " << std::fixed << diff
		<< ", Swaps: " << swaps << " ( " << swapPercent << "% )"
		<< " (" << phase << ")\n";
	std::cout << out.str();
#else
	( void ) iteration;
	( void ) diff;
	( void ) swaps;
	( void ) denominator;
	( void ) phase;
#endif
}

#ifdef ANIMATION
// Materializes one animation frame. This stays out of the hot kernel so the swap loop stays branch-light.
void writeAnimationFrame( const std::string& prefix, const int frameNumber, const Geometry& geometry, const LabColor* lab, png_bytep* outputRows ) {
	std::ostringstream name;
	name << prefix << std::setw( 5 ) << std::setfill( '0' ) << frameNumber << ".png";
	labToImage( geometry, lab, outputRows );
	writePNGFile( name.str().c_str(), outputRows, geometry.width, geometry.height );
}
#endif

// Runs the deterministic ordered phase, where candidate coordinates are generated by table-driven state machines.
template <bool UseNeon>
void runOrderedPhase(
	const Geometry& geometry,
	LabColor* __restrict src,
	const LabColor* __restrict dst,
	png_bytep* outputRows,
	const KernelTables& tables,
	const int innerOrderedLoopCount,
	const std::string* animationPrefix
) {
	for( int stepIndex = 0; stepIndex < kOrderedLoopCount; ++stepIndex ) {
		int swaps = 0;
		uint32_t row1 = 0;
		uint32_t row2 = 0;
		uint32_t col1 = 0;
		uint32_t col2 = 0;
		const uint32_t* nextRows = tables.orderedRows.nextForStep( stepIndex );
		const uint32_t* carryRows = tables.orderedRows.carryForStep( stepIndex );
		const uint32_t* nextCols = tables.orderedCols.nextForStep( stepIndex );
		const uint32_t* carryCols = tables.orderedCols.carryForStep( stepIndex );

		for( int i = 0; i < innerOrderedLoopCount; ++i ) {
			LabColor* const srcRow1 = tables.srcRows[row1];
			LabColor* const srcRow2 = tables.srcRows[row2];
			const LabColor* const dstRow1 = tables.dstRows[row1];
			const LabColor* const dstRow2 = tables.dstRows[row2];

			LabColor* const srcPixel1 = srcRow1 + col1;
			LabColor* const srcPixel2 = srcRow2 + col2;
			const LabColor* const dstPixel1 = dstRow1 + col1;
			const LabColor* const dstPixel2 = dstRow2 + col2;

			if( shouldSwap<UseNeon>( srcPixel1, srcPixel2, dstPixel1, dstPixel2 ) ) {
				swapPixels<UseNeon>( srcPixel1, srcPixel2 );
				++swaps;
			}

			advanceOrderedState( nextRows, carryRows, static_cast<uint32_t>( geometry.height ), row1, row2 );
			advanceOrderedState( nextCols, carryCols, static_cast<uint32_t>( geometry.width ), col1, col2 );
		}

#ifdef OUTPUT
		printIterationStatus( stepIndex, totalDiff( geometry, src, dst ), swaps, static_cast<float>( innerOrderedLoopCount ), "ordered" );
#endif

#ifdef ANIMATION
		if( animationPrefix != nullptr ) {
			writeAnimationFrame( *animationPrefix, 24 + stepIndex, geometry, src, outputRows );
		}
#else
		( void ) outputRows;
		( void ) animationPrefix;
#endif
	}
}

// Runs the random phase, using exact modulo lookup tables on the RNG fragments to avoid hot-path division.
template <bool UseNeon>
void runRandomPhase(
	const Geometry& geometry,
	LabColor* __restrict src,
	const LabColor* __restrict dst,
	png_bytep* outputRows,
	const KernelTables& tables,
	XorShift64Star& rng,
	const std::string* animationPrefix
) {
	for( int iteration = 0; iteration < kRandomLoopCount; ++iteration ) {
		int swaps = 0;
		const int candidateCount = iteration * 100000;
		for( int i = 0; i < candidateCount; ++i ) {
			const uint64_t random = rng.next();
			const uint32_t r1 = static_cast<uint32_t>( random & 0xFFFFULL );
			const uint32_t r2 = static_cast<uint32_t>( ( random >> 16 ) & 0xFFFFULL );
			const uint32_t r3 = static_cast<uint32_t>( ( random >> 32 ) & 0xFFFFULL );
			const uint32_t r4 = static_cast<uint32_t>( ( random >> 48 ) & 0xFFFFULL );

			const uint32_t y1 = tables.modRows[r1];
			const uint32_t y2 = tables.modRows[r2];
			const uint32_t x1 = tables.modCols[r3];
			const uint32_t x2 = tables.modCols[r4];

			LabColor* const srcPixel1 = tables.srcRows[y1] + x1;
			LabColor* const srcPixel2 = tables.srcRows[y2] + x2;
			const LabColor* const dstPixel1 = tables.dstRows[y1] + x1;
			const LabColor* const dstPixel2 = tables.dstRows[y2] + x2;

			if( shouldSwap<UseNeon>( srcPixel1, srcPixel2, dstPixel1, dstPixel2 ) ) {
				swapPixels<UseNeon>( srcPixel1, srcPixel2 );
				++swaps;
			}
		}

#ifdef OUTPUT
		printIterationStatus( iteration + kOrderedLoopCount, totalDiff( geometry, src, dst ), swaps, static_cast<float>( candidateCount ), "random" );
#endif

#ifdef ANIMATION
		if( animationPrefix != nullptr ) {
			writeAnimationFrame( *animationPrefix, 24 + kOrderedLoopCount + iteration, geometry, src, outputRows );
		}
#else
		( void ) outputRows;
		( void ) animationPrefix;
#endif
	}
}

// Top-level kernel driver for one scalar or NEON configuration, including per-phase profiling scopes.
template <bool UseNeon>
void processImage( const Geometry& geometry, LabColor* __restrict src, const LabColor* __restrict dst, png_bytep* outputRows, const KernelTables& tables, const std::string* animationPrefix ) {
	PROFILE_FUNCTION();
	const int scale = geometry.width / 320;
	const int innerOrderedLoopCount = 300000 * scale * scale;

#ifdef PROFILE_STATS
	const uint64_t orderedCandidates = static_cast<uint64_t>( kOrderedLoopCount ) * static_cast<uint64_t>( innerOrderedLoopCount );
	const uint64_t randomCandidates = 100000ULL * static_cast<uint64_t>( kRandomLoopCount - 1 ) * static_cast<uint64_t>( kRandomLoopCount ) / 2ULL;
	ProfileStats::ScopedTimer processTimer( ProfileStats::EventId::ProcessPNG, orderedCandidates + randomCandidates );
	{
		ProfileStats::ScopedTimer orderedTimer( ProfileStats::EventId::OrderedLoop, orderedCandidates );
#endif
		runOrderedPhase<UseNeon>( geometry, src, dst, outputRows, tables, innerOrderedLoopCount, animationPrefix );
#ifdef PROFILE_STATS
	}
	{
		ProfileStats::ScopedTimer randomTimer( ProfileStats::EventId::RandomLoop, randomCandidates );
#endif
		XorShift64Star rng;
		runRandomPhase<UseNeon>( geometry, src, dst, outputRows, tables, rng, animationPrefix );
#ifdef PROFILE_STATS
	}
#endif
}

// Heuristic dispatch: use NEON only when the combined source/destination working set is small enough to stay cache-friendly.
bool shouldUseNeonSwapKernel( const Geometry& geometry ) {
#if defined(PNG_LAB_ENABLE_NEON) && defined(__ARM_NEON)
	return ( geometry.pixelCount() * sizeof( LabColor ) * 2ULL ) <= kNeonWorkingSetLimitBytes;
#else
	( void ) geometry;
	return false;
#endif
}

// One-time runtime dispatch between the exact scalar and exact-safe NEON kernels.
void runKernel( const Geometry& geometry, LabColor* __restrict src, const LabColor* __restrict dst, png_bytep* outputRows, const KernelTables& tables, const std::string* animationPrefix ) {
#if defined(PNG_LAB_ENABLE_NEON) && defined(__ARM_NEON)
	if( shouldUseNeonSwapKernel( geometry ) ) {
		processImage<true>( geometry, src, dst, outputRows, tables, animationPrefix );
		return;
	}
#endif
	processImage<false>( geometry, src, dst, outputRows, tables, animationPrefix );
}

} // namespace

// CLI entry point: read PNGs, build the working LAB buffers, run the kernel, and write the final image.
int main( int argc, char* argv[] ) {
	PROFILE_FUNCTION();
#ifdef PROFILE_STATS
	ProfileStats::ScopedTimer mainTimer( ProfileStats::EventId::ProgramTotal );
#endif
	if( argc != 4 ) {
		std::cout << "Usage: " << argv[0] << " <palette image> <source image> <output image>" << std::endl;
		return 1;
	}

	Geometry paletteGeometry;
	Geometry targetGeometry;

	OwnedRows paletteRows = OwnedRows::read( argv[1], paletteGeometry );
	OwnedRows targetRows = OwnedRows::read( argv[2], targetGeometry );
	if( paletteGeometry.pixelCount() != targetGeometry.pixelCount() ) {
		std::cerr << "Palette and target must contain the same number of pixels. Got "
			<< paletteGeometry.width << 'x' << paletteGeometry.height
			<< " vs "
			<< targetGeometry.width << 'x' << targetGeometry.height
			<< '.' << std::endl;
		return 1;
	}
	OwnedRows outputRows = OwnedRows::allocateContiguous( targetGeometry );
	outputRows.seedFromExactPalettePixels( paletteRows, paletteGeometry, targetGeometry );

	LabBuffer srcLab = imageToLab( targetGeometry, outputRows.rows() );
	LabBuffer dstLab = imageToLab( targetGeometry, targetRows.rows() );
	const KernelTables tables( targetGeometry, srcLab.data(), dstLab.data() );

#ifdef ANIMATION
	const std::string animationPrefix = animationPrefixFor( argv[3] );
	writePNGFile( ( animationPrefix + "00000.png" ).c_str(), outputRows.rows(), targetGeometry.width, targetGeometry.height );
	runKernel( targetGeometry, srcLab.data(), dstLab.data(), outputRows.rows(), tables, &animationPrefix );
#else
	runKernel( targetGeometry, srcLab.data(), dstLab.data(), outputRows.rows(), tables, nullptr );
#endif

	labToImage( targetGeometry, srcLab.data(), outputRows.rows() );
	writePNGFile( argv[3], outputRows.rows(), targetGeometry.width, targetGeometry.height );
	return 0;
}
