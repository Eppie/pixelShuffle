#pragma once

#ifdef ENABLE_CPU_COUNTERS

#include <iostream>
#include <string>

#include "perf.h"

#define PNGLAB_PMU_COUNTERS CACHE_PROFILE
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
