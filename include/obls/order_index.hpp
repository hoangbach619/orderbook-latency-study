// Shared order identity index.
//
// Every book variant locates a resting order by id through this one component. It is
// deliberately identical across variants because the price level container is the only
// independent variable in the study: if the lookup that precedes a cancel or a reduce
// differed between variants, the latency comparison would be measuring two things at
// once.
//
// The map is open addressing with linear probing over a single pre allocated slab. It
// never rehashes, so no operation on the measured path allocates. Capacity is fixed at
// construction and must exceed the peak count of simultaneously live orders in the
// stream; exceeding the load factor is a configuration error, not a runtime condition
// we grow out of.
#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "obls/types.hpp"

namespace obls {

class OrderIndex {
public:
    // A free slot is marked by this id. Real LOBSTER order ids are positive, so the
    // most negative integer can never collide with a live key.
    static constexpr OrderId kEmpty = kInvalidPrice;  // shares the int64 minimum

    // capacity_hint is the expected peak of live orders. The slab is rounded up to the
    // next power of two at no less than twice the hint, keeping the load factor below
    // one half so probe sequences stay short.
    explicit OrderIndex(std::size_t capacity_hint) {
        std::size_t want = next_power_of_two(capacity_hint < 1 ? 1 : capacity_hint) * 2;
        slots_.assign(want, Order{kEmpty, 0, 0, Side::Buy});
        mask_ = want - 1;
        shift_ = 64U - static_cast<unsigned>(std::countr_zero(want));
    }

    // Inserts a fresh order. The id must not already be present; the caller guarantees
    // this because LOBSTER never reuses a live id.
    void insert(const Order& order) {
        assert(size_ * 2 < slots_.size() && "order index load factor exceeded");
        std::size_t i = slot_for(order.id);
        while (slots_[i].id != kEmpty) {
            i = (i + 1) & mask_;
        }
        slots_[i] = order;
        ++size_;
    }

    // Returns a pointer to the stored record, or nullptr when the id is unknown. The
    // pointer is stable until the next erase, which is all the book needs to mutate the
    // remaining quantity in place during a reduce.
    Order* find(OrderId id) {
        std::size_t i = slot_for(id);
        while (slots_[i].id != kEmpty) {
            if (slots_[i].id == id) {
                return &slots_[i];
            }
            i = (i + 1) & mask_;
        }
        return nullptr;
    }

    // Removes an id using Knuth backward shift deletion so the probe sequence stays
    // valid without tombstones. Tombstones would slowly lengthen probes over a full
    // trading day and quietly distort the lookup latency we are trying to measure.
    void erase(OrderId id) {
        std::size_t i = slot_for(id);
        while (slots_[i].id != kEmpty && slots_[i].id != id) {
            i = (i + 1) & mask_;
        }
        if (slots_[i].id == kEmpty) {
            return;
        }
        std::size_t hole = i;
        std::size_t j = i;
        while (true) {
            j = (j + 1) & mask_;
            if (slots_[j].id == kEmpty) {
                break;
            }
            std::size_t home = slot_for(slots_[j].id);
            // Move the probed entry into the hole only when doing so does not jump it
            // past its home slot, which is the condition that keeps every key findable.
            if (in_range(home, hole, j)) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole] = Order{kEmpty, 0, 0, Side::Buy};
        --size_;
    }

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return slots_.size(); }

private:
    static std::size_t next_power_of_two(std::size_t n) {
        std::size_t p = 1;
        while (p < n) {
            p <<= 1;
        }
        return p;
    }

    // Fibonacci hashing. Multiplying by the golden ratio constant moves the well mixed
    // information into the high bits of the product, which are then shifted down to index
    // the table. This matters because LOBSTER ids are sequential rather than random, and a
    // plain modulus would cluster them; taking the high bits instead spreads them evenly.
    std::size_t slot_for(OrderId id) const {
        const std::uint64_t h = static_cast<std::uint64_t>(id) * 0x9E3779B97F4A7C15ULL;
        return static_cast<std::size_t>(h >> shift_);
    }

    static bool in_range(std::size_t home, std::size_t hole, std::size_t cur) {
        // True when home lies in the cyclic interval (hole, cur].
        if (hole <= cur) {
            return home > hole && home <= cur;
        }
        return home > hole || home <= cur;
    }

    std::vector<Order> slots_;
    std::size_t mask_ = 0;
    unsigned shift_ = 0;
    std::size_t size_ = 0;
};

}  // namespace obls
