#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "pngReadWrite.h"

namespace {

constexpr int kRevealFrameCount = 128;
constexpr int kPolishFrameCount = 32;
constexpr uint64_t kInitialRandomSeed = 0x4811BEA75EED1234ULL;

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
		image.contiguousBuffer_ = static_cast<png_bytep>( std::malloc( geometry.strideBytes() * static_cast<size_t>( geometry.height ) ) );
		image.rows_ = static_cast<png_bytep*>( std::malloc( sizeof( png_bytep ) * static_cast<size_t>( geometry.height ) ) );
		if( image.contiguousBuffer_ == nullptr || image.rows_ == nullptr ) {
			std::abort();
		}
		for( int y = 0; y < geometry.height; ++y ) {
			image.rows_[y] = image.contiguousBuffer_ + static_cast<size_t>( y ) * geometry.strideBytes();
		}
		return image;
	}

	png_bytep* rows() const {
		return rows_;
	}

	void setPixel( const size_t index, const Pixel& pixel ) {
		std::memcpy( contiguousBuffer_ + index * 4u, pixel.rgba.data(), 4 );
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

class XorShift64Star {
public:
	uint64_t next() {
		state_ ^= state_ >> 12;
		state_ ^= state_ << 25;
		state_ ^= state_ >> 27;
		return state_ * 2685821657736338717ULL;
	}

	size_t uniformIndex( const size_t limit ) {
		return static_cast<size_t>( next() % static_cast<uint64_t>( limit ) );
	}

	int uniformInt( const int limit ) {
		return static_cast<int>( next() % static_cast<uint64_t>( limit ) );
	}

	double unit() {
		return static_cast<double>( next() >> 11 ) * ( 1.0 / 9007199254740992.0 );
	}

private:
	uint64_t state_ = kInitialRandomSeed;
};

float srgbToLinear( const float value ) {
	return value > 0.04045f ? std::pow( ( value + 0.055f ) / 1.055f, 2.4f ) : value / 12.92f;
}

LabColor rgbaToLab( const png_bytep px ) {
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

uint32_t quantize( const float value, const float low, const float high ) {
	const float normalized = std::clamp( ( value - low ) / ( high - low ), 0.0f, 1.0f );
	return static_cast<uint32_t>( normalized * 1023.0f + 0.5f );
}

uint64_t colorKey( const LabColor& color ) {
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

bool labLess( const LabColor& lhs, const LabColor& rhs ) {
	if( lhs.l != rhs.l ) {
		return lhs.l < rhs.l;
	}
	if( lhs.a != rhs.a ) {
		return lhs.a < rhs.a;
	}
	return lhs.b < rhs.b;
}

std::vector<Pixel> readPalettePixels( const OwnedRows& rows, const Geometry& geometry ) {
	std::vector<Pixel> pixels;
	pixels.reserve( geometry.pixelCount() );
	for( int y = 0; y < geometry.height; ++y ) {
		const png_bytep row = rows.rows()[y];
		for( int x = 0; x < geometry.width; ++x ) {
			Pixel pixel;
			std::memcpy( pixel.rgba.data(), &row[x * 4], 4 );
			pixel.lab = rgbaToLab( &row[x * 4] );
			pixel.key = colorKey( pixel.lab );
			pixels.push_back( pixel );
		}
	}
	return pixels;
}

std::vector<LabColor> imageToLab( const Geometry& geometry, png_bytep* rows ) {
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

float squaredLabDistance( const LabColor& lhs, const LabColor& rhs ) {
	const float dl = lhs.l - rhs.l;
	const float da = lhs.a - rhs.a;
	const float db = lhs.b - rhs.b;
	return ( dl * dl ) + ( da * da ) + ( db * db );
}

float totalCost( const std::vector<size_t>& assignment, const std::vector<Pixel>& pixels, const std::vector<LabColor>& target ) {
	float cost = 0.0f;
	for( size_t i = 0; i < assignment.size(); ++i ) {
		cost += squaredLabDistance( pixels[assignment[i]].lab, target[i] );
	}
	return cost;
}

uint32_t nextPowerOfTwo( const uint32_t value ) {
	uint32_t result = 1;
	while( result < value ) {
		result <<= 1u;
	}
	return result;
}

void hilbertRotate( const uint32_t n, uint32_t& x, uint32_t& y, const uint32_t rx, const uint32_t ry ) {
	if( ry == 0 ) {
		if( rx == 1 ) {
			x = n - 1u - x;
			y = n - 1u - y;
		}
		std::swap( x, y );
	}
}

uint64_t hilbertIndex( const uint32_t n, uint32_t x, uint32_t y ) {
	uint64_t d = 0;
	for( uint32_t s = n / 2u; s > 0; s /= 2u ) {
		const uint32_t rx = ( x & s ) ? 1u : 0u;
		const uint32_t ry = ( y & s ) ? 1u : 0u;
		d += static_cast<uint64_t>( s ) * static_cast<uint64_t>( s ) * static_cast<uint64_t>( ( 3u * rx ) ^ ry );
		hilbertRotate( s, x, y, rx, ry );
	}
	return d;
}

std::vector<size_t> hilbertOrderFor( const Geometry& geometry ) {
	const uint32_t side = nextPowerOfTwo( static_cast<uint32_t>( std::max( geometry.width, geometry.height ) ) );
	std::vector<size_t> order( geometry.pixelCount() );
	std::iota( order.begin(), order.end(), 0u );
	std::sort( order.begin(), order.end(), [&]( const size_t lhs, const size_t rhs ) {
		const uint32_t lx = static_cast<uint32_t>( lhs % static_cast<size_t>( geometry.width ) );
		const uint32_t ly = static_cast<uint32_t>( lhs / static_cast<size_t>( geometry.width ) );
		const uint32_t rx = static_cast<uint32_t>( rhs % static_cast<size_t>( geometry.width ) );
		const uint32_t ry = static_cast<uint32_t>( rhs / static_cast<size_t>( geometry.width ) );
		return hilbertIndex( side, lx, ly ) < hilbertIndex( side, rx, ry );
	} );
	return order;
}

struct Segment {
	size_t begin = 0;
	size_t end = 0;
	LabColor mean;
	uint64_t key = 0;
};

LabColor meanTargetForSegment( const std::vector<size_t>& curveOrder, const std::vector<LabColor>& target, const size_t begin, const size_t end ) {
	double l = 0.0;
	double a = 0.0;
	double b = 0.0;
	for( size_t i = begin; i < end; ++i ) {
		const LabColor& color = target[curveOrder[i]];
		l += color.l;
		a += color.a;
		b += color.b;
	}
	const double invCount = 1.0 / static_cast<double>( end - begin );
	return {
		static_cast<float>( l * invCount ),
		static_cast<float>( a * invCount ),
		static_cast<float>( b * invCount )
	};
}

std::vector<Segment> makeSegments( const std::vector<size_t>& curveOrder, const std::vector<LabColor>& target, const size_t segmentSize ) {
	std::vector<Segment> segments;
	segments.reserve( ( curveOrder.size() + segmentSize - 1u ) / segmentSize );
	for( size_t begin = 0; begin < curveOrder.size(); begin += segmentSize ) {
		const size_t end = std::min( curveOrder.size(), begin + segmentSize );
		Segment segment;
		segment.begin = begin;
		segment.end = end;
		segment.mean = meanTargetForSegment( curveOrder, target, begin, end );
		segment.key = colorKey( segment.mean );
		segments.push_back( segment );
	}
	return segments;
}

std::vector<size_t> segmentSizesFor( const Geometry& geometry ) {
	const size_t count = geometry.pixelCount();
	const size_t rawSizes[] = {
		std::max<size_t>( 2u, count / 4u ),
		std::max<size_t>( 2u, count / 8u ),
		std::max<size_t>( 2u, count / 16u ),
		std::max<size_t>( 2u, count / 32u ),
		std::max<size_t>( 2u, count / 64u ),
		std::max<size_t>( 2u, count / 128u ),
		std::max<size_t>( 2u, count / 256u ),
		std::max<size_t>( 2u, count / 512u ),
		std::max<size_t>( 2u, count / 1024u ),
		std::max<size_t>( 2u, count / 2048u ),
		std::max<size_t>( 2u, count / 4096u ),
		std::max<size_t>( 2u, count / 8192u ),
		1u
	};

	std::vector<size_t> result;
	for( const size_t size : rawSizes ) {
		if( result.empty() || result.back() != size ) {
			result.push_back( size );
		}
	}
	if( result.back() != 1u ) {
		result.push_back( 1u );
	}
	return result;
}

std::vector<int> framesPerStageFor( const size_t stageCount, const int totalFrames ) {
	std::vector<int> frames( stageCount, 0 );
	if( stageCount == 0 ) {
		return frames;
	}
	const int baseFrames = totalFrames / static_cast<int>( stageCount );
	int extraFrames = totalFrames % static_cast<int>( stageCount );
	for( size_t i = 0; i < stageCount; ++i ) {
		frames[i] = baseFrames;
		if( extraFrames > 0 ) {
			++frames[i];
			--extraFrames;
		}
	}
	return frames;
}

std::string animationPrefixFor( const char* outputPath ) {
	const std::string path( outputPath );
	const size_t slash = path.find_last_of( '/' );
	const std::string dir = slash == std::string::npos ? std::string() : path.substr( 0, slash + 1 );
	const std::string name = slash == std::string::npos ? path : path.substr( slash + 1 );
	size_t dot = name.find_last_of( '.' );
	if( dot == std::string::npos ) {
		dot = name.size();
	}
	return dir + "out" + name.substr( 0, dot );
}

class FrameWriter {
public:
	FrameWriter( const Geometry& geometry, png_bytep* rows, std::string prefix )
		: geometry_( geometry ),
		  rows_( rows ),
		  prefix_( std::move( prefix ) ) {
	}

	void write() {
#ifdef ANIMATION
		std::ostringstream name;
		name << prefix_ << std::setw( 5 ) << std::setfill( '0' ) << frameIndex_++ << ".png";
		writePNGFile( name.str().c_str(), rows_, geometry_.width, geometry_.height );
#endif
	}

private:
	Geometry geometry_;
	png_bytep* rows_ = nullptr;
	std::string prefix_;
	int frameIndex_ = 0;
};

std::vector<size_t> sortedPaletteIds( const std::vector<Pixel>& pixels ) {
	std::vector<size_t> ids( pixels.size() );
	std::iota( ids.begin(), ids.end(), 0u );
	std::sort( ids.begin(), ids.end(), [&]( const size_t lhs, const size_t rhs ) {
		if( pixels[lhs].key != pixels[rhs].key ) {
			return pixels[lhs].key < pixels[rhs].key;
		}
		return labLess( pixels[lhs].lab, pixels[rhs].lab );
	} );
	return ids;
}

std::vector<size_t> buildStageAssignment(
	const std::vector<size_t>& curveOrder,
	const std::vector<Segment>& segments,
	const std::vector<size_t>& paletteByColor,
	const std::vector<LabColor>& target,
	const std::vector<uint64_t>& targetKeys
) {
	std::vector<size_t> desired( curveOrder.size() );
	std::vector<Segment> segmentsByColor = segments;
	std::sort( segmentsByColor.begin(), segmentsByColor.end(), []( const Segment& lhs, const Segment& rhs ) {
		if( lhs.key != rhs.key ) {
			return lhs.key < rhs.key;
		}
		return labLess( lhs.mean, rhs.mean );
	} );

	size_t cursor = 0;
	for( const Segment& segment : segmentsByColor ) {
		std::vector<size_t> positions;
		positions.reserve( segment.end - segment.begin );
		for( size_t i = segment.begin; i < segment.end; ++i ) {
			positions.push_back( curveOrder[i] );
		}
		std::sort( positions.begin(), positions.end(), [&]( const size_t lhs, const size_t rhs ) {
			if( targetKeys[lhs] != targetKeys[rhs] ) {
				return targetKeys[lhs] < targetKeys[rhs];
			}
			return labLess( target[lhs], target[rhs] );
		} );

		for( size_t i = 0; i < positions.size(); ++i ) {
			desired[positions[i]] = paletteByColor[cursor + i];
		}
		cursor += positions.size();
	}

	return desired;
}

void writeAssignmentToRows( const std::vector<size_t>& assignment, const std::vector<Pixel>& pixels, OwnedRows& outputRows ) {
	for( size_t i = 0; i < assignment.size(); ++i ) {
		outputRows.setPixel( i, pixels[assignment[i]] );
	}
}

void applyStage(
	const std::vector<size_t>& curveOrder,
	const std::vector<size_t>& desired,
	std::vector<size_t>& current,
	const std::vector<Pixel>& pixels,
	OwnedRows& outputRows,
	FrameWriter& frames,
	const int framesToWrite
) {
	const uint64_t totalUpdates = static_cast<uint64_t>( curveOrder.size() );
	uint64_t updates = 0;
	int writtenFrames = 0;
	uint64_t nextFrameAt = framesToWrite > 0
		? std::max<uint64_t>( 1, ( totalUpdates + static_cast<uint64_t>( framesToWrite ) - 1u ) / static_cast<uint64_t>( framesToWrite ) )
		: totalUpdates + 1u;

	for( const size_t index : curveOrder ) {
		current[index] = desired[index];
		outputRows.setPixel( index, pixels[current[index]] );
		++updates;

		while( writtenFrames < framesToWrite && updates >= nextFrameAt ) {
			frames.write();
			++writtenFrames;
			const uint64_t frameNumber = static_cast<uint64_t>( writtenFrames + 1 );
			nextFrameAt = ( totalUpdates * frameNumber + static_cast<uint64_t>( framesToWrite ) - 1u ) / static_cast<uint64_t>( framesToWrite );
			nextFrameAt = std::max<uint64_t>( updates + 1u, nextFrameAt );
		}
	}
}

size_t pickPartner( const size_t index, const Geometry& geometry, const int window, const double globalChance, XorShift64Star& rng ) {
	const size_t count = geometry.pixelCount();
	if( window >= std::max( geometry.width, geometry.height ) || rng.unit() < globalChance ) {
		size_t partner = rng.uniformIndex( count );
		if( partner == index ) {
			partner = ( partner + 1u ) % count;
		}
		return partner;
	}

	const int x = static_cast<int>( index % static_cast<size_t>( geometry.width ) );
	const int y = static_cast<int>( index / static_cast<size_t>( geometry.width ) );
	const int span = ( window * 2 ) + 1;
	const int dx = rng.uniformInt( span ) - window;
	const int dy = rng.uniformInt( span ) - window;
	const int nx = std::clamp( x + dx, 0, geometry.width - 1 );
	const int ny = std::clamp( y + dy, 0, geometry.height - 1 );
	size_t partner = static_cast<size_t>( ny ) * static_cast<size_t>( geometry.width ) + static_cast<size_t>( nx );
	if( partner == index ) {
		partner = ( partner + 1u ) % count;
	}
	return partner;
}

struct PolishResult {
	uint64_t accepted = 0;
	uint64_t improved = 0;
	uint64_t uphill = 0;
};

PolishResult runPolishPass(
	const Geometry& geometry,
	std::vector<size_t>& current,
	const std::vector<Pixel>& pixels,
	const std::vector<LabColor>& target,
	OwnedRows& outputRows,
	const uint64_t candidates,
	const int window,
	const double globalChance,
	const double startTemperature,
	const double endTemperature,
	const int framesToWrite,
	XorShift64Star& rng,
	FrameWriter& frames
) {
	PolishResult result;
	const size_t count = geometry.pixelCount();
	int writtenFrames = 0;
	uint64_t nextFrameAt = framesToWrite > 0
		? std::max<uint64_t>( 1, ( candidates + static_cast<uint64_t>( framesToWrite ) - 1u ) / static_cast<uint64_t>( framesToWrite ) )
		: candidates + 1u;

	for( uint64_t iter = 1; iter <= candidates; ++iter ) {
		const size_t first = rng.uniformIndex( count );
		const size_t second = pickPartner( first, geometry, window, globalChance, rng );
		const size_t firstPixel = current[first];
		const size_t secondPixel = current[second];

		const float keepCost = squaredLabDistance( pixels[firstPixel].lab, target[first] ) + squaredLabDistance( pixels[secondPixel].lab, target[second] );
		const float swapCost = squaredLabDistance( pixels[firstPixel].lab, target[second] ) + squaredLabDistance( pixels[secondPixel].lab, target[first] );
		const double delta = static_cast<double>( swapCost - keepCost );

		const double progress = static_cast<double>( iter ) / static_cast<double>( candidates );
		const double temperature = startTemperature + ( ( endTemperature - startTemperature ) * progress );
		bool accept = delta < 0.0;
		bool uphill = false;
		if( !accept && temperature > 0.0 && delta / temperature < 60.0 ) {
			accept = rng.unit() < std::exp( -delta / temperature );
			uphill = accept;
		}

		if( accept ) {
			std::swap( current[first], current[second] );
			outputRows.setPixel( first, pixels[current[first]] );
			outputRows.setPixel( second, pixels[current[second]] );
			++result.accepted;
			if( uphill ) {
				++result.uphill;
			} else {
				++result.improved;
			}
		}

		while( writtenFrames < framesToWrite && iter >= nextFrameAt ) {
			frames.write();
			++writtenFrames;
			const uint64_t frameNumber = static_cast<uint64_t>( writtenFrames + 1 );
			nextFrameAt = ( candidates * frameNumber + static_cast<uint64_t>( framesToWrite ) - 1u ) / static_cast<uint64_t>( framesToWrite );
			nextFrameAt = std::max<uint64_t>( iter + 1u, nextFrameAt );
		}
	}

	return result;
}

std::vector<int> polishFramesPerPass() {
	constexpr int passCount = 4;
	std::vector<int> frames( passCount, kPolishFrameCount / passCount );
	int extraFrames = kPolishFrameCount % passCount;
	for( int& value : frames ) {
		if( extraFrames > 0 ) {
			++value;
			--extraFrames;
		}
	}
	return frames;
}

} // namespace

int main( int argc, char* argv[] ) {
	if( argc != 4 ) {
		std::cout << "Usage: " << argv[0] << " <palette image> <target image> <output image>" << std::endl;
		return 1;
	}

	Geometry paletteGeometry;
	Geometry targetGeometry;
	OwnedRows paletteRows = OwnedRows::read( argv[1], paletteGeometry );
	OwnedRows targetRows = OwnedRows::read( argv[2], targetGeometry );

	if( paletteGeometry.pixelCount() != targetGeometry.pixelCount() ) {
		std::cerr << "Palette and target must contain the same number of pixels. Got "
			<< paletteGeometry.width << "x" << paletteGeometry.height
			<< " vs "
			<< targetGeometry.width << "x" << targetGeometry.height
			<< "." << std::endl;
		return 1;
	}

	const std::vector<Pixel> pixels = readPalettePixels( paletteRows, paletteGeometry );
	const std::vector<size_t> paletteByColor = sortedPaletteIds( pixels );
	const std::vector<LabColor> targetLab = imageToLab( targetGeometry, targetRows.rows() );
	std::vector<uint64_t> targetKeys( targetLab.size() );
	for( size_t i = 0; i < targetLab.size(); ++i ) {
		targetKeys[i] = colorKey( targetLab[i] );
	}

	const std::vector<size_t> curveOrder = hilbertOrderFor( targetGeometry );
	std::vector<size_t> current( targetGeometry.pixelCount() );
	std::iota( current.begin(), current.end(), 0u );

	OwnedRows outputRows = OwnedRows::allocateContiguous( targetGeometry );
	writeAssignmentToRows( current, pixels, outputRows );

	const std::vector<size_t> segmentSizes = segmentSizesFor( targetGeometry );
	const std::vector<int> stageFrames = framesPerStageFor( segmentSizes.size(), kRevealFrameCount - 1 );
	FrameWriter frames( targetGeometry, outputRows.rows(), animationPrefixFor( argv[3] ) );
	frames.write();

	for( size_t stageIndex = 0; stageIndex < segmentSizes.size(); ++stageIndex ) {
		const size_t segmentSize = segmentSizes[stageIndex];
		const std::vector<Segment> segments = makeSegments( curveOrder, targetLab, segmentSize );
		const std::vector<size_t> desired = buildStageAssignment(
			curveOrder,
			segments,
			paletteByColor,
			targetLab,
			targetKeys
		);

		std::cout << "Stage " << ( stageIndex + 1 ) << "/" << segmentSizes.size()
			<< ": segment_size=" << segmentSize
			<< ", segments=" << segments.size()
			<< ", frames=" << stageFrames[stageIndex]
			<< std::endl;

		applyStage(
			curveOrder,
			desired,
			current,
			pixels,
			outputRows,
			frames,
			stageFrames[stageIndex]
		);

		std::cout << "  sharp_cost=" << std::fixed << std::setprecision( 2 )
			<< totalCost( current, pixels, targetLab )
			<< std::endl;
	}

	const size_t count = targetGeometry.pixelCount();
	const uint64_t baseCandidates = std::max<uint64_t>( 300000u, static_cast<uint64_t>( count ) * 8u );
	const std::vector<int> polishFrames = polishFramesPerPass();
	XorShift64Star rng;
	const int maxDimension = std::max( targetGeometry.width, targetGeometry.height );
	const int windows[] = { maxDimension, 32, 10, 4 };
	const double globalChances[] = { 1.0, 0.14, 0.05, 0.01 };
	const double startTemperatures[] = { 32.0, 8.0, 1.5, 0.0 };
	const double endTemperatures[] = { 3.0, 0.5, 0.04, 0.0 };
	const uint64_t multipliers[] = { 2u, 2u, 2u, 1u };
	for( size_t pass = 0; pass < polishFrames.size(); ++pass ) {
		const uint64_t candidates = baseCandidates * multipliers[pass];
		std::cout << "Polish " << ( pass + 1 ) << "/" << polishFrames.size()
			<< ": candidates=" << candidates
			<< ", window=" << windows[pass]
			<< ", frames=" << polishFrames[pass]
			<< ", temp=" << std::fixed << std::setprecision( 2 )
			<< startTemperatures[pass] << "->" << endTemperatures[pass]
			<< std::endl;
		const PolishResult result = runPolishPass(
			targetGeometry,
			current,
			pixels,
			targetLab,
			outputRows,
			candidates,
			windows[pass],
			globalChances[pass],
			startTemperatures[pass],
			endTemperatures[pass],
			polishFrames[pass],
			rng,
			frames
		);
		std::cout << "  accepted=" << result.accepted
			<< " improved=" << result.improved
			<< " uphill=" << result.uphill
			<< " sharp_cost=" << std::fixed << std::setprecision( 2 )
			<< totalCost( current, pixels, targetLab )
			<< std::endl;
	}

	writePNGFile( argv[3], outputRows.rows(), targetGeometry.width, targetGeometry.height );
	std::cout << "Final image written to " << argv[3] << std::endl;
	return 0;
}
