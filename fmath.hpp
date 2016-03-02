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
#include <stddef.h>
#include <assert.h>
#include <limits>
#include <stdlib.h>
#include <float.h>
#include <string.h> // for memcpy
#include <x86intrin.h>

#define MIE_ALIGN(x) __attribute__((aligned(x)))
#define MIE_PACK(x, y, z, w) ((x) * 64 + (y) * 16 + (z) * 4 + (w))

namespace fmath {

namespace local {

const size_t EXP_TABLE_SIZE = 10;
const size_t EXPD_TABLE_SIZE = 11;
const size_t LOG_TABLE_SIZE = 12;

typedef unsigned long long uint64_t;

union fi {
	float f;
	unsigned int i;
};

union di {
	double d;
	uint64_t i;
};

inline unsigned int mask( int x ) {
	return ( 1U << x ) - 1;
}

inline uint64_t mask64( int x ) {
	return ( 1ULL << x ) - 1;
}

template<class T>
inline const T* cast_to( const void* p ) {
	return reinterpret_cast<const T*>( p );
}

template<class T, size_t N>
size_t NumOfArray( const T( & )[N] ) {
	return N;
}

template<size_t N = EXP_TABLE_SIZE>
struct ExpVar {
	enum {
		s = N,
		n = 1 << s,
		f88 = 0x42b00000 // 88.0
	};
	float minX[8];
	float maxX[8];
	float a[8];
	float b[8];
	float f1[8];
	unsigned int i127s[8];
	unsigned int mask_s[8];
	unsigned int i7fffffff[8];
	unsigned int tbl[n];
	ExpVar() {
		float log_2 = ::logf( 2.0f );

		for( int i = 0; i < 8; i++ ) {
			maxX[i] = 88;
			minX[i] = -88;
			a[i] = n / log_2;
			b[i] = log_2 / n;
			f1[i] = 1.0f;
			i127s[i] = 127 << s;
			i7fffffff[i] = 0x7fffffff;
			mask_s[i] = mask( s );
		}

		for( int i = 0; i < n; i++ ) {
			float y = pow( 2.0f, ( float )i / n );
			fi fi;
			fi.f = y;
			tbl[i] = fi.i & mask( 23 );
		}
	}
};

template<size_t sbit_ = EXPD_TABLE_SIZE>
struct ExpdVar {
	enum {
		sbit = sbit_,
		s = 1UL << sbit,
		adj = ( 1UL << ( sbit + 10 ) ) - ( 1UL << sbit )
	};
	// A = 1, B = 1, C = 1/2, D = 1/6
	double C1[2]; // A
	double C2[2]; // D
	double C3[2]; // C/D
	uint64_t tbl[s];
	double a;
	double ra;
	ExpdVar()
		: a( s / ::log( 2.0 ) )
		, ra( 1 / a ) {
		for( int i = 0; i < 2; i++ ) {
			C1[i] = 1.0;
			C2[i] = 0.16666666685227835064;
			C3[i] = 3.0000000027955394;
		}

		for( int i = 0; i < s; i++ ) {
			di di;
			di.d = ::pow( 2.0, i * ( 1.0 / s ) );
			tbl[i] = di.i & mask64( 52 );
		}
	}
};

template<size_t N = LOG_TABLE_SIZE>
struct LogVar {
	enum {
		LEN = N - 1
	};
	unsigned int m1[4]; // 0
	unsigned int m2[4]; // 16
	unsigned int m3[4]; // 32
	float m4[4];        // 48
	unsigned int m5[4]; // 64
	struct {
		float app;
		float rev;
	} tbl[1 << LEN];
	float c_log2;
	LogVar()
		: c_log2( ::logf( 2.0f ) / ( 1 << 23 ) ) {
		const double e = 1 / double( 1 << 24 );
		const double h = 1 / double( 1 << LEN );
		const size_t n = 1U << LEN;

		for( size_t i = 0; i < n; i++ ) {
			double x = 1 + double( i ) / n;
			double a = ::log( x );
			tbl[i].app = ( float )a;

			if( i < n - 1 ) {
				double b = ::log( x + h - e );
				tbl[i].rev = ( float )( ( b - a ) / ( ( h - e ) * ( 1 << 23 ) ) );
			} else {
				tbl[i].rev = ( float )( 1 / ( x * ( 1 << 23 ) ) );
			}
		}

		for( int i = 0; i < 4; i++ ) {
			m1[i] = mask( 8 ) << 23;
			m2[i] = mask( LEN ) << ( 23 - LEN );
			m3[i] = mask( 23 - LEN );
			m4[i] = c_log2;
			m5[i] = 127U << 23;
		}
	}
};

/* to define static variables in fmath.hpp */
template<size_t EXP_N = EXP_TABLE_SIZE, size_t LOG_N = LOG_TABLE_SIZE, size_t EXPD_N = EXPD_TABLE_SIZE>
struct C {
	static const ExpVar<EXP_N> expVar;
	static const LogVar<LOG_N> logVar;
	static const ExpdVar<EXPD_N> expdVar;
};

template<size_t EXP_N, size_t LOG_N, size_t EXPD_N>
MIE_ALIGN( 32 ) const ExpVar<EXP_N> C<EXP_N, LOG_N, EXPD_N>::expVar;

template<size_t EXP_N, size_t LOG_N, size_t EXPD_N>
MIE_ALIGN( 32 ) const LogVar<LOG_N> C<EXP_N, LOG_N, EXPD_N>::logVar;

template<size_t EXP_N, size_t LOG_N, size_t EXPD_N>
MIE_ALIGN( 32 ) const ExpdVar<EXPD_N> C<EXP_N, LOG_N, EXPD_N>::expdVar;

} // fmath::local

inline float exp( float x )
{
	using namespace local;
	const ExpVar<> &expVar = C<>::expVar;

	__m128 x1 = _mm_set_ss( x );

	int limit = _mm_cvtss_si32( x1 ) & 0x7fffffff;

	if( limit > ExpVar<>::f88 ) {
		x1 = _mm_min_ss( x1, _mm_load_ss( expVar.maxX ) );
		x1 = _mm_max_ss( x1, _mm_load_ss( expVar.minX ) );
	}

	int r = _mm_cvtss_si32( _mm_mul_ss( x1, _mm_load_ss( expVar.a ) ) );
	unsigned int v = r & mask( expVar.s );
	float t = _mm_cvtss_f32( x1 ) - r * expVar.b[0];
	int u = r >> expVar.s;
	fi fi;
	fi.i = ( ( u + 127 ) << 23 ) | expVar.tbl[v];
	return ( 1 + t ) * fi.f;
}

inline float log( float x ) {
	using namespace local;
	const LogVar<> &logVar = C<>::logVar;
	const size_t logLen = logVar.LEN;

	fi fi;
	fi.f = x;
	int a = fi.i & ( mask( 8 ) << 23 );
	unsigned int b1 = fi.i & ( mask( logLen ) << ( 23 - logLen ) );
	unsigned int b2 = fi.i & mask( 23 - logLen );
	int idx = b1 >> ( 23 - logLen );
	float f = float( a - ( 127 << 23 ) ) * logVar.c_log2 + logVar.tbl[idx].app + float( b2 ) * logVar.tbl[idx].rev;
	return f;
}

// log2(x) = log(x) / log(2)
inline float log2( float x ) {
	return fmath::log( x ) * 1.442695f;
}

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

// exp2(x) = pow(2, x)
inline float exp2( float x ) {
	return fmath::exp( x * 0.6931472f );
}

} // fmath
