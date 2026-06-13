#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

#include "fmath.hpp"
#include "pngReadWrite.h"
#include "lab_roundtrip.h"
#include "profile_stats.h"

namespace {

constexpr uint64_t kInitialRandomSeed = 0x8E588AFE51D8B00DULL;

#define HOT_INLINE inline __attribute__((always_inline))

struct Geometry {
	int width = 0;
	int height = 0;

	HOT_INLINE size_t pixelCount() const {
		return static_cast<size_t>( width ) * static_cast<size_t>( height );
	}

	HOT_INLINE size_t rgbaStrideBytes() const {
		return static_cast<size_t>( width ) * 4u;
	}
};

struct Projection {
	float l;
	float a;
	float b;
};

struct SearchStage {
	const char* name;
	int blurRadius;
	int orderedLoops;
	int randomLoops;
	int randomStride;
	float lWeight;
	float chromaWeight;
	int frameEvery;
};

struct SortItem {
	float key;
	uint32_t tie;
	LabColor color;
};

struct PositionItem {
	float key;
	uint32_t index;
};

constexpr std::array<Projection, 6> kInitializationProjections = {{
	{ 1.00f, 0.00f, 0.00f },
	{ 0.75f, 0.45f, 0.10f },
	{ 0.80f, -0.15f, 0.55f },
	{ 0.45f, 0.75f, -0.30f },
	{ 0.60f, 0.10f, -0.70f },
	{ 0.40f, -0.65f, -0.15f },
}};

constexpr std::array<SearchStage, 4> kStages = {{
	{ "coarse", 12, 26, 10, 30000, 1.35f, 0.50f, 6 },
	{ "mid",     6, 38, 16, 50000, 1.15f, 0.80f, 8 },
	{ "fine",    2, 60, 28, 70000, 1.00f, 1.00f, 10 },
	{ "refine",  0, 100, 40, 100000, 1.00f, 1.00f, 12 },
}};

template <typename T>
T* allocateAlignedArray( const size_t count ) {
	void* ptr = nullptr;
	if( posix_memalign( &ptr, alignof( T ), sizeof( T ) * count ) != 0 ) {
		std::abort();
	}
	return static_cast<T*>( ptr );
}

class OwnedRows {
public:
	OwnedRows() = default;

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

	~OwnedRows() {
		reset();
	}

	static OwnedRows read( const char* filename, Geometry& geometry ) {
		OwnedRows image;
		readPNGFile( filename, &image.rows_, &geometry.width, &geometry.height );
		image.height_ = geometry.height;
		return image;
	}

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

	HOT_INLINE png_bytep* rows() {
		return rows_;
	}

	HOT_INLINE png_bytep* rows() const {
		return rows_;
	}

	void seedFromExactPalettePixels( const OwnedRows& palette, const Geometry& paletteGeometry, const Geometry& targetGeometry ) {
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

class LabBuffer {
public:
	explicit LabBuffer( const Geometry& geometry )
		: geometry_( geometry ),
		  pixels_( allocateAlignedArray<LabColor>( geometry.pixelCount() ) ) {
	}

	LabBuffer( LabBuffer&& other ) noexcept
		: geometry_( other.geometry_ ),
		  pixels_( other.pixels_ ) {
		other.geometry_ = {};
		other.pixels_ = nullptr;
	}

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

	~LabBuffer() {
		std::free( pixels_ );
	}

	HOT_INLINE LabColor* data() {
		return pixels_;
	}

	HOT_INLINE const LabColor* data() const {
		return pixels_;
	}

	HOT_INLINE LabColor* row( const int y ) {
		return pixels_ + static_cast<size_t>( y ) * static_cast<size_t>( geometry_.width );
	}

	HOT_INLINE const LabColor* row( const int y ) const {
		return pixels_ + static_cast<size_t>( y ) * static_cast<size_t>( geometry_.width );
	}

private:
	Geometry geometry_;
	LabColor* pixels_ = nullptr;
};

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

	HOT_INLINE const uint32_t* nextForStep( const int stepIndex ) const {
		return next.data() + static_cast<size_t>( stepIndex ) * static_cast<size_t>( dimension );
	}

	HOT_INLINE const uint32_t* carryForStep( const int stepIndex ) const {
		return carry.data() + static_cast<size_t>( stepIndex ) * static_cast<size_t>( dimension );
	}
};

struct SearchTables {
	std::vector<LabColor*> srcRows;
	AdvanceTables orderedRows;
	AdvanceTables orderedCols;
	std::vector<uint32_t> modRows;
	std::vector<uint32_t> modCols;

	SearchTables( const Geometry& geometry, LabColor* srcPixels )
		: srcRows( static_cast<size_t>( geometry.height ) ),
		  orderedRows( geometry.height, 150 ),
		  orderedCols( geometry.width, 150 ),
		  modRows( 1u << 16 ),
		  modCols( 1u << 16 ) {
		for( int y = 0; y < geometry.height; ++y ) {
			srcRows[static_cast<size_t>( y )] = srcPixels + static_cast<size_t>( y ) * static_cast<size_t>( geometry.width );
		}
		for( uint32_t value = 0; value < ( 1u << 16 ); ++value ) {
			modRows[value] = value % static_cast<uint32_t>( geometry.height );
			modCols[value] = value % static_cast<uint32_t>( geometry.width );
		}
	}
};

class XorShift64Star {
public:
	HOT_INLINE uint64_t next() {
		state_ ^= state_ >> 12;
		state_ ^= state_ << 25;
		state_ ^= state_ >> 27;
		return state_ * 2685821657736338717ULL;
	}

private:
	uint64_t state_ = kInitialRandomSeed;
};

class FrameWriter {
public:
	FrameWriter( const Geometry& geometry, png_bytep* rows, const std::string& prefix )
		: geometry_( geometry ),
		  rows_( rows ),
		  prefix_( prefix ) {
	}

	void write( const LabBuffer& lab ) {
#ifdef ANIMATION
		std::ostringstream name;
		name << prefix_ << std::setw( 5 ) << std::setfill( '0' ) << frameIndex_++ << ".png";
		labToImage( lab, rows_ );
		writePNGFile( name.str().c_str(), rows_, geometry_.width, geometry_.height );
#else
		( void ) lab;
#endif
	}

	void labToImage( const LabBuffer& lab, png_bytep* rows ) const;

private:
	Geometry geometry_;
	png_bytep* rows_ = nullptr;
	std::string prefix_;
	int frameIndex_ = 0;
};

std::string framePrefixFor( const char* outputPath ) {
	const std::string path( outputPath );
	const size_t slash = path.find_last_of( '/' );
	const std::string dir = slash == std::string::npos ? std::string() : path.substr( 0, slash + 1 );
	const std::string name = slash == std::string::npos ? path : path.substr( slash + 1 );
	const size_t dot = name.find_last_of( '.' );
	return dir + name.substr( 0, dot == std::string::npos ? name.size() : dot ) + ".pretty.";
}

LabBuffer imageToLab( const Geometry& geometry, png_bytep* image ) {
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

void FrameWriter::labToImage( const LabBuffer& lab, png_bytep* rows ) const {
	for( int y = 0; y < geometry_.height; ++y ) {
		png_bytep dstRow = rows[y];
		const LabColor* srcRow = lab.row( y );
		for( int x = 0; x < geometry_.width; ++x ) {
			const LabColor rgb = xyzToRgb( labToXyz( srcRow[x] ) );
			dstRow[( x * 4 ) + 0] = static_cast<int>( rgb.l + 0.5f );
			dstRow[( x * 4 ) + 1] = static_cast<int>( rgb.a + 0.5f );
			dstRow[( x * 4 ) + 2] = static_cast<int>( rgb.b + 0.5f );
		}
	}
}

std::vector<const LabColor*> buildTargetRows( const Geometry& geometry, const LabBuffer& buffer ) {
	std::vector<const LabColor*> rows( static_cast<size_t>( geometry.height ) );
	for( int y = 0; y < geometry.height; ++y ) {
		rows[static_cast<size_t>( y )] = buffer.row( y );
	}
	return rows;
}

LabBuffer boxBlur( const Geometry& geometry, const LabBuffer& source, const int radius ) {
	if( radius <= 0 ) {
		LabBuffer copy( geometry );
		std::memcpy( copy.data(), source.data(), geometry.pixelCount() * sizeof( LabColor ) );
		return copy;
	}

	LabBuffer horizontal( geometry );
	LabBuffer result( geometry );

	std::vector<float> prefixL( static_cast<size_t>( geometry.width ) + 1u, 0.0f );
	std::vector<float> prefixA( static_cast<size_t>( geometry.width ) + 1u, 0.0f );
	std::vector<float> prefixB( static_cast<size_t>( geometry.width ) + 1u, 0.0f );
	for( int y = 0; y < geometry.height; ++y ) {
		prefixL[0] = 0.0f;
		prefixA[0] = 0.0f;
		prefixB[0] = 0.0f;
		const LabColor* srcRow = source.row( y );
		for( int x = 0; x < geometry.width; ++x ) {
			prefixL[static_cast<size_t>( x ) + 1u] = prefixL[static_cast<size_t>( x )] + srcRow[x].l;
			prefixA[static_cast<size_t>( x ) + 1u] = prefixA[static_cast<size_t>( x )] + srcRow[x].a;
			prefixB[static_cast<size_t>( x ) + 1u] = prefixB[static_cast<size_t>( x )] + srcRow[x].b;
		}
		LabColor* dstRow = horizontal.row( y );
		for( int x = 0; x < geometry.width; ++x ) {
			const int lo = std::max( 0, x - radius );
			const int hi = std::min( geometry.width - 1, x + radius );
			const float scale = 1.0f / static_cast<float>( hi - lo + 1 );
			dstRow[x] = {
				( prefixL[static_cast<size_t>( hi ) + 1u] - prefixL[static_cast<size_t>( lo )] ) * scale,
				( prefixA[static_cast<size_t>( hi ) + 1u] - prefixA[static_cast<size_t>( lo )] ) * scale,
				( prefixB[static_cast<size_t>( hi ) + 1u] - prefixB[static_cast<size_t>( lo )] ) * scale,
				0.0f
			};
		}
	}

	std::vector<float> colL( static_cast<size_t>( geometry.height ) + 1u, 0.0f );
	std::vector<float> colA( static_cast<size_t>( geometry.height ) + 1u, 0.0f );
	std::vector<float> colB( static_cast<size_t>( geometry.height ) + 1u, 0.0f );
	for( int x = 0; x < geometry.width; ++x ) {
		colL[0] = 0.0f;
		colA[0] = 0.0f;
		colB[0] = 0.0f;
		for( int y = 0; y < geometry.height; ++y ) {
			const LabColor& value = horizontal.row( y )[x];
			colL[static_cast<size_t>( y ) + 1u] = colL[static_cast<size_t>( y )] + value.l;
			colA[static_cast<size_t>( y ) + 1u] = colA[static_cast<size_t>( y )] + value.a;
			colB[static_cast<size_t>( y ) + 1u] = colB[static_cast<size_t>( y )] + value.b;
		}
		for( int y = 0; y < geometry.height; ++y ) {
			const int lo = std::max( 0, y - radius );
			const int hi = std::min( geometry.height - 1, y + radius );
			const float scale = 1.0f / static_cast<float>( hi - lo + 1 );
			result.row( y )[x] = {
				( colL[static_cast<size_t>( hi ) + 1u] - colL[static_cast<size_t>( lo )] ) * scale,
				( colA[static_cast<size_t>( hi ) + 1u] - colA[static_cast<size_t>( lo )] ) * scale,
				( colB[static_cast<size_t>( hi ) + 1u] - colB[static_cast<size_t>( lo )] ) * scale,
				0.0f
			};
		}
	}

	return result;
}

HOT_INLINE float projectedKey( const LabColor& color, const Projection& projection ) {
	return ( color.l * projection.l ) + ( color.a * projection.a ) + ( color.b * projection.b );
}

void globallyReassignByProjection( const Geometry& geometry, LabBuffer& working, const LabBuffer& target, const Projection& projection ) {
	std::vector<SortItem> colors;
	std::vector<PositionItem> positions;
	colors.reserve( geometry.pixelCount() );
	positions.reserve( geometry.pixelCount() );

	for( uint32_t index = 0; index < static_cast<uint32_t>( geometry.pixelCount() ); ++index ) {
		const LabColor color = working.data()[index];
		colors.push_back( { projectedKey( color, projection ), index, color } );
		positions.push_back( { projectedKey( target.data()[index], projection ), index } );
	}

	const auto colorLess = []( const SortItem& lhs, const SortItem& rhs ) {
		return lhs.key < rhs.key || ( lhs.key == rhs.key && lhs.tie < rhs.tie );
	};
	const auto positionLess = []( const PositionItem& lhs, const PositionItem& rhs ) {
		return lhs.key < rhs.key || ( lhs.key == rhs.key && lhs.index < rhs.index );
	};

	std::stable_sort( colors.begin(), colors.end(), colorLess );
	std::stable_sort( positions.begin(), positions.end(), positionLess );

	for( size_t i = 0; i < colors.size(); ++i ) {
		working.data()[positions[i].index] = colors[i].color;
	}
}

void runGuidedInitialization( const Geometry& geometry, LabBuffer& working, const std::vector<LabBuffer*>& targetLevels, FrameWriter& frames ) {
	frames.write( working );
	for( size_t pass = 0; pass < targetLevels.size(); ++pass ) {
		globallyReassignByProjection( geometry, working, *targetLevels[pass], kInitializationProjections[pass % kInitializationProjections.size()] );
		frames.write( working );
	}
}

HOT_INLINE float weightedPixelDiff( const LabColor* lhs, const LabColor* rhs, const float lWeight, const float chromaWeight ) {
	const float dl = lhs->l - rhs->l;
	const float da = lhs->a - rhs->a;
	const float db = lhs->b - rhs->b;
	return ( lWeight * dl * dl ) + ( chromaWeight * da * da ) + ( chromaWeight * db * db );
}

HOT_INLINE bool shouldSwap(
	const LabColor* src1,
	const LabColor* src2,
	const LabColor* dst1,
	const LabColor* dst2,
	const float lWeight,
	const float chromaWeight
) {
	const float swapCost = weightedPixelDiff( src1, dst2, lWeight, chromaWeight ) + weightedPixelDiff( src2, dst1, lWeight, chromaWeight );
	const float keepCost = weightedPixelDiff( src1, dst1, lWeight, chromaWeight ) + weightedPixelDiff( src2, dst2, lWeight, chromaWeight );
	return swapCost < keepCost;
}

float totalDiff( const Geometry& geometry, const LabBuffer& src, const LabBuffer& dst ) {
	float diff = 0.0f;
	for( size_t i = 0; i < geometry.pixelCount(); ++i ) {
		diff += weightedPixelDiff( &src.data()[i], &dst.data()[i], 1.0f, 1.0f );
	}
	return diff;
}

void printStageStatus( const char* phase, const char* stageName, const int iteration, const int swaps, const float diff ) {
#ifdef OUTPUT
	std::cout << phase
		<< " stage=" << stageName
		<< " iteration=" << iteration
		<< " swaps=" << swaps
		<< " diff=" << std::fixed << diff
		<< '\n';
#else
	( void ) phase;
	( void ) stageName;
	( void ) iteration;
	( void ) swaps;
	( void ) diff;
#endif
}

void writeFrameIfNeeded( FrameWriter& frames, const LabBuffer& working, const int iteration, const int every ) {
	if( every > 0 && ( iteration % every ) == 0 ) {
		frames.write( working );
	}
}

void runSearchStage(
	const Geometry& geometry,
	LabBuffer& working,
	const SearchTables& tables,
	const std::vector<const LabColor*>& targetRows,
	const SearchStage& stage,
	FrameWriter& frames,
	const LabBuffer& fineTarget
) {
	const int scale = std::max( 1, geometry.width / 320 );
	const int innerOrderedLoopCount = 300000 * scale * scale;

	for( int stepIndex = 0; stepIndex < stage.orderedLoops; ++stepIndex ) {
		int swaps = 0;
		uint32_t row1 = 0;
		uint32_t row2 = 0;
		uint32_t col1 = 0;
		uint32_t col2 = 0;
		const uint32_t* nextRows = tables.orderedRows.nextForStep( stepIndex % tables.orderedRows.stepCount );
		const uint32_t* carryRows = tables.orderedRows.carryForStep( stepIndex % tables.orderedRows.stepCount );
		const uint32_t* nextCols = tables.orderedCols.nextForStep( stepIndex % tables.orderedCols.stepCount );
		const uint32_t* carryCols = tables.orderedCols.carryForStep( stepIndex % tables.orderedCols.stepCount );

		for( int i = 0; i < innerOrderedLoopCount; ++i ) {
			LabColor* const srcRow1 = tables.srcRows[row1];
			LabColor* const srcRow2 = tables.srcRows[row2];
			const LabColor* const dstRow1 = targetRows[row1];
			const LabColor* const dstRow2 = targetRows[row2];

			LabColor* const srcPixel1 = srcRow1 + col1;
			LabColor* const srcPixel2 = srcRow2 + col2;
			const LabColor* const dstPixel1 = dstRow1 + col1;
			const LabColor* const dstPixel2 = dstRow2 + col2;

			if( shouldSwap( srcPixel1, srcPixel2, dstPixel1, dstPixel2, stage.lWeight, stage.chromaWeight ) ) {
				std::swap( *srcPixel1, *srcPixel2 );
				++swaps;
			}

			advanceOrderedState( nextRows, carryRows, static_cast<uint32_t>( geometry.height ), row1, row2 );
			advanceOrderedState( nextCols, carryCols, static_cast<uint32_t>( geometry.width ), col1, col2 );
		}

		printStageStatus( "ordered", stage.name, stepIndex, swaps, totalDiff( geometry, working, fineTarget ) );
		writeFrameIfNeeded( frames, working, stepIndex, stage.frameEvery );
	}

	XorShift64Star rng;
	for( int iteration = 0; iteration < stage.randomLoops; ++iteration ) {
		const int candidateCount = iteration * stage.randomStride;
		int swaps = 0;
		for( int i = 0; i < candidateCount; ++i ) {
			const uint64_t random = rng.next();
			const uint32_t y1 = tables.modRows[static_cast<uint32_t>( random & 0xFFFFULL )];
			const uint32_t y2 = tables.modRows[static_cast<uint32_t>( ( random >> 16 ) & 0xFFFFULL )];
			const uint32_t x1 = tables.modCols[static_cast<uint32_t>( ( random >> 32 ) & 0xFFFFULL )];
			const uint32_t x2 = tables.modCols[static_cast<uint32_t>( ( random >> 48 ) & 0xFFFFULL )];

			LabColor* const srcPixel1 = tables.srcRows[y1] + x1;
			LabColor* const srcPixel2 = tables.srcRows[y2] + x2;
			const LabColor* const dstPixel1 = targetRows[y1] + x1;
			const LabColor* const dstPixel2 = targetRows[y2] + x2;

			if( shouldSwap( srcPixel1, srcPixel2, dstPixel1, dstPixel2, stage.lWeight, stage.chromaWeight ) ) {
				std::swap( *srcPixel1, *srcPixel2 );
				++swaps;
			}
		}

		printStageStatus( "random", stage.name, iteration, swaps, totalDiff( geometry, working, fineTarget ) );
		writeFrameIfNeeded( frames, working, iteration, stage.frameEvery );
	}
}

int polishImage( const Geometry& geometry, LabBuffer& working, const LabBuffer& target ) {
	int swaps = 0;
	for( int y = 0; y < geometry.height; ++y ) {
		for( int x = 0; x + 1 < geometry.width; ++x ) {
			LabColor& lhs = working.row( y )[x];
			LabColor& rhs = working.row( y )[x + 1];
			const LabColor* targetL = &target.row( y )[x];
			const LabColor* targetR = &target.row( y )[x + 1];
			if( shouldSwap( &lhs, &rhs, targetL, targetR, 1.0f, 1.0f ) ) {
				std::swap( lhs, rhs );
				++swaps;
			}
		}
	}

	for( int x = 0; x < geometry.width; ++x ) {
		for( int y = 0; y + 1 < geometry.height; ++y ) {
			LabColor& top = working.row( y )[x];
			LabColor& bottom = working.row( y + 1 )[x];
			const LabColor* targetTop = &target.row( y )[x];
			const LabColor* targetBottom = &target.row( y + 1 )[x];
			if( shouldSwap( &top, &bottom, targetTop, targetBottom, 1.0f, 1.0f ) ) {
				std::swap( top, bottom );
				++swaps;
			}
		}
	}
	return swaps;
}

} // namespace

int main( int argc, char* argv[] ) {
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

	LabBuffer working = imageToLab( targetGeometry, outputRows.rows() );
	LabBuffer fineTarget = imageToLab( targetGeometry, targetRows.rows() );
	LabBuffer coarseTarget = boxBlur( targetGeometry, fineTarget, kStages[0].blurRadius );
	LabBuffer midTarget = boxBlur( targetGeometry, fineTarget, kStages[1].blurRadius );
	LabBuffer softFineTarget = boxBlur( targetGeometry, fineTarget, kStages[2].blurRadius );

	const std::vector<LabBuffer*> initTargets = { &coarseTarget, &coarseTarget, &midTarget, &midTarget, &softFineTarget, &fineTarget };
	FrameWriter frames( targetGeometry, outputRows.rows(), framePrefixFor( argv[3] ) );
	runGuidedInitialization( targetGeometry, working, initTargets, frames );

	const SearchTables tables( targetGeometry, working.data() );
	const std::vector<const LabColor*> coarseRows = buildTargetRows( targetGeometry, coarseTarget );
	const std::vector<const LabColor*> midRows = buildTargetRows( targetGeometry, midTarget );
	const std::vector<const LabColor*> softFineRows = buildTargetRows( targetGeometry, softFineTarget );
	const std::vector<const LabColor*> fineRows = buildTargetRows( targetGeometry, fineTarget );

	runSearchStage( targetGeometry, working, tables, coarseRows, kStages[0], frames, fineTarget );
	runSearchStage( targetGeometry, working, tables, midRows, kStages[1], frames, fineTarget );
	runSearchStage( targetGeometry, working, tables, softFineRows, kStages[2], frames, fineTarget );
	runSearchStage( targetGeometry, working, tables, fineRows, kStages[3], frames, fineTarget );

	for( int polishIteration = 0; polishIteration < 4; ++polishIteration ) {
		const int swaps = polishImage( targetGeometry, working, fineTarget );
#ifdef OUTPUT
		std::cout << "polish iteration=" << polishIteration
			<< " swaps=" << swaps
			<< " diff=" << std::fixed << totalDiff( targetGeometry, working, fineTarget )
			<< '\n';
#endif
		frames.write( working );
	}

	frames.labToImage( working, outputRows.rows() );
	writePNGFile( argv[3], outputRows.rows(), targetGeometry.width, targetGeometry.height );
	return 0;
}
