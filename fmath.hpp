#pragma once
/*
	@brief fast math library for float
	@author herumi
	@url http://homepage1.nifty.com/herumi/
	@note modified new BSD license
	http://opensource.org/licenses/BSD-3-Clause

	cl /Ox /Ob2 /arch:SSE2 /fp:fast bench.cpp -I../xbyak /EHsc /DNOMINMAX
	g++ -O3 -fomit-frame-pointer -fno-operator-names -march=core2 -mssse3 -mfpmath=sse -ffast-math -fexcess-precision=fast
*/
#include <math.h>
#include <x86intrin.h>

namespace fmath {

namespace local {

union fi {
	float f;
	unsigned int i;
};

inline unsigned int mask( const int x ) {
	return ( 1U << x ) - 1;
}

} // fmath::local

/*
	for given y > 0
	get f_y(x) := pow(x, y) for x >= 0
*/
class PowGenerator {
	enum {
		N = 11
	};
	float tbl0_[256];
	struct {
		float app;
		float rev;
	} tbl1_[1 << N];
public:
	PowGenerator( float y ) {
		for( int i = 0; i < 256; i++ ) {
			tbl0_[i] = ::powf( 2, ( i - 127 ) * y );
		}

		const double e = 1 / double( 1 << 24 );
		const double h = 1 / double( 1 << N );
		const size_t n = 1U << N;

		for( size_t i = 0; i < n; i++ ) {
			double x = 1 + double( i ) / n;
			double a = ::pow( x, ( double )y );
			tbl1_[i].app = ( float )a;
			double b = ::pow( x + h - e, ( double )y );
			tbl1_[i].rev = ( float )( ( b - a ) / ( h - e ) / ( 1 << 23 ) );
		}
	}
	float get( float x ) const {
		using namespace local;
		fi fi;
		fi.f = x;
		int a = ( fi.i >> 23 ) & mask( 8 );
		unsigned int b = fi.i & mask( 23 );
		unsigned int b1 = b & ( mask( N ) << ( 23 - N ) );
		unsigned int b2 = b & mask( 23 - N );
		float f;
		int idx = b1 >> ( 23 - N );
		f = tbl0_[a] * ( tbl1_[idx].app + float( b2 ) * tbl1_[idx].rev );
		return f;
	}
};

} // fmath
