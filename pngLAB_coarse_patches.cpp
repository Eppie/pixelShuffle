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

constexpr int kRevealFrameCount = 128;
constexpr int kPolishFrameCount = 32;
constexpr uint64_t kInitialRandomSeed = 0xC0A45E2A7C4B11DULL;

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

struct Block {
	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;
	size_t count = 0;
	LabColor mean;
	uint64_t key = 0;
	double visualOrder = 0.0;
};

LabColor meanTargetForBlock( const std::vector<LabColor>& target, const Geometry& geometry, const int x0, const int y0, const int x1, const int y1 ) {
	double l = 0.0;
	double a = 0.0;
	double b = 0.0;
	size_t count = 0;
	for( int y = y0; y < y1; ++y ) {
		const size_t rowOffset = static_cast<size_t>( y ) * static_cast<size_t>( geometry.width );
		for( int x = x0; x < x1; ++x ) {
			const LabColor& color = target[rowOffset + static_cast<size_t>( x )];
			l += color.l;
			a += color.a;
			b += color.b;
			++count;
		}
	}
	const double invCount = 1.0 / static_cast<double>( count );
	return {
		static_cast<float>( l * invCount ),
		static_cast<float>( a * invCount ),
		static_cast<float>( b * invCount )
	};
}

std::vector<Block> makeBlocks( const Geometry& geometry, const std::vector<LabColor>& target, const int blockSize ) {
	std::vector<Block> blocks;
	blocks.reserve( ( static_cast<size_t>( geometry.width ) + static_cast<size_t>( blockSize ) - 1u ) / static_cast<size_t>( blockSize )
		* ( ( static_cast<size_t>( geometry.height ) + static_cast<size_t>( blockSize ) - 1u ) / static_cast<size_t>( blockSize ) ) );

	const double cx = ( static_cast<double>( geometry.width ) - 1.0 ) * 0.5;
	const double cy = ( static_cast<double>( geometry.height ) - 1.0 ) * 0.5;
	for( int y0 = 0; y0 < geometry.height; y0 += blockSize ) {
		const int y1 = std::min( geometry.height, y0 + blockSize );
		for( int x0 = 0; x0 < geometry.width; x0 += blockSize ) {
			const int x1 = std::min( geometry.width, x0 + blockSize );
			Block block;
			block.x0 = x0;
			block.y0 = y0;
			block.x1 = x1;
			block.y1 = y1;
			block.count = static_cast<size_t>( x1 - x0 ) * static_cast<size_t>( y1 - y0 );
			block.mean = meanTargetForBlock( target, geometry, x0, y0, x1, y1 );
			block.key = colorKey( block.mean );
			const double bx = ( static_cast<double>( x0 + x1 - 1 ) * 0.5 ) - cx;
			const double by = ( static_cast<double>( y0 + y1 - 1 ) * 0.5 ) - cy;
			block.visualOrder = ( bx * bx ) + ( by * by );
			blocks.push_back( block );
		}
	}
	return blocks;
}

std::vector<int> blockSizesFor( const Geometry& geometry ) {
	const int minDim = std::max( 1, std::min( geometry.width, geometry.height ) );
	const int rawSizes[] = {
		std::max( 2, minDim / 2 ),
		std::max( 2, minDim / 3 ),
		std::max( 2, minDim / 4 ),
		std::max( 2, minDim / 6 ),
		std::max( 2, minDim / 8 ),
		std::max( 2, minDim / 12 ),
		std::max( 2, minDim / 16 ),
		std::max( 2, minDim / 24 ),
		std::max( 2, minDim / 32 ),
		std::max( 2, minDim / 48 ),
		std::max( 2, minDim / 80 ),
		std::max( 2, minDim / 160 ),
		1
	};

	std::vector<int> result;
	for( const int size : rawSizes ) {
		if( result.empty() || result.back() != size ) {
			result.push_back( size );
		}
	}
	if( result.back() != 1 ) {
		result.push_back( 1 );
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

std::vector<size_t> positionsInBlock( const Geometry& geometry, const Block& block ) {
	std::vector<size_t> positions;
	positions.reserve( block.count );
	for( int y = block.y0; y < block.y1; ++y ) {
		const size_t rowOffset = static_cast<size_t>( y ) * static_cast<size_t>( geometry.width );
		for( int x = block.x0; x < block.x1; ++x ) {
			positions.push_back( rowOffset + static_cast<size_t>( x ) );
		}
	}
	return positions;
}

std::vector<size_t> buildStageAssignment(
	const Geometry& geometry,
	const std::vector<Block>& blocks,
	const std::vector<size_t>& paletteByColor,
	const std::vector<LabColor>& target,
	const std::vector<uint64_t>& targetKeys
) {
	std::vector<size_t> desired( geometry.pixelCount() );
	std::vector<Block> blocksByColor = blocks;
	std::sort( blocksByColor.begin(), blocksByColor.end(), []( const Block& lhs, const Block& rhs ) {
		if( lhs.key != rhs.key ) {
			return lhs.key < rhs.key;
		}
		return labLess( lhs.mean, rhs.mean );
	} );

	size_t cursor = 0;
	for( const Block& block : blocksByColor ) {
		std::vector<size_t> positions = positionsInBlock( geometry, block );
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
	const Geometry& geometry,
	const std::vector<Block>& blocks,
	const std::vector<size_t>& desired,
	std::vector<size_t>& current,
	const std::vector<Pixel>& pixels,
	OwnedRows& outputRows,
	FrameWriter& frames,
	const int framesToWrite
) {
	std::vector<Block> visualBlocks = blocks;
	std::sort( visualBlocks.begin(), visualBlocks.end(), []( const Block& lhs, const Block& rhs ) {
		if( lhs.visualOrder != rhs.visualOrder ) {
			return lhs.visualOrder < rhs.visualOrder;
		}
		if( lhs.y0 != rhs.y0 ) {
			return lhs.y0 < rhs.y0;
		}
		return lhs.x0 < rhs.x0;
	} );

	const uint64_t totalUpdates = static_cast<uint64_t>( geometry.pixelCount() );
	uint64_t updates = 0;
	int writtenFrames = 0;
	uint64_t nextFrameAt = framesToWrite > 0
		? std::max<uint64_t>( 1, ( totalUpdates + static_cast<uint64_t>( framesToWrite ) - 1u ) / static_cast<uint64_t>( framesToWrite ) )
		: totalUpdates + 1u;

	for( const Block& block : visualBlocks ) {
		for( int y = block.y0; y < block.y1; ++y ) {
			const size_t rowOffset = static_cast<size_t>( y ) * static_cast<size_t>( geometry.width );
			for( int x = block.x0; x < block.x1; ++x ) {
				const size_t index = rowOffset + static_cast<size_t>( x );
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

	std::vector<size_t> current( targetGeometry.pixelCount() );
	std::iota( current.begin(), current.end(), 0u );

	OwnedRows outputRows = OwnedRows::allocateContiguous( targetGeometry );
	writeAssignmentToRows( current, pixels, outputRows );

	const std::vector<int> blockSizes = blockSizesFor( targetGeometry );
	const std::vector<int> stageFrames = framesPerStageFor( blockSizes.size(), kRevealFrameCount - 1 );
	FrameWriter frames( targetGeometry, outputRows.rows(), animationPrefixFor( argv[3] ) );
	frames.write();

	for( size_t stageIndex = 0; stageIndex < blockSizes.size(); ++stageIndex ) {
		const int blockSize = blockSizes[stageIndex];
		const std::vector<Block> blocks = makeBlocks( targetGeometry, targetLab, blockSize );
		const std::vector<size_t> desired = buildStageAssignment(
			targetGeometry,
			blocks,
			paletteByColor,
			targetLab,
			targetKeys
		);

		std::cout << "Stage " << ( stageIndex + 1 ) << "/" << blockSizes.size()
			<< ": block_size=" << blockSize
			<< ", blocks=" << blocks.size()
			<< ", frames=" << stageFrames[stageIndex]
			<< std::endl;

		applyStage(
			targetGeometry,
			blocks,
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
