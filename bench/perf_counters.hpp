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

#if defined(__APPLE__) && defined(__aarch64__)
#include <cstring>  // memcpy, used to convert dlsym results without an old style cast

#include <dlfcn.h>
#include <unistd.h>  // usleep, to let the counters settle before the first read
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

#elif defined(__APPLE__) && defined(__aarch64__)

// Apple Silicon backend, through the private kperf and kperfdata frameworks.
//
// macOS has no perf_event_open, but the kernel exposes the same on core performance monitor
// counters through two private frameworks that are loaded by path at run time. The
// mechanism is adapted from ibireme's public domain kpc_demo and the working mperf
// reference on this machine. We deliberately take the per thread counting path, not the
// whole process PET path: kpc_set_thread_counting plus kpc_get_thread_counters reads the
// calling thread's own accumulated counters, so reading immediately before and after the
// measured loop and subtracting attributes the counts to exactly the work under test. A
// whole process read would fold in framework loading, other threads, and warm up, which
// would make the cache and branch figures meaningless.
//
// The kpc configuration calls require root. Without sudo the force counters probe fails,
// and the backend reports unavailable rather than zeros, so the harness degrades to timing
// only exactly as it does for the steady clock timer fallback.
//
// Five raw events are configured to mirror the Linux backend one for one, so the same
// CounterReading accessors stay correct: cycles, instructions, branches, branch misses, and
// L1 data cache load misses. The branches event is the denominator branch_miss_rate already
// divides by, so it is plumbing for the existing accessor, not a new metric. The retired
// conditional miss event BRANCH_COND_MISPRED_NONSPEC is available on this chip and may
// sharpen the variant C against D contrast in the later writeup, but widening the interface
// is out of scope here.
//
// Opaque kperfdata handles; only ever held as pointers and never dereferenced here.
struct kpep_db;
struct kpep_config;
struct kpep_event;

class PerfCounters {
public:
    PerfCounters() { available_ = configure(); }

    ~PerfCounters() { teardown(); }

    PerfCounters(const PerfCounters&) = delete;
    PerfCounters& operator=(const PerfCounters&) = delete;
    PerfCounters(PerfCounters&&) = delete;
    PerfCounters& operator=(PerfCounters&&) = delete;

    bool available() const { return available_; }

    // start and stop only read the calling thread's counters into pre allocated buffers, so
    // neither allocates nor leaves the measured path to configure anything.
    void start() {
        if (available_) {
            read_thread(baseline_);
        }
    }

    void stop() {
        if (available_) {
            read_thread(current_);
        }
    }

    CounterReading read_counters() {
        CounterReading out;
        if (!available_) {
            return out;
        }
        out.available = true;
        out.cycles = delta(kCycles);
        out.instructions = delta(kInstructions);
        out.branches = delta(kBranches);
        out.branch_misses = delta(kBranchMisses);
        out.cache_misses = delta(kCacheMisses);
        return out;
    }

private:
    // Counter slots, in the order events are added. kpep_config_kpc_map fills counter_map_
    // in that same order, so the enum doubles as the index into counter_map_.
    enum Slot {
        kCycles = 0,
        kInstructions = 1,
        kBranches = 2,
        kBranchMisses = 3,
        kCacheMisses = 4,
        kEventCount = 5
    };

    static constexpr std::uint32_t kFixedMask = 1U << 0;
    static constexpr std::uint32_t kConfigurableMask = 1U << 1;
    static constexpr std::uint32_t kMaxCounters = 32U;

    using kpc_force_all_ctrs_get_fn = int (*)(int*);
    using kpc_force_all_ctrs_set_fn = int (*)(int);
    using kpc_get_counter_count_fn = std::uint32_t (*)(std::uint32_t);
    using kpc_set_config_fn = int (*)(std::uint32_t, std::uint64_t*);
    using kpc_get_thread_counters_fn = int (*)(std::uint32_t, std::uint32_t,
                                               std::uint64_t*);
    using kpc_set_counting_fn = int (*)(std::uint32_t);
    using kpc_set_thread_counting_fn = int (*)(std::uint32_t);
    using kpep_db_create_fn = int (*)(const char*, kpep_db**);
    using kpep_db_free_fn = void (*)(kpep_db*);
    using kpep_db_event_fn = int (*)(kpep_db*, const char*, kpep_event**);
    using kpep_config_create_fn = int (*)(kpep_db*, kpep_config**);
    using kpep_config_free_fn = void (*)(kpep_config*);
    using kpep_config_add_event_fn = int (*)(kpep_config*, kpep_event**, std::uint32_t,
                                             std::uint32_t*);
    using kpep_config_force_counters_fn = int (*)(kpep_config*);
    using kpep_config_kpc_classes_fn = int (*)(kpep_config*, std::uint32_t*);
    using kpep_config_kpc_count_fn = int (*)(kpep_config*, std::size_t*);
    using kpep_config_kpc_map_fn = int (*)(kpep_config*, std::size_t*, std::size_t);
    using kpep_config_kpc_fn = int (*)(kpep_config*, std::uint64_t*, std::size_t);

    // dlsym hands back a void*, and casting that straight to a function pointer trips
    // -Wpedantic. Copying the bits sidesteps the object to function pointer cast cleanly.
    template <typename Fn>
    static Fn load_sym(void* lib, const char* name) {
        void* sym = ::dlsym(lib, name);
        if (sym == nullptr) {
            return nullptr;
        }
        Fn fn = nullptr;
        std::memcpy(&fn, &sym, sizeof(fn));
        return fn;
    }

    bool load_symbols() {
        lib_kperf_ =
            ::dlopen("/System/Library/PrivateFrameworks/kperf.framework/kperf", RTLD_LAZY);
        lib_kperfdata_ = ::dlopen(
            "/System/Library/PrivateFrameworks/kperfdata.framework/kperfdata", RTLD_LAZY);
        if (lib_kperf_ == nullptr || lib_kperfdata_ == nullptr) {
            return false;
        }
        force_get_ =
            load_sym<kpc_force_all_ctrs_get_fn>(lib_kperf_, "kpc_force_all_ctrs_get");
        force_set_ =
            load_sym<kpc_force_all_ctrs_set_fn>(lib_kperf_, "kpc_force_all_ctrs_set");
        counter_count_fn_ =
            load_sym<kpc_get_counter_count_fn>(lib_kperf_, "kpc_get_counter_count");
        set_config_ = load_sym<kpc_set_config_fn>(lib_kperf_, "kpc_set_config");
        get_thread_counters_ =
            load_sym<kpc_get_thread_counters_fn>(lib_kperf_, "kpc_get_thread_counters");
        set_counting_ = load_sym<kpc_set_counting_fn>(lib_kperf_, "kpc_set_counting");
        set_thread_counting_ =
            load_sym<kpc_set_thread_counting_fn>(lib_kperf_, "kpc_set_thread_counting");
        db_create_ = load_sym<kpep_db_create_fn>(lib_kperfdata_, "kpep_db_create");
        db_free_ = load_sym<kpep_db_free_fn>(lib_kperfdata_, "kpep_db_free");
        db_event_ = load_sym<kpep_db_event_fn>(lib_kperfdata_, "kpep_db_event");
        config_create_ =
            load_sym<kpep_config_create_fn>(lib_kperfdata_, "kpep_config_create");
        config_free_ = load_sym<kpep_config_free_fn>(lib_kperfdata_, "kpep_config_free");
        config_add_event_ =
            load_sym<kpep_config_add_event_fn>(lib_kperfdata_, "kpep_config_add_event");
        config_force_counters_ = load_sym<kpep_config_force_counters_fn>(
            lib_kperfdata_, "kpep_config_force_counters");
        config_kpc_classes_ =
            load_sym<kpep_config_kpc_classes_fn>(lib_kperfdata_, "kpep_config_kpc_classes");
        config_kpc_count_ =
            load_sym<kpep_config_kpc_count_fn>(lib_kperfdata_, "kpep_config_kpc_count");
        config_kpc_map_ =
            load_sym<kpep_config_kpc_map_fn>(lib_kperfdata_, "kpep_config_kpc_map");
        config_kpc_ = load_sym<kpep_config_kpc_fn>(lib_kperfdata_, "kpep_config_kpc");
        return force_get_ && force_set_ && counter_count_fn_ && set_config_ &&
               get_thread_counters_ && set_counting_ && set_thread_counting_ &&
               db_create_ && db_free_ && db_event_ && config_create_ && config_free_ &&
               config_add_event_ && config_force_counters_ && config_kpc_classes_ &&
               config_kpc_count_ && config_kpc_map_ && config_kpc_;
    }

    // Resolves an event by name, trying the retired non speculative variant first where one
    // is given, then the speculative fallback, so the honest count is preferred when
    // present.
    kpep_event* find_event(const char* primary, const char* fallback) {
        kpep_event* ev = nullptr;
        if (db_event_(db_, primary, &ev) == 0 && ev != nullptr) {
            return ev;
        }
        if (fallback != nullptr && db_event_(db_, fallback, &ev) == 0 && ev != nullptr) {
            return ev;
        }
        return nullptr;
    }

    bool add_event(const char* primary, const char* fallback) {
        kpep_event* ev = find_event(primary, fallback);
        if (ev == nullptr) {
            return false;
        }
        std::uint32_t err = 0;
        return config_add_event_(config_, &ev, 0U, &err) == 0;
    }

    bool configure() {
        if (!load_symbols()) {
            return false;
        }
        // The force counters probe is the root check: it fails without sudo, before any
        // kernel state is touched, so a non root run degrades cleanly to timing only.
        int forced_state = 0;
        if (force_get_(&forced_state) != 0) {
            return false;
        }
        if (db_create_(nullptr, &db_) != 0 || config_create_(db_, &config_) != 0 ||
            config_force_counters_(config_) != 0) {
            return false;
        }
        // Order matters: counter_map_ is filled in this add order, matching the Slot enum.
        if (!add_event("FIXED_CYCLES", nullptr) ||
            !add_event("FIXED_INSTRUCTIONS", nullptr) ||
            !add_event("INST_BRANCH", nullptr) ||
            !add_event("BRANCH_MISPRED_NONSPEC", nullptr) ||
            !add_event("L1D_CACHE_MISS_LD_NONSPEC", "L1D_CACHE_MISS_LD")) {
            return false;
        }
        if (config_kpc_classes_(config_, &classes_) != 0 ||
            config_kpc_count_(config_, &reg_count_) != 0 ||
            config_kpc_map_(config_, counter_map_, sizeof(counter_map_)) != 0 ||
            config_kpc_(config_, regs_, sizeof(regs_)) != 0) {
            return false;
        }
        if (force_set_(1) != 0) {
            return false;
        }
        forced_ = true;
        if ((classes_ & kConfigurableMask) != 0U && reg_count_ != 0 &&
            set_config_(classes_, regs_) != 0) {
            return false;
        }
        if (set_counting_(classes_) != 0 || set_thread_counting_(classes_) != 0) {
            return false;
        }
        counting_ = true;
        counter_count_ = counter_count_fn_(classes_);
        // Documented kpc quirk: counters can read back zero for a moment right after
        // counting is enabled, so settle briefly before any baseline is taken. This sits in
        // the constructor, never on the measured path.
        ::usleep(10000U);
        return true;
    }

    void teardown() {
        if (counting_) {
            set_counting_(0);
            set_thread_counting_(0);
        }
        if (forced_) {
            force_set_(0);
        }
        if (config_ != nullptr) {
            config_free_(config_);
        }
        if (db_ != nullptr) {
            db_free_(db_);
        }
        if (lib_kperfdata_ != nullptr) {
            ::dlclose(lib_kperfdata_);
        }
        if (lib_kperf_ != nullptr) {
            ::dlclose(lib_kperf_);
        }
    }

    void read_thread(std::uint64_t* buffer) {
        // tid 0 means the calling thread, which is the thread running the measured loop.
        get_thread_counters_(0U, kMaxCounters, buffer);
    }

    std::uint64_t delta(Slot slot) const {
        const std::size_t idx = counter_map_[static_cast<std::size_t>(slot)];
        return current_[idx] - baseline_[idx];
    }

    void* lib_kperf_ = nullptr;
    void* lib_kperfdata_ = nullptr;

    kpc_force_all_ctrs_get_fn force_get_ = nullptr;
    kpc_force_all_ctrs_set_fn force_set_ = nullptr;
    kpc_get_counter_count_fn counter_count_fn_ = nullptr;
    kpc_set_config_fn set_config_ = nullptr;
    kpc_get_thread_counters_fn get_thread_counters_ = nullptr;
    kpc_set_counting_fn set_counting_ = nullptr;
    kpc_set_thread_counting_fn set_thread_counting_ = nullptr;
    kpep_db_create_fn db_create_ = nullptr;
    kpep_db_free_fn db_free_ = nullptr;
    kpep_db_event_fn db_event_ = nullptr;
    kpep_config_create_fn config_create_ = nullptr;
    kpep_config_free_fn config_free_ = nullptr;
    kpep_config_add_event_fn config_add_event_ = nullptr;
    kpep_config_force_counters_fn config_force_counters_ = nullptr;
    kpep_config_kpc_classes_fn config_kpc_classes_ = nullptr;
    kpep_config_kpc_count_fn config_kpc_count_ = nullptr;
    kpep_config_kpc_map_fn config_kpc_map_ = nullptr;
    kpep_config_kpc_fn config_kpc_ = nullptr;

    kpep_db* db_ = nullptr;
    kpep_config* config_ = nullptr;

    std::uint32_t classes_ = 0;
    std::size_t reg_count_ = 0;
    std::uint32_t counter_count_ = 0;
    std::uint64_t regs_[kMaxCounters] = {};
    std::size_t counter_map_[kMaxCounters] = {};
    std::uint64_t baseline_[kMaxCounters] = {};
    std::uint64_t current_[kMaxCounters] = {};

    bool forced_ = false;
    bool counting_ = false;
    bool available_ = false;
};

#else  // neither Linux nor Apple Silicon

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
