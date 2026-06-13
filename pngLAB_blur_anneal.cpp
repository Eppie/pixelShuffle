#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "pngReadWrite.h"
#include "lab_grid.h"

namespace {

constexpr uint64_t kInitialRandomSeed = 0xB17A55EED5A11E9ULL;
constexpr int kTargetFrameCount = 128;

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

	png_bytep pixel( const size_t index ) {
		return contiguousBuffer_ + index * 4u;
	}

	void seedFromPalette( const OwnedRows& palette, const Geometry& paletteGeometry, const Geometry& targetGeometry ) {
		size_t dstIndex = 0;
		for( int y = 0; y < paletteGeometry.height; ++y ) {
			const png_bytep row = palette.rows_[y];
			for( int x = 0; x < paletteGeometry.width; ++x ) {
				std::memcpy( pixel( dstIndex++ ), &row[x * 4], 4 );
			}
		}
		( void ) targetGeometry;
	}

	void swapPixels( const size_t lhs, const size_t rhs ) {
		png_bytep left = pixel( lhs );
		png_bytep right = pixel( rhs );
		for( int i = 0; i < 4; ++i ) {
			std::swap( left[i], right[i] );
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

float totalCost( const std::vector<LabColor>& working, const std::vector<LabColor>& target ) {
	float cost = 0.0f;
	for( size_t i = 0; i < working.size(); ++i ) {
		cost += squaredLabDistance( working[i], target[i] );
	}
	return cost;
}

std::vector<LabColor> boxBlurLab( const std::vector<LabColor>& source, const Geometry& geometry, const int radius ) {
	if( radius <= 0 ) {
		return source;
	}

	const int width = geometry.width;
	const int height = geometry.height;
	const size_t count = geometry.pixelCount();
	std::vector<LabColor> horizontal( count );
	std::vector<LabColor> result( count );
	std::vector<float> prefixL( static_cast<size_t>( std::max( width, height ) ) + 1u );
	std::vector<float> prefixA( prefixL.size() );
	std::vector<float> prefixB( prefixL.size() );

	for( int y = 0; y < height; ++y ) {
		const size_t rowOffset = static_cast<size_t>( y ) * static_cast<size_t>( width );
		prefixL[0] = 0.0f;
		prefixA[0] = 0.0f;
		prefixB[0] = 0.0f;
		for( int x = 0; x < width; ++x ) {
			const LabColor& px = source[rowOffset + static_cast<size_t>( x )];
			prefixL[static_cast<size_t>( x + 1 )] = prefixL[static_cast<size_t>( x )] + px.l;
			prefixA[static_cast<size_t>( x + 1 )] = prefixA[static_cast<size_t>( x )] + px.a;
			prefixB[static_cast<size_t>( x + 1 )] = prefixB[static_cast<size_t>( x )] + px.b;
		}
		for( int x = 0; x < width; ++x ) {
			const int begin = std::max( 0, x - radius );
			const int end = std::min( width - 1, x + radius );
			const float invCount = 1.0f / static_cast<float>( end - begin + 1 );
			horizontal[rowOffset + static_cast<size_t>( x )] = {
				( prefixL[static_cast<size_t>( end + 1 )] - prefixL[static_cast<size_t>( begin )] ) * invCount,
				( prefixA[static_cast<size_t>( end + 1 )] - prefixA[static_cast<size_t>( begin )] ) * invCount,
				( prefixB[static_cast<size_t>( end + 1 )] - prefixB[static_cast<size_t>( begin )] ) * invCount
			};
		}
	}

	for( int x = 0; x < width; ++x ) {
		prefixL[0] = 0.0f;
		prefixA[0] = 0.0f;
		prefixB[0] = 0.0f;
		for( int y = 0; y < height; ++y ) {
			const LabColor& px = horizontal[static_cast<size_t>( y ) * static_cast<size_t>( width ) + static_cast<size_t>( x )];
			prefixL[static_cast<size_t>( y + 1 )] = prefixL[static_cast<size_t>( y )] + px.l;
			prefixA[static_cast<size_t>( y + 1 )] = prefixA[static_cast<size_t>( y )] + px.a;
			prefixB[static_cast<size_t>( y + 1 )] = prefixB[static_cast<size_t>( y )] + px.b;
		}
		for( int y = 0; y < height; ++y ) {
			const int begin = std::max( 0, y - radius );
			const int end = std::min( height - 1, y + radius );
			const float invCount = 1.0f / static_cast<float>( end - begin + 1 );
			result[static_cast<size_t>( y ) * static_cast<size_t>( width ) + static_cast<size_t>( x )] = {
				( prefixL[static_cast<size_t>( end + 1 )] - prefixL[static_cast<size_t>( begin )] ) * invCount,
				( prefixA[static_cast<size_t>( end + 1 )] - prefixA[static_cast<size_t>( begin )] ) * invCount,
				( prefixB[static_cast<size_t>( end + 1 )] - prefixB[static_cast<size_t>( begin )] ) * invCount
			};
		}
	}

	return result;
}

std::vector<int> blurRadiiFor( const Geometry& geometry ) {
	const int minDim = std::max( 1, std::min( geometry.width, geometry.height ) );
	const int rawRadii[] = {
		std::max( 1, minDim / 6 ),
		std::max( 1, minDim / 9 ),
		std::max( 1, minDim / 13 ),
		std::max( 1, minDim / 19 ),
		std::max( 1, minDim / 28 ),
		std::max( 1, minDim / 42 ),
		std::max( 1, minDim / 64 ),
		std::max( 1, minDim / 96 ),
		std::max( 1, minDim / 144 ),
		std::max( 1, minDim / 220 ),
		0,
		0,
		0
	};

	std::vector<int> radii;
	for( const int radius : rawRadii ) {
		if( radius == 0 || radii.empty() || radii.back() != radius ) {
			radii.push_back( radius );
		}
	}
	if( radii.back() != 0 ) {
		radii.push_back( 0 );
	}
	return radii;
}

std::vector<int> framesPerStageFor( const size_t stageCount ) {
	std::vector<int> frames( stageCount, 0 );
	if( stageCount == 0 ) {
		return frames;
	}

	const int remainingFrames = kTargetFrameCount - 1;
	const int baseFrames = remainingFrames / static_cast<int>( stageCount );
	int extraFrames = remainingFrames % static_cast<int>( stageCount );
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

struct StageResult {
	uint64_t accepted = 0;
	uint64_t improved = 0;
	uint64_t uphill = 0;
};

StageResult runAnnealingStage(
	const Geometry& geometry,
	std::vector<LabColor>& working,
	OwnedRows& outputRows,
	const std::vector<LabColor>& target,
	const uint64_t candidates,
	const int window,
	const double globalChance,
	const double startTemperature,
	const double endTemperature,
	const int framesToWrite,
	XorShift64Star& rng,
	FrameWriter& frames
) {
	StageResult result;
	const size_t count = geometry.pixelCount();
	int writtenFrames = 0;
	uint64_t nextFrameAt = framesToWrite > 0
		? std::max<uint64_t>( 1, ( candidates + static_cast<uint64_t>( framesToWrite ) - 1u ) / static_cast<uint64_t>( framesToWrite ) )
		: candidates + 1u;

	for( uint64_t iter = 1; iter <= candidates; ++iter ) {
		const size_t first = rng.uniformIndex( count );
		const size_t second = pickPartner( first, geometry, window, globalChance, rng );

		const float keepCost = squaredLabDistance( working[first], target[first] ) + squaredLabDistance( working[second], target[second] );
		const float swapCost = squaredLabDistance( working[first], target[second] ) + squaredLabDistance( working[second], target[first] );
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
			std::swap( working[first], working[second] );
			outputRows.swapPixels( first, second );
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

uint64_t candidatesPerStage( const Geometry& geometry ) {
	const size_t count = geometry.pixelCount();
	const uint64_t multiplier = count > 600000u ? 9u : 20u;
	return std::max<uint64_t>( 200000u, static_cast<uint64_t>( count ) * multiplier );
}

uint64_t candidatesForRadius( const uint64_t baseCandidates, const int radius ) {
	if( radius == 0 ) {
		return baseCandidates * 2u;
	}
	if( radius <= 2 ) {
		return ( baseCandidates * 3u ) / 2u;
	}
	return baseCandidates;
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

	OwnedRows outputRows = OwnedRows::allocateContiguous( targetGeometry );
	outputRows.seedFromPalette( paletteRows, paletteGeometry, targetGeometry );

	std::vector<LabColor> working = imageToLab( targetGeometry, outputRows.rows() );
	const std::vector<LabColor> sharpTarget = imageToLab( targetGeometry, targetRows.rows() );
	const std::vector<int> radii = blurRadiiFor( targetGeometry );
	const std::vector<int> stageFrames = framesPerStageFor( radii.size() );
	const int maxRadius = std::max( 1, radii.front() );
	const int maxDimension = std::max( targetGeometry.width, targetGeometry.height );
	const uint64_t baseCandidates = candidatesPerStage( targetGeometry );

	FrameWriter frames( targetGeometry, outputRows.rows(), animationPrefixFor( argv[3] ) );
	frames.write();

	XorShift64Star rng;
	int sharpPassIndex = 0;
	for( size_t stageIndex = 0; stageIndex < radii.size(); ++stageIndex ) {
		const int radius = radii[stageIndex];
		const int currentSharpPass = radius == 0 ? sharpPassIndex++ : -1;
		std::vector<LabColor> blurredTarget;
		const std::vector<LabColor>* stageTarget = &sharpTarget;
		if( radius > 0 ) {
			blurredTarget = boxBlurLab( sharpTarget, targetGeometry, radius );
			stageTarget = &blurredTarget;
		}

		const double radiusFraction = static_cast<double>( radius ) / static_cast<double>( maxRadius );
		int window = stageIndex == 0
			? maxDimension
			: std::clamp( ( radius * 5 ) + 6, 4, maxDimension );
		double globalChance = radius == 0 ? 0.04 : 0.12;
		double startTemperature = radius == 0 ? 10.0 : 1200.0 * radiusFraction + 8.0;
		double endTemperature = radius == 0 ? 0.35 : 80.0 * radiusFraction + 0.75;
		if( radius == 0 ) {
			if( currentSharpPass == 0 ) {
				window = 12;
				globalChance = 0.08;
				startTemperature = 14.0;
				endTemperature = 1.0;
			} else if( currentSharpPass == 1 ) {
				window = 6;
				globalChance = 0.03;
				startTemperature = 2.5;
				endTemperature = 0.08;
			} else {
				window = 3;
				globalChance = 0.01;
				startTemperature = 0.0;
				endTemperature = 0.0;
			}
		}
		const uint64_t stageCandidates = candidatesForRadius( baseCandidates, radius );

		std::cout << "Stage " << ( stageIndex + 1 ) << "/" << radii.size()
			<< ": blur_radius=" << radius
			<< ", candidates=" << stageCandidates
			<< ", window=" << window
			<< ", frames=" << stageFrames[stageIndex]
			<< ", temp=" << std::fixed << std::setprecision( 2 ) << startTemperature
			<< "->" << endTemperature
			<< std::endl;

		const StageResult result = runAnnealingStage(
			targetGeometry,
			working,
			outputRows,
			*stageTarget,
			stageCandidates,
			window,
			globalChance,
			startTemperature,
			endTemperature,
			stageFrames[stageIndex],
			rng,
			frames
		);

		std::cout << "  accepted=" << result.accepted
			<< " improved=" << result.improved
			<< " uphill=" << result.uphill
			<< " sharp_cost=" << std::fixed << std::setprecision( 2 ) << totalCost( working, sharpTarget )
			<< std::endl;
	}

	writePNGFile( argv[3], outputRows.rows(), targetGeometry.width, targetGeometry.height );
	std::cout << "Final image written to " << argv[3] << std::endl;
	return 0;
}
