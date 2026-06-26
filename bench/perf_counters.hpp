// Hardware performance counters, read through perf_event_open.
//
// Latency percentiles say which structure is faster. These counters say why. A run
// reports instructions per cycle, branch misprediction rate, and cache misses per
// thousand instructions, which is the language in which the map versus contiguous
// result is actually explained: the map loses on cache misses from pointer chasing, the
// contiguous scan can lose on branch misses once it grows long.
//
// The wrapper opens one counter group so the five events are scheduled together and
// read in a single syscall. It is Linux only and degrades cleanly: on any other system,
// or when the kernel denies counter access, available() returns false and the harness
// reports counters as unavailable rather than failing.
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__linux__)
#include <asm/unistd.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace obls {

struct CounterReading {
    bool available = false;
    std::uint64_t instructions = 0;
    std::uint64_t cycles = 0;
    std::uint64_t branches = 0;
    std::uint64_t branch_misses = 0;
    std::uint64_t cache_misses = 0;

    double ipc() const {
        return cycles == 0
                   ? 0.0
                   : static_cast<double>(instructions) / static_cast<double>(cycles);
    }
    double branch_miss_rate() const {
        return branches == 0
                   ? 0.0
                   : static_cast<double>(branch_misses) / static_cast<double>(branches);
    }
    // Misses per thousand instructions, the conventional way to quote cache pressure
    // without needing a separate cache references counter that might not co schedule.
    double cache_misses_per_kilo_instr() const {
        return instructions == 0 ? 0.0
                                 : static_cast<double>(cache_misses) * 1000.0 /
                                       static_cast<double>(instructions);
    }
};

#if defined(__linux__)

class PerfCounters {
public:
    PerfCounters() {
        leader_fd_ = open_event(PERF_COUNT_HW_INSTRUCTIONS, -1, true);
        if (leader_fd_ < 0) {
            available_ = false;
            return;
        }
        cycles_fd_ = open_event(PERF_COUNT_HW_CPU_CYCLES, leader_fd_, false);
        branches_fd_ = open_event(PERF_COUNT_HW_BRANCH_INSTRUCTIONS, leader_fd_, false);
        branch_miss_fd_ = open_event(PERF_COUNT_HW_BRANCH_MISSES, leader_fd_, false);
        cache_miss_fd_ = open_event(PERF_COUNT_HW_CACHE_MISSES, leader_fd_, false);
        available_ = cycles_fd_ >= 0 && branches_fd_ >= 0 && branch_miss_fd_ >= 0 &&
                     cache_miss_fd_ >= 0;
    }

    ~PerfCounters() {
        close_fd(cache_miss_fd_);
        close_fd(branch_miss_fd_);
        close_fd(branches_fd_);
        close_fd(cycles_fd_);
        close_fd(leader_fd_);
    }

    PerfCounters(const PerfCounters&) = delete;
    PerfCounters& operator=(const PerfCounters&) = delete;
    PerfCounters(PerfCounters&&) = delete;
    PerfCounters& operator=(PerfCounters&&) = delete;

    bool available() const { return available_; }

    void start() {
        if (!available_) {
            return;
        }
        ioctl(leader_fd_, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
        ioctl(leader_fd_, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
    }

    void stop() {
        if (!available_) {
            return;
        }
        ioctl(leader_fd_, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
    }

    // Reads all five counters in one go. PERF_FORMAT_GROUP returns the values in the
    // order the events were added, leader first, so no per event id bookkeeping is
    // needed to demultiplex them.
    CounterReading read_counters() {
        CounterReading out;
        if (!available_) {
            return out;
        }
        struct {
            std::uint64_t nr;
            std::uint64_t values[5];
        } buffer{};
        const ssize_t got = ::read(leader_fd_, &buffer, sizeof(buffer));
        if (got < 0 || buffer.nr != 5ULL) {
            return out;
        }
        out.available = true;
        out.instructions = buffer.values[0];
        out.cycles = buffer.values[1];
        out.branches = buffer.values[2];
        out.branch_misses = buffer.values[3];
        out.cache_misses = buffer.values[4];
        return out;
    }

private:
    static long perf_event_open(perf_event_attr* attr, pid_t pid, int cpu, int group_fd,
                                unsigned long flags) {
        return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
    }

    int open_event(std::uint64_t config, int group_fd, bool is_leader) {
        perf_event_attr attr{};
        attr.type = PERF_TYPE_HARDWARE;
        attr.size = static_cast<std::uint32_t>(sizeof(attr));
        attr.config = config;
        attr.disabled = is_leader ? 1U : 0U;
        attr.exclude_kernel = 1U;  // count user space only, where the book code runs
        attr.exclude_hv = 1U;
        if (is_leader) {
            attr.read_format = PERF_FORMAT_GROUP;
        }
        const long fd = perf_event_open(&attr, 0, -1, group_fd, 0);
        return static_cast<int>(fd);
    }

    static void close_fd(int fd) {
        if (fd >= 0) {
            ::close(fd);
        }
    }

    int leader_fd_ = -1;
    int cycles_fd_ = -1;
    int branches_fd_ = -1;
    int branch_miss_fd_ = -1;
    int cache_miss_fd_ = -1;
    bool available_ = false;
};

#else  // not Linux

// No op stand in so the harness compiles and runs everywhere. It always reports
// unavailable, which the benchmark prints in place of counter figures.
class PerfCounters {
public:
    bool available() const { return false; }
    void start() {}
    void stop() {}
    CounterReading read_counters() { return CounterReading{}; }
};

#endif

}  // namespace obls
