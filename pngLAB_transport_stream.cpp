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
#include "lab_grid.h"

namespace {

constexpr int kFrameCount = 192;
constexpr uint64_t kInitialRandomSeed = 0x71A15C0FFEE5EEDULL;

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

	void setRgb( const size_t index, const float r, const float g, const float b ) {
		png_bytep dst = contiguousBuffer_ + index * 4u;
		dst[0] = static_cast<png_byte>( std::clamp( r, 0.0f, 255.0f ) + 0.5f );
		dst[1] = static_cast<png_byte>( std::clamp( g, 0.0f, 255.0f ) + 0.5f );
		dst[2] = static_cast<png_byte>( std::clamp( b, 0.0f, 255.0f ) + 0.5f );
		dst[3] = 255;
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

float totalCost( const std::vector<size_t>& assignment, const std::vector<Pixel>& pixels, const std::vector<LabColor>& target ) {
	float cost = 0.0f;
	for( size_t i = 0; i < assignment.size(); ++i ) {
		cost += squaredLabDistance( pixels[assignment[i]].lab, target[i] );
	}
	return cost;
}

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

std::vector<size_t> sortedTargetPositions( const std::vector<LabColor>& target ) {
	std::vector<size_t> ids( target.size() );
	std::iota( ids.begin(), ids.end(), 0u );
	std::sort( ids.begin(), ids.end(), [&]( const size_t lhs, const size_t rhs ) {
		const uint64_t leftKey = colorKey( target[lhs] );
		const uint64_t rightKey = colorKey( target[rhs] );
		if( leftKey != rightKey ) {
			return leftKey < rightKey;
		}
		return labLess( target[lhs], target[rhs] );
	} );
	return ids;
}

std::vector<size_t> buildInitialAssignment( const std::vector<Pixel>& pixels, const std::vector<LabColor>& target ) {
	const std::vector<size_t> paletteByColor = sortedPaletteIds( pixels );
	const std::vector<size_t> targetByColor = sortedTargetPositions( target );
	std::vector<size_t> assignment( target.size() );
	for( size_t i = 0; i < targetByColor.size(); ++i ) {
		assignment[targetByColor[i]] = paletteByColor[i];
	}
	return assignment;
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
	std::vector<size_t>& assignment,
	const std::vector<Pixel>& pixels,
	const std::vector<LabColor>& target,
	const uint64_t candidates,
	const int window,
	const double globalChance,
	const double startTemperature,
	const double endTemperature,
	XorShift64Star& rng
) {
	PolishResult result;
	const size_t count = geometry.pixelCount();
	for( uint64_t iter = 1; iter <= candidates; ++iter ) {
		const size_t first = rng.uniformIndex( count );
		const size_t second = pickPartner( first, geometry, window, globalChance, rng );
		const size_t firstPixel = assignment[first];
		const size_t secondPixel = assignment[second];

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
			std::swap( assignment[first], assignment[second] );
			++result.accepted;
			if( uphill ) {
				++result.uphill;
			} else {
				++result.improved;
			}
		}
	}
	return result;
}

void polishAssignment( const Geometry& geometry, std::vector<size_t>& assignment, const std::vector<Pixel>& pixels, const std::vector<LabColor>& target ) {
	const size_t count = geometry.pixelCount();
	const uint64_t baseCandidates = std::max<uint64_t>( 250000u, static_cast<uint64_t>( count ) * 6u );
	const int windows[] = { std::max( geometry.width, geometry.height ), 28, 9, 4 };
	const double globalChances[] = { 1.0, 0.12, 0.04, 0.01 };
	const double startTemperatures[] = { 26.0, 6.0, 1.0, 0.0 };
	const double endTemperatures[] = { 2.0, 0.35, 0.03, 0.0 };
	const uint64_t multipliers[] = { 2u, 2u, 2u, 1u };
	XorShift64Star rng;

	for( size_t pass = 0; pass < 4u; ++pass ) {
		const uint64_t candidates = baseCandidates * multipliers[pass];
		std::cout << "Assignment polish " << ( pass + 1 ) << "/4"
			<< ": candidates=" << candidates
			<< ", window=" << windows[pass]
			<< ", temp=" << std::fixed << std::setprecision( 2 ) << startTemperatures[pass]
			<< "->" << endTemperatures[pass]
			<< std::endl;
		const PolishResult result = runPolishPass(
			geometry,
			assignment,
			pixels,
			target,
			candidates,
			windows[pass],
			globalChances[pass],
			startTemperatures[pass],
			endTemperatures[pass],
			rng
		);
		std::cout << "  accepted=" << result.accepted
			<< " improved=" << result.improved
			<< " uphill=" << result.uphill
			<< " sharp_cost=" << std::fixed << std::setprecision( 2 )
			<< totalCost( assignment, pixels, target )
			<< std::endl;
	}
}

void writeAssignmentToRows( const std::vector<size_t>& assignment, const std::vector<Pixel>& pixels, OwnedRows& outputRows ) {
	for( size_t i = 0; i < assignment.size(); ++i ) {
		outputRows.setPixel( i, pixels[assignment[i]] );
	}
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

float smoothstep( const float value ) {
	const float x = std::clamp( value, 0.0f, 1.0f );
	return x * x * ( 3.0f - ( 2.0f * x ) );
}

float hashUnit( const size_t value ) {
	uint64_t x = static_cast<uint64_t>( value ) + 0x9E3779B97F4A7C15ULL;
	x ^= x >> 30;
	x *= 0xBF58476D1CE4E5B9ULL;
	x ^= x >> 27;
	x *= 0x94D049BB133111EBULL;
	x ^= x >> 31;
	return static_cast<float>( x & 0xFFFFFFu ) / static_cast<float>( 0x1000000u );
}

float interpolate( const float a, const float b, const float t ) {
	return a + ( b - a ) * t;
}

int colorTransportBin( const LabColor& lab ) {
	const int lBin = std::clamp( static_cast<int>( lab.l / 25.0f ), 0, 3 );
	const int aBin = std::clamp( static_cast<int>( ( lab.a + 128.0f ) / 64.0f ), 0, 3 );
	const int bBin = std::clamp( static_cast<int>( ( lab.b + 128.0f ) / 64.0f ), 0, 3 );
	return ( lBin * 16 ) + ( aBin * 4 ) + bBin;
}

struct TransportBin {
	float sourceX = 0.0f;
	float sourceY = 0.0f;
	float targetX = 0.0f;
	float targetY = 0.0f;
	float spread = 1.0f;
	float phase = 0.0f;
	uint32_t count = 0;
};

std::vector<TransportBin> buildTransportBins(
	const Geometry& geometry,
	const std::vector<Pixel>& pixels,
	const std::vector<size_t>& destinationForPixel
) {
	constexpr int kBinCount = 64;
	std::vector<double> sourceX( kBinCount, 0.0 );
	std::vector<double> sourceY( kBinCount, 0.0 );
	std::vector<double> targetX( kBinCount, 0.0 );
	std::vector<double> targetY( kBinCount, 0.0 );
	std::vector<double> sourceRadius( kBinCount, 0.0 );
	std::vector<double> targetRadius( kBinCount, 0.0 );
	std::vector<TransportBin> bins( kBinCount );

	for( size_t pixelId = 0; pixelId < pixels.size(); ++pixelId ) {
		const int bin = colorTransportBin( pixels[pixelId].lab );
		const size_t dstIndex = destinationForPixel[pixelId];
		const float sx = static_cast<float>( pixelId % static_cast<size_t>( geometry.width ) );
		const float sy = static_cast<float>( pixelId / static_cast<size_t>( geometry.width ) );
		const float dx = static_cast<float>( dstIndex % static_cast<size_t>( geometry.width ) );
		const float dy = static_cast<float>( dstIndex / static_cast<size_t>( geometry.width ) );
		sourceX[bin] += sx;
		sourceY[bin] += sy;
		targetX[bin] += dx;
		targetY[bin] += dy;
		++bins[bin].count;
	}

	const float cx = ( static_cast<float>( geometry.width ) - 1.0f ) * 0.5f;
	const float cy = ( static_cast<float>( geometry.height ) - 1.0f ) * 0.5f;
	for( size_t bin = 0; bin < bins.size(); ++bin ) {
		if( bins[bin].count == 0u ) {
			bins[bin].sourceX = cx;
			bins[bin].sourceY = cy;
			bins[bin].targetX = cx;
			bins[bin].targetY = cy;
			bins[bin].phase = hashUnit( bin );
			continue;
		}
		const double invCount = 1.0 / static_cast<double>( bins[bin].count );
		bins[bin].sourceX = static_cast<float>( sourceX[bin] * invCount );
		bins[bin].sourceY = static_cast<float>( sourceY[bin] * invCount );
		bins[bin].targetX = static_cast<float>( targetX[bin] * invCount );
		bins[bin].targetY = static_cast<float>( targetY[bin] * invCount );
	}

	for( size_t pixelId = 0; pixelId < pixels.size(); ++pixelId ) {
		const int bin = colorTransportBin( pixels[pixelId].lab );
		const size_t dstIndex = destinationForPixel[pixelId];
		const float sx = static_cast<float>( pixelId % static_cast<size_t>( geometry.width ) );
		const float sy = static_cast<float>( pixelId / static_cast<size_t>( geometry.width ) );
		const float dx = static_cast<float>( dstIndex % static_cast<size_t>( geometry.width ) );
		const float dy = static_cast<float>( dstIndex / static_cast<size_t>( geometry.width ) );
		const float sourceDx = sx - bins[bin].sourceX;
		const float sourceDy = sy - bins[bin].sourceY;
		const float targetDx = dx - bins[bin].targetX;
		const float targetDy = dy - bins[bin].targetY;
		sourceRadius[bin] += std::sqrt( sourceDx * sourceDx + sourceDy * sourceDy );
		targetRadius[bin] += std::sqrt( targetDx * targetDx + targetDy * targetDy );
	}

	for( size_t bin = 0; bin < bins.size(); ++bin ) {
		if( bins[bin].count == 0u ) {
			continue;
		}
		const double invCount = 1.0 / static_cast<double>( bins[bin].count );
		bins[bin].spread = static_cast<float>( 0.5 * ( sourceRadius[bin] + targetRadius[bin] ) * invCount );
		const int lBin = static_cast<int>( bin / 16u );
		const int chromaBin = static_cast<int>( bin % 16u );
		const float lightPhase = static_cast<float>( lBin ) / 3.0f;
		const float chromaPhase = static_cast<float>( chromaBin ) / 15.0f;
		bins[bin].phase = std::clamp( 0.58f * lightPhase + 0.27f * chromaPhase + 0.15f * hashUnit( bin ), 0.0f, 1.0f );
	}
	return bins;
}

void splat(
	const Geometry& geometry,
	const Pixel& pixel,
	const float x,
	const float y,
	const float opacity,
	std::vector<float>& red,
	std::vector<float>& green,
	std::vector<float>& blue,
	std::vector<float>& weight
) {
	const int x0 = static_cast<int>( std::floor( x ) );
	const int y0 = static_cast<int>( std::floor( y ) );
	const float fx = x - static_cast<float>( x0 );
	const float fy = y - static_cast<float>( y0 );

	for( int yy = 0; yy <= 1; ++yy ) {
		const int py = y0 + yy;
		if( py < 0 || py >= geometry.height ) {
			continue;
		}
		const float wy = yy == 0 ? 1.0f - fy : fy;
		for( int xx = 0; xx <= 1; ++xx ) {
			const int px = x0 + xx;
			if( px < 0 || px >= geometry.width ) {
				continue;
			}
			const float wx = xx == 0 ? 1.0f - fx : fx;
			const float w = std::max( 0.001f, wx * wy ) * opacity;
			const size_t index = static_cast<size_t>( py ) * static_cast<size_t>( geometry.width ) + static_cast<size_t>( px );
			red[index] += static_cast<float>( pixel.rgba[0] ) * w;
			green[index] += static_cast<float>( pixel.rgba[1] ) * w;
			blue[index] += static_cast<float>( pixel.rgba[2] ) * w;
			weight[index] += w;
		}
	}
}

void cubicPoint(
	const float ax,
	const float ay,
	const float bx,
	const float by,
	const float cx,
	const float cy,
	const float dx,
	const float dy,
	const float t,
	float& x,
	float& y
) {
	const float oneMinus = 1.0f - t;
	const float aa = oneMinus * oneMinus * oneMinus;
	const float bb = 3.0f * oneMinus * oneMinus * t;
	const float cc = 3.0f * oneMinus * t * t;
	const float dd = t * t * t;
	x = ax * aa + bx * bb + cx * cc + dx * dd;
	y = ay * aa + by * bb + cy * cc + dy * dd;
}

void renderFlowFrame(
	const Geometry& geometry,
	const std::vector<Pixel>& pixels,
	const std::vector<size_t>& finalAssignment,
	const std::vector<size_t>& destinationForPixel,
	const std::vector<TransportBin>& bins,
	OwnedRows& outputRows,
	const int frameIndex
) {
	const size_t count = geometry.pixelCount();
	if( frameIndex == 0 ) {
		for( size_t i = 0; i < count; ++i ) {
			outputRows.setPixel( i, pixels[i] );
		}
		return;
	}
	if( frameIndex == kFrameCount - 1 ) {
		std::vector<size_t> assignment( count );
		for( size_t pixelId = 0; pixelId < count; ++pixelId ) {
			assignment[destinationForPixel[pixelId]] = pixelId;
		}
		writeAssignmentToRows( assignment, pixels, outputRows );
		return;
	}

	std::vector<float> red( count, 0.0f );
	std::vector<float> green( count, 0.0f );
	std::vector<float> blue( count, 0.0f );
	std::vector<float> weight( count, 0.0f );
	const float globalProgress = static_cast<float>( frameIndex ) / static_cast<float>( kFrameCount - 1 );

	const float backgroundWeight = 1.10f;
	const float targetReveal = smoothstep( ( globalProgress - 0.02f ) / 0.66f );
	for( size_t i = 0; i < count; ++i ) {
		const Pixel& finalPixel = pixels[finalAssignment[i]];
		const float brightness = 0.13f + 0.58f * targetReveal;
		red[i] = static_cast<float>( finalPixel.rgba[0] ) * brightness * backgroundWeight;
		green[i] = static_cast<float>( finalPixel.rgba[1] ) * brightness * backgroundWeight;
		blue[i] = static_cast<float>( finalPixel.rgba[2] ) * brightness * backgroundWeight;
		weight[i] = backgroundWeight;
	}

	for( size_t pixelId = 0; pixelId < count; ++pixelId ) {
		const size_t srcIndex = pixelId;
		const size_t dstIndex = destinationForPixel[pixelId];
		const int binIndex = colorTransportBin( pixels[pixelId].lab );
		const TransportBin& bin = bins[binIndex];
		const float sx = static_cast<float>( srcIndex % static_cast<size_t>( geometry.width ) );
		const float sy = static_cast<float>( srcIndex / static_cast<size_t>( geometry.width ) );
		const float dx = static_cast<float>( dstIndex % static_cast<size_t>( geometry.width ) );
		const float dy = static_cast<float>( dstIndex / static_cast<size_t>( geometry.width ) );
		const float binDx = bin.targetX - bin.sourceX;
		const float binDy = bin.targetY - bin.sourceY;
		const float distance = std::sqrt( binDx * binDx + binDy * binDy );
		const float invDistance = distance > 0.001f ? 1.0f / distance : 0.0f;
		const float lane = ( hashUnit( pixelId ^ 0xC0A57A11u ) - 0.5f ) * std::min( 42.0f, 5.0f + bin.spread * 0.18f );
		const float px = -binDy * invDistance;
		const float py = binDx * invDistance;
		const float controlLift = std::min( 72.0f, 10.0f + distance * 0.18f + bin.spread * 0.05f );
		const float jitter = hashUnit( pixelId ^ 0x1F123BB5u );
		const float delay = 0.02f + 0.22f * bin.phase + 0.06f * jitter;
		const float p = smoothstep( ( globalProgress - delay ) / std::max( 0.22f, 1.0f - delay ) );
		if( p <= 0.0f && frameIndex > 0 ) {
			continue;
		}

		const float sourceControlX = interpolate( sx, bin.sourceX, 0.72f ) + px * lane - py * controlLift;
		const float sourceControlY = interpolate( sy, bin.sourceY, 0.72f ) + py * lane + px * controlLift;
		const float targetControlX = interpolate( dx, bin.targetX, 0.72f ) + px * lane + py * controlLift;
		const float targetControlY = interpolate( dy, bin.targetY, 0.72f ) + py * lane - px * controlLift;
		const float streamPulse = std::sin( p * 3.1415926535f );
		const float settleFade = 1.0f - 0.35f * smoothstep( ( p - 0.66f ) / 0.30f );
		const float opacity = ( 0.24f + 1.15f * smoothstep( ( p - 0.02f ) / 0.24f ) ) * settleFade;
		for( int tail = 2; tail >= 0; --tail ) {
			const float tailP = std::clamp( p - static_cast<float>( tail ) * ( 0.035f + 0.015f * jitter ), 0.0f, 1.0f );
			float x = 0.0f;
			float y = 0.0f;
			cubicPoint( sx, sy, sourceControlX, sourceControlY, targetControlX, targetControlY, dx, dy, tailP, x, y );
			const float shimmer = std::sin( ( tailP * 6.283185307f ) + bin.phase * 6.283185307f + jitter * 6.283185307f );
			x += px * shimmer * streamPulse * 3.0f;
			y += py * shimmer * streamPulse * 3.0f;
			const float tailOpacity = opacity * ( tail == 0 ? 1.0f : ( tail == 1 ? 0.42f : 0.20f ) );
			splat( geometry, pixels[pixelId], x, y, tailOpacity, red, green, blue, weight );
		}
	}

	for( size_t i = 0; i < count; ++i ) {
		const float invWeight = 1.0f / weight[i];
		outputRows.setRgb( i, red[i] * invWeight, green[i] * invWeight, blue[i] * invWeight );
	}
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
	const std::vector<LabColor> targetLab = imageToLab( targetGeometry, targetRows.rows() );
	std::vector<size_t> assignment = buildInitialAssignment( pixels, targetLab );
	std::cout << "Initial color-rank sharp_cost=" << std::fixed << std::setprecision( 2 )
		<< totalCost( assignment, pixels, targetLab )
		<< std::endl;
	polishAssignment( targetGeometry, assignment, pixels, targetLab );

	std::vector<size_t> destinationForPixel( targetGeometry.pixelCount() );
	for( size_t targetIndex = 0; targetIndex < assignment.size(); ++targetIndex ) {
		destinationForPixel[assignment[targetIndex]] = targetIndex;
	}
	const std::vector<TransportBin> transportBins = buildTransportBins( targetGeometry, pixels, destinationForPixel );

	OwnedRows outputRows = OwnedRows::allocateContiguous( targetGeometry );
	FrameWriter frames( targetGeometry, outputRows.rows(), animationPrefixFor( argv[3] ) );
	for( int frameIndex = 0; frameIndex < kFrameCount; ++frameIndex ) {
		renderFlowFrame( targetGeometry, pixels, assignment, destinationForPixel, transportBins, outputRows, frameIndex );
		frames.write();
	}

	writeAssignmentToRows( assignment, pixels, outputRows );
	writePNGFile( argv[3], outputRows.rows(), targetGeometry.width, targetGeometry.height );
	std::cout << "Final sharp_cost=" << std::fixed << std::setprecision( 2 )
		<< totalCost( assignment, pixels, targetLab )
		<< std::endl;
	std::cout << "Final image written to " << argv[3] << std::endl;
	return 0;
}
