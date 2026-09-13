// order_pool.hpp - slab allocator with a free list for Order nodes.
//
// WHY: a day of AMZN has ~270k messages; MSFT ~670k. malloc/free per order is
// ~50-100ns each and fragments the heap. A pool hands out nodes from large
// contiguous slabs (good locality) and recycles freed nodes through a singly
// linked free list threaded through the unused `next` pointer: O(1) both ways.
//
// POINTER STABILITY: slabs are never moved or shrunk, so an Order* stays valid
// for the pool's lifetime. That is essential because the hash index and the
// intrusive list both hold raw pointers.
#pragma once
#include <cstddef>
#include <memory>
#include <vector>

#include "lob/order.hpp"

namespace lob {

class OrderPool {
public:
    explicit OrderPool(std::size_t slab_size = 1 << 16) : slab_size_(slab_size) {}

    OrderPool(const OrderPool&)            = delete;
    OrderPool& operator=(const OrderPool&) = delete;

    Order* alloc() {
        if (!free_) grow();
        Order* o = free_;
        free_    = o->next;
        *o       = Order{};  // reset all fields
        ++live_;
        return o;
    }

    void release(Order* o) noexcept {
        o->next = free_;
        free_   = o;
        --live_;
    }

    std::size_t live() const noexcept { return live_; }
    std::size_t capacity() const noexcept { return slabs_.size() * slab_size_; }

private:
    void grow() {
        slabs_.push_back(std::make_unique<Order[]>(slab_size_));
        Order* slab = slabs_.back().get();
        // Thread the new slab onto the free list (in reverse so alloc() walks
        // forward through memory, which is friendlier to the prefetcher).
        for (std::size_t i = slab_size_; i-- > 0;) {
            slab[i].next = free_;
            free_        = &slab[i];
        }
    }

    std::size_t                           slab_size_;
    std::vector<std::unique_ptr<Order[]>> slabs_;
    Order*                                free_{nullptr};
    std::size_t                           live_{0};
};

}  // namespace lob
