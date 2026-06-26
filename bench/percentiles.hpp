// Exact percentiles from a pre allocated sample buffer.
//
// One latency sample is recorded for every measured operation into a buffer sized up
// front to the known operation count, so recording never allocates on the hot path.
// After the run the percentiles are computed exactly by partial selection over the
// buffer, not by accumulating into a histogram.
//
// Two deliberate choices follow from the study's stance that measurement is the
// product. First, we measure every operation rather than sampling at a fixed offered
// rate, so the coordinated omission that Gil Tene describes, where a stalled load
// generator stops issuing requests during the very stalls that should be recorded, does
// not arise: there is no request rate to fall behind. Second, exact percentiles are
// preferred over histogram approximation because the tail is the point of the study and
// histogram bucketing blurs exactly the p99.9 and p99.99 figures we care about most.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace obls {

struct Percentiles {
    std::size_t count = 0;
    std::uint64_t min = 0;
    std::uint64_t p50 = 0;
    std::uint64_t p90 = 0;
    std::uint64_t p99 = 0;
    std::uint64_t p999 = 0;
    std::uint64_t p9999 = 0;
    std::uint64_t max = 0;
};

class LatencySamples {
public:
    explicit LatencySamples(std::size_t capacity) { samples_.reserve(capacity); }

    // No allocation occurs here as long as the recorded count stays within the reserved
    // capacity, which the harness guarantees by sizing to the operation count.
    void record(std::uint64_t sample) { samples_.push_back(sample); }

    std::size_t size() const { return samples_.size(); }
    bool empty() const { return samples_.empty(); }
    void clear() { samples_.clear(); }

    // Computes the percentile set by repeated nth_element, which partitions the buffer
    // around each requested rank in linear time without a full sort and without any
    // histogram. The buffer is reordered in place because the original recording order
    // carries no information once the run is over.
    Percentiles compute() {
        Percentiles out;
        out.count = samples_.size();
        if (samples_.empty()) {
            return out;
        }
        out.min = select(0);
        out.p50 = select(rank(0.50));
        out.p90 = select(rank(0.90));
        out.p99 = select(rank(0.99));
        out.p999 = select(rank(0.999));
        out.p9999 = select(rank(0.9999));
        out.max = select(samples_.size() - 1);
        return out;
    }

private:
    // Nearest rank method: the smallest sample whose cumulative share reaches the
    // requested fraction. Clamped so the highest fraction maps to the last element.
    std::size_t rank(double fraction) const {
        const double n = static_cast<double>(samples_.size());
        std::size_t idx = static_cast<std::size_t>(std::ceil(fraction * n));
        if (idx > 0) {
            --idx;
        }
        if (idx >= samples_.size()) {
            idx = samples_.size() - 1;
        }
        return idx;
    }

    std::uint64_t select(std::size_t k) {
        auto begin = samples_.begin();
        std::nth_element(begin, begin + static_cast<std::ptrdiff_t>(k), samples_.end());
        return samples_[k];
    }

    std::vector<std::uint64_t> samples_;
};

}  // namespace obls
