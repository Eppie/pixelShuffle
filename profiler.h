#ifndef PROFILER_H
#define PROFILER_H

#ifdef ENABLE_PROFILING

#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>
#include <iomanip>

namespace Profiling {

struct FunctionProfile {
    std::string name;
    long long call_count;
    std::chrono::nanoseconds total_time;

    FunctionProfile(std::string n) : name(std::move(n)), call_count(0), total_time(0) {}
};

// Global vector to store profiling data
static std::vector<FunctionProfile> global_profile_data;
// Mutex for thread safety
// #include <mutex>
// static std::mutex profile_mutex;

class Profiler {
public:
    Profiler(std::string function_name)
        : func_name(std::move(function_name)), start_time(std::chrono::high_resolution_clock::now()) {}

    ~Profiler() {
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);

        // std::lock_guard<std::mutex> lock(profile_mutex); // For thread safety
        auto it = std::find_if(global_profile_data.begin(), global_profile_data.end(),
                               [&](const FunctionProfile& fp) { return fp.name == func_name; });

        if (it == global_profile_data.end()) {
            global_profile_data.emplace_back(func_name);
            it = global_profile_data.end() - 1;
        }

        it->call_count++;
        it->total_time += duration;
    }

private:
    std::string func_name;
    std::chrono::time_point<std::chrono::high_resolution_clock> start_time;
};

static void printProfilingReport() {
    std::cerr << "\n--- Profiling Report ---\n";
    std::cerr << std::left << std::setw(40) << "Function Name"
              << std::setw(15) << "Call Count"
              << std::setw(25) << "Total Time (ns)"
              << std::setw(20) << "Avg Time/Call (ns)" << std::endl;
    std::cerr << std::string(100, '-') << std::endl;

    for (const auto& data : global_profile_data) {
        long long avg_time_per_call = 0;
        if (data.call_count > 0) {
            avg_time_per_call = data.total_time.count() / data.call_count;
        }
        std::cerr << std::left << std::setw(40) << data.name
                  << std::setw(15) << data.call_count
                  << std::setw(25) << data.total_time.count()
                  << std::setw(20) << avg_time_per_call << std::endl;
    }
    std::cerr << "------------------------\n";
}

// Register the report printing function to be called at exit
// Needs to be done in a way that it's executed only once.
// A common way is to use a static object whose constructor registers it.
struct AtExitReporter {
    AtExitReporter() {
        std::atexit(printProfilingReport);
    }
};
static AtExitReporter global_at_exit_reporter;


#define PROFILE_FUNCTION() Profiling::Profiler profiler_##__func__(__func__)
#define PROFILE_SCOPE(name) Profiling::Profiler profiler_##name(#name)

} // namespace Profiling

#else // ENABLE_PROFILING not defined

#define PROFILE_FUNCTION()
#define PROFILE_SCOPE(name)

#endif // ENABLE_PROFILING

#endif // PROFILER_H
