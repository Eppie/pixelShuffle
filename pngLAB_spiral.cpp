#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "fmath.hpp"
#include "pngReadWrite.h"
#include "profile_stats.h"

namespace {

struct Geometry {
	int width = 0;
	int height = 0;

	size_t pixelCount() const {
		return static_cast<size_t>( width ) * static_cast<size_t>( height );
	}

	size_t rgbaStrideBytes() const {
		return static_cast<size_t>( width ) * 4u;
	}
};

struct LabColor {
	float l;
	float a;
	float b;
};

struct OutputPaths {
	std::string finalPath;
	std::string framePrefix;
};

struct BlockCoord {
	int blockX;
	int blockY;
};

struct Projection {
	float l;
	float a;
	float b;
};

struct KeyedPosition {
	int index;
	float key;
};

struct KeyedColor {
	LabColor color;
	int ordinal;
	float key;
};

constexpr std::array<Projection, 6> kProjectionCycle = {{
	{ 1.0f, 0.0f, 0.0f },
	{ 0.0f, 1.0f, 0.35f },
	{ 0.15f, -0.45f, 1.0f },
	{ 0.57735026f, 0.57735026f, 0.57735026f },
	{ 0.75f, -0.50f, 0.35f },
	{ -0.25f, 0.85f, 0.45f },
}};

template <typename T>
T* allocateArray( const size_t count ) {
	T* ptr = static_cast<T*>( std::malloc( sizeof( T ) * count ) );
	if( ptr == nullptr ) {
		std::abort();
	}
	return ptr;
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

	png_bytep* rows() {
		return rows_;
	}

	png_bytep* rows() const {
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
		  pixels_( allocateArray<LabColor>( geometry.pixelCount() ) ) {
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

	LabColor* data() {
		return pixels_;
	}

	const LabColor* data() const {
		return pixels_;
	}

	LabColor& at( const int x, const int y ) {
		return pixels_[static_cast<size_t>( y ) * static_cast<size_t>( geometry_.width ) + static_cast<size_t>( x )];
	}

	const LabColor& at( const int x, const int y ) const {
		return pixels_[static_cast<size_t>( y ) * static_cast<size_t>( geometry_.width ) + static_cast<size_t>( x )];
	}

private:
	Geometry geometry_;
	LabColor* pixels_ = nullptr;
};

class FrameWriter {
public:
	FrameWriter( const Geometry& geometry, png_bytep* rows, const std::string& prefix )
		: geometry_( geometry ),
		  rows_( rows ),
		  prefix_( prefix ) {
	}

	void write( const LabBuffer& image ) {
#ifdef ANIMATION
		std::ostringstream name;
		name << prefix_ << std::setw( 5 ) << std::setfill( '0' ) << frameIndex_++ << ".png";
		labToImage( image );
		writePNGFile( name.str().c_str(), rows_, geometry_.width, geometry_.height );
#else
		( void ) image;
#endif
	}

	void labToImage( const LabBuffer& lab ) const;

	Geometry geometry_;
	png_bytep* rows_ = nullptr;
	std::string prefix_;
	int frameIndex_ = 0;
};

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

	return { r * 255.0f, g * 255.0f, b * 255.0f };
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
	};
}

inline LabColor labToXyz( const LabColor& px ) {
	float y = ( px.l + 16.0f ) / 116.0f;
	float x = px.a / 500.0f + y;
	float z = y - px.b / 200.0f;

	x = x * x * x > 0.008856f ? x * x * x : ( x - 16.0f / 116.0f ) / 7.787f;
	y = y * y * y > 0.008856f ? y * y * y : ( y - 16.0f / 116.0f ) / 7.787f;
	z = z * z * z > 0.008856f ? z * z * z : ( z - 16.0f / 116.0f ) / 7.787f;

	return { x * 95.047f, y * 100.0f, z * 108.883f };
}

inline LabColor rgbaToLab( png_bytep px ) {
	return xyzToLab( rgbToXyz( px ) );
}

LabBuffer imageToLab( const Geometry& geometry, png_bytep* image ) {
	LabBuffer result( geometry );
	for( int y = 0; y < geometry.height; ++y ) {
		png_bytep srcRow = image[y];
		for( int x = 0; x < geometry.width; ++x ) {
			result.at( x, y ) = rgbaToLab( &srcRow[x * 4] );
		}
	}
	return result;
}

void FrameWriter::labToImage( const LabBuffer& lab ) const {
	for( int y = 0; y < geometry_.height; ++y ) {
		png_bytep dstRow = rows_[y];
		for( int x = 0; x < geometry_.width; ++x ) {
			const LabColor rgb = xyzToRgb( labToXyz( lab.at( x, y ) ) );
			dstRow[( x * 4 ) + 0] = static_cast<int>( rgb.l + 0.5f );
			dstRow[( x * 4 ) + 1] = static_cast<int>( rgb.a + 0.5f );
			dstRow[( x * 4 ) + 2] = static_cast<int>( rgb.b + 0.5f );
		}
	}
}

float squaredLabDistance( const LabColor& lhs, const LabColor& rhs ) {
	const float dl = lhs.l - rhs.l;
	const float da = lhs.a - rhs.a;
	const float db = lhs.b - rhs.b;
	return ( dl * dl ) + ( da * da ) + ( db * db );
}

float totalDiff( const Geometry& geometry, const LabBuffer& working, const LabBuffer& target ) {
	float diff = 0.0f;
	for( int y = 0; y < geometry.height; ++y ) {
		for( int x = 0; x < geometry.width; ++x ) {
			diff += squaredLabDistance( working.at( x, y ), target.at( x, y ) );
		}
	}
	return diff;
}

float projectLab( const LabColor& color, const Projection& projection ) {
	return ( color.l * projection.l ) + ( color.a * projection.a ) + ( color.b * projection.b );
}

bool improvesSwap( const LabColor& left, const LabColor& right, const LabColor& leftTarget, const LabColor& rightTarget ) {
	const float keep = squaredLabDistance( left, leftTarget ) + squaredLabDistance( right, rightTarget );
	const float swap = squaredLabDistance( left, rightTarget ) + squaredLabDistance( right, leftTarget );
	return swap < keep;
}

std::vector<int> buildBlockSizes( const Geometry& geometry ) {
	static constexpr std::array<int, 6> kPreferred = { 64, 32, 16, 8, 4, 2 };
	std::vector<int> sizes;
	const int minDim = std::min( geometry.width, geometry.height );
	for( const int size : kPreferred ) {
		if( size <= minDim ) {
			sizes.push_back( size );
		}
	}
	if( sizes.empty() ) {
		sizes.push_back( 1 );
	}
	return sizes;
}

std::vector<BlockCoord> buildSpiralBlockOrder( const int gridWidth, const int gridHeight ) {
	std::vector<BlockCoord> order;
	order.reserve( static_cast<size_t>( gridWidth ) * static_cast<size_t>( gridHeight ) );

	int x = gridWidth / 2;
	int y = gridHeight / 2;
	int stepLength = 1;
	int direction = 0;

	auto maybePush = [&]( const int px, const int py ) {
		if( px >= 0 && px < gridWidth && py >= 0 && py < gridHeight ) {
			order.push_back( { px, py } );
		}
	};

	maybePush( x, y );

	while( order.size() < static_cast<size_t>( gridWidth ) * static_cast<size_t>( gridHeight ) ) {
		for( int repeat = 0; repeat < 2; ++repeat ) {
			for( int step = 0; step < stepLength; ++step ) {
				switch( direction ) {
					case 0: ++x; break;
					case 1: ++y; break;
					case 2: --x; break;
					default: --y; break;
				}
				maybePush( x, y );
			}
			direction = ( direction + 1 ) & 3;
		}
		++stepLength;
	}

	return order;
}

void sortBlockByProjection(
	const Geometry& geometry,
	LabBuffer& working,
	const LabBuffer& target,
	const int x0,
	const int y0,
	const int blockSize,
	const Projection& projection
) {
	const int x1 = std::min( geometry.width, x0 + blockSize );
	const int y1 = std::min( geometry.height, y0 + blockSize );
	const int blockPixelCount = ( x1 - x0 ) * ( y1 - y0 );

	std::vector<KeyedPosition> positions;
	std::vector<KeyedColor> colors;
	positions.reserve( static_cast<size_t>( blockPixelCount ) );
	colors.reserve( static_cast<size_t>( blockPixelCount ) );

	int ordinal = 0;
	for( int y = y0; y < y1; ++y ) {
		for( int x = x0; x < x1; ++x ) {
			const int index = y * geometry.width + x;
			positions.push_back( { index, projectLab( target.at( x, y ), projection ) } );
			colors.push_back( { working.at( x, y ), ordinal++, projectLab( working.at( x, y ), projection ) } );
		}
	}

	const auto comparePositions = []( const KeyedPosition& lhs, const KeyedPosition& rhs ) {
		return lhs.key < rhs.key || ( lhs.key == rhs.key && lhs.index < rhs.index );
	};
	const auto compareColors = []( const KeyedColor& lhs, const KeyedColor& rhs ) {
		return lhs.key < rhs.key || ( lhs.key == rhs.key && lhs.ordinal < rhs.ordinal );
	};

	std::stable_sort( positions.begin(), positions.end(), comparePositions );
	std::stable_sort( colors.begin(), colors.end(), compareColors );

	for( size_t i = 0; i < positions.size(); ++i ) {
		const int index = positions[i].index;
		const int y = index / geometry.width;
		const int x = index % geometry.width;
		working.at( x, y ) = colors[i].color;
	}
}

int polishHorizontal( const Geometry& geometry, LabBuffer& working, const LabBuffer& target ) {
	int swaps = 0;
	for( int y = 0; y < geometry.height; ++y ) {
		if( ( y & 1 ) == 0 ) {
			for( int x = 0; x + 1 < geometry.width; ++x ) {
				LabColor& left = working.at( x, y );
				LabColor& right = working.at( x + 1, y );
				if( improvesSwap( left, right, target.at( x, y ), target.at( x + 1, y ) ) ) {
					std::swap( left, right );
					++swaps;
				}
			}
		} else {
			for( int x = geometry.width - 2; x >= 0; --x ) {
				LabColor& left = working.at( x, y );
				LabColor& right = working.at( x + 1, y );
				if( improvesSwap( left, right, target.at( x, y ), target.at( x + 1, y ) ) ) {
					std::swap( left, right );
					++swaps;
				}
			}
		}
	}
	return swaps;
}

int polishVertical( const Geometry& geometry, LabBuffer& working, const LabBuffer& target ) {
	int swaps = 0;
	for( int x = 0; x < geometry.width; ++x ) {
		if( ( x & 1 ) == 0 ) {
			for( int y = 0; y + 1 < geometry.height; ++y ) {
				LabColor& top = working.at( x, y );
				LabColor& bottom = working.at( x, y + 1 );
				if( improvesSwap( top, bottom, target.at( x, y ), target.at( x, y + 1 ) ) ) {
					std::swap( top, bottom );
					++swaps;
				}
			}
		} else {
			for( int y = geometry.height - 2; y >= 0; --y ) {
				LabColor& top = working.at( x, y );
				LabColor& bottom = working.at( x, y + 1 );
				if( improvesSwap( top, bottom, target.at( x, y ), target.at( x, y + 1 ) ) ) {
					std::swap( top, bottom );
					++swaps;
				}
			}
		}
	}
	return swaps;
}

OutputPaths buildOutputPaths( const char* outputPath ) {
	const std::string path( outputPath );
	const size_t slash = path.find_last_of( '/' );
	const std::string dir = slash == std::string::npos ? std::string() : path.substr( 0, slash + 1 );
	const std::string name = slash == std::string::npos ? path : path.substr( slash + 1 );
	size_t dot = name.find_last_of( '.' );
	if( dot == std::string::npos ) {
		dot = name.size();
	}

	OutputPaths result;
	result.finalPath = path;
	result.framePrefix = dir + name.substr( 0, dot ) + ".spiral.";
	return result;
}

void printStageStatus( const char* label, const int blockSize, const int passIndex, const float diff ) {
#ifdef OUTPUT
	std::cout << label
		<< " block=" << blockSize
		<< " pass=" << passIndex
		<< " diff=" << std::fixed << diff
		<< '\n';
#else
	( void ) label;
	( void ) blockSize;
	( void ) passIndex;
	( void ) diff;
#endif
}

void runSpiralPatchSort( const Geometry& geometry, LabBuffer& working, const LabBuffer& target, FrameWriter& frames ) {
	const std::vector<int> blockSizes = buildBlockSizes( geometry );
	frames.write( working );

	for( size_t scaleIndex = 0; scaleIndex < blockSizes.size(); ++scaleIndex ) {
		const int blockSize = blockSizes[scaleIndex];
		const int gridWidth = ( geometry.width + blockSize - 1 ) / blockSize;
		const int gridHeight = ( geometry.height + blockSize - 1 ) / blockSize;
		const std::vector<BlockCoord> spiral = buildSpiralBlockOrder( gridWidth, gridHeight );
		const size_t blocksPerFrame = std::max<size_t>( 1, spiral.size() / 12u );

		for( int passIndex = 0; passIndex < 2; ++passIndex ) {
			const Projection& projection = kProjectionCycle[( scaleIndex * 2 + static_cast<size_t>( passIndex ) ) % kProjectionCycle.size()];
			size_t processed = 0;

			for( const BlockCoord block : spiral ) {
				sortBlockByProjection( geometry, working, target, block.blockX * blockSize, block.blockY * blockSize, blockSize, projection );
				++processed;
				if( processed % blocksPerFrame == 0 ) {
					frames.write( working );
				}
			}

			printStageStatus( "spiral-sort", blockSize, passIndex, totalDiff( geometry, working, target ) );
			frames.write( working );
		}

		const int horizontalSwaps = polishHorizontal( geometry, working, target );
		printStageStatus( "polish-h", blockSize, horizontalSwaps, totalDiff( geometry, working, target ) );
		frames.write( working );

		const int verticalSwaps = polishVertical( geometry, working, target );
		printStageStatus( "polish-v", blockSize, verticalSwaps, totalDiff( geometry, working, target ) );
		frames.write( working );
	}

	for( int settleIteration = 0; settleIteration < 6; ++settleIteration ) {
		const int horizontalSwaps = polishHorizontal( geometry, working, target );
		printStageStatus( "settle-h", settleIteration, horizontalSwaps, totalDiff( geometry, working, target ) );
		frames.write( working );

		const int verticalSwaps = polishVertical( geometry, working, target );
		printStageStatus( "settle-v", settleIteration, verticalSwaps, totalDiff( geometry, working, target ) );
		frames.write( working );
	}
}

void writeFinalImage( const Geometry& geometry, const LabBuffer& working, png_bytep* outputRows, const std::string& outputPath ) {
	FrameWriter writer( geometry, outputRows, std::string() );
	writer.labToImage( working );
	writePNGFile( outputPath.c_str(), outputRows, geometry.width, geometry.height );
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
	const LabBuffer target = imageToLab( targetGeometry, targetRows.rows() );
	const OutputPaths paths = buildOutputPaths( argv[3] );
	FrameWriter frames( targetGeometry, outputRows.rows(), paths.framePrefix );

	runSpiralPatchSort( targetGeometry, working, target, frames );
	writeFinalImage( targetGeometry, working, outputRows.rows(), paths.finalPath );
	return 0;
}
