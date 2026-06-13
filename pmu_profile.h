#pragma once

// The kernel reads PNGLAB_PMU_CHUNK_CANDIDATES unconditionally (it also sizes the
// non-PMU candidate chunking), so its fallback must live outside the
// ENABLE_CPU_COUNTERS guard. When CPU counters are on, CMake passes the value as a
// compile definition and this #ifndef leaves it untouched.
#ifndef PNGLAB_PMU_CHUNK_CANDIDATES
#define PNGLAB_PMU_CHUNK_CANDIDATES 100000
#endif

#ifdef ENABLE_CPU_COUNTERS

#include <iostream>
#include <string>

#include "perf.h"

#if defined(PNGLAB_PMU_PROFILE_BRANCH)
#define PNGLAB_PMU_COUNTERS BRANCH_PROFILE
#elif defined(PNGLAB_PMU_PROFILE_FRONTEND)
#define PNGLAB_PMU_COUNTERS FRONTEND_PROFILE
#elif defined(PNGLAB_PMU_PROFILE_EXECUTION)
#define PNGLAB_PMU_COUNTERS EXECUTION_PROFILE
#else
#define PNGLAB_PMU_COUNTERS CACHE_PROFILE
#endif

#define PNGLAB_PMU_SCOPE( label ) PERF_SCOPE( ( label ), PNGLAB_PMU_COUNTERS )
#define PNGLAB_PMU_SCOPE_SAMPLED( label, sample_every ) PERF_SCOPE_SAMPLED( ( label ), PNGLAB_PMU_COUNTERS, ( sample_every ) )

inline void primePnglabCpuCounters() {
	std::string error;
	if( !PerfPrimeThread( PNGLAB_PMU_COUNTERS, &error ) && !error.empty() ) {
		std::cerr << "cpu counter instrumentation warning: " << error << '\n';
	}
}

#else

#define PNGLAB_PMU_SCOPE( label )
#define PNGLAB_PMU_SCOPE_SAMPLED( label, sample_every )

inline void primePnglabCpuCounters() {
}

#endif
