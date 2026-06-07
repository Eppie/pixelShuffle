#pragma once

#ifdef PROFILE_STATS

#include <array>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>

namespace ProfileStats {

enum class EventId : std::size_t {
    ProgramTotal = 0,
    ReadPNG,
    WritePNG,
    SeedCopy,
    ImageToLab,
    LabToImage,
    TotalDiff,
    ProcessPNG,
    OrderedLoop,
    RandomLoop,
    SwapDecisionSample,
    PixelDiffSample,
    SwapPixelsSample,
    XorShiftSample,
    PowGetSample,
    EventCount
};

struct EventStats {
    const char* name;
    uint64_t calls;
    uint64_t ops;
    uint64_t ns;
};

inline uint64_t nowNs() {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

inline std::array<EventStats, static_cast<std::size_t>(EventId::EventCount)>& events() {
    static std::array<EventStats, static_cast<std::size_t>(EventId::EventCount)> value = {{
        { "program_total", 0, 0, 0 },
        { "read_png", 0, 0, 0 },
        { "write_png", 0, 0, 0 },
        { "seed_copy", 0, 0, 0 },
        { "image_to_lab", 0, 0, 0 },
        { "lab_to_image", 0, 0, 0 },
        { "total_diff", 0, 0, 0 },
        { "process_png_file", 0, 0, 0 },
        { "ordered_loop", 0, 0, 0 },
        { "random_loop", 0, 0, 0 },
        { "swap_decision_sample", 0, 0, 0 },
        { "pixel_diff_sample", 0, 0, 0 },
        { "swap_pixels_sample", 0, 0, 0 },
        { "xorshift64star_sample", 0, 0, 0 },
        { "pow_get_sample", 0, 0, 0 },
    }};
    return value;
}

inline void add(EventId id, uint64_t ns, uint64_t calls = 1, uint64_t ops = 0) {
    auto& event = events()[static_cast<std::size_t>(id)];
    event.calls += calls;
    event.ops += ops;
    event.ns += ns;
}

class ScopedTimer {
public:
    explicit ScopedTimer(EventId id, uint64_t ops = 0)
        : id_(id), ops_(ops), start_ns_(nowNs()), stopped_(false) {
    }

    ~ScopedTimer() {
        stop();
    }

    void stop(uint64_t ops_override = UINT64_MAX) {
        if (stopped_) {
            return;
        }
        const uint64_t ops = ops_override == UINT64_MAX ? ops_ : ops_override;
        add(id_, nowNs() - start_ns_, 1, ops);
        stopped_ = true;
    }

private:
    EventId id_;
    uint64_t ops_;
    uint64_t start_ns_;
    bool stopped_;
};

inline void printReport() {
    std::cerr << "\n--- Profile Stats ---\n";
    std::cerr << std::left
              << std::setw(24) << "Region"
              << std::setw(12) << "Calls"
              << std::setw(18) << "Ops"
              << std::setw(18) << "Total ns"
              << std::setw(18) << "ns/call"
              << std::setw(18) << "ns/op"
              << std::setw(18) << "op/s"
              << '\n';
    std::cerr << std::string(126, '-') << '\n';

    for (const auto& event : events()) {
        if (event.calls == 0 && event.ops == 0 && event.ns == 0) {
            continue;
        }

        const double ns_per_call = event.calls ? static_cast<double>(event.ns) / static_cast<double>(event.calls) : 0.0;
        const double ns_per_op = event.ops ? static_cast<double>(event.ns) / static_cast<double>(event.ops) : 0.0;
        const double ops_per_s = event.ns ? (static_cast<double>(event.ops) * 1e9) / static_cast<double>(event.ns) : 0.0;

        std::cerr << std::left
                  << std::setw(24) << event.name
                  << std::setw(12) << event.calls
                  << std::setw(18) << event.ops
                  << std::setw(18) << event.ns
                  << std::setw(18) << std::fixed << std::setprecision(2) << ns_per_call
                  << std::setw(18) << std::fixed << std::setprecision(4) << ns_per_op
                  << std::setw(18) << std::fixed << std::setprecision(2) << ops_per_s
                  << '\n';
    }

    std::cerr << std::string(126, '-') << '\n';

    std::cerr << "PROFILE_STATS_JSON {";
    bool first = true;
    for (const auto& event : events()) {
        if (event.calls == 0 && event.ops == 0 && event.ns == 0) {
            continue;
        }
        if (!first) {
            std::cerr << ',';
        }
        first = false;
        std::cerr << '"' << event.name << "\":{"
                  << "\"calls\":" << event.calls << ','
                  << "\"ops\":" << event.ops << ','
                  << "\"ns\":" << event.ns
                  << '}';
    }
    std::cerr << "}\n";
}

struct AtExitReporter {
    AtExitReporter() {
        std::atexit(printReport);
    }
};

inline AtExitReporter reporter;

} // namespace ProfileStats

#endif
