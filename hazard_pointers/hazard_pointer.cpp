#include "hazard_pointer.hpp"

#include <new>
#include <stdexcept>
#include <thread>

constexpr unsigned int max_hazard_pointers = 100;

struct alignas(std::hardware_destructive_interference_size) hazard_pointer {
    std::atomic<std::thread::id> id{};
    std::atomic<void*> pointer{nullptr};
};
hazard_pointer hazard_pointers[max_hazard_pointers];

class hp_owner {
    hazard_pointer* hp;

public:
    hp_owner(hp_owner const&) = delete; // no copy constructor
    hp_owner& operator=(hp_owner const&)=delete;
    hp_owner() : hp(nullptr) {
        for (unsigned int i = 0; i < max_hazard_pointers; ++i) {
            std::thread::id old_id;
            if (hazard_pointers[i].id.compare_exchange_strong(old_id, std::this_thread::get_id(),
                                                            std::memory_order_acquire,
                                                            std::memory_order_relaxed)) {
                hp = &hazard_pointers[i];
                break;
            }
        }
        if (!hp)
            throw std::runtime_error("No hazard pointers available");
    }

    std::atomic<void*>& get_pointer() {
        return hp->pointer;
    }

    ~hp_owner() {
        hp->pointer.store(nullptr, std::memory_order_release);
        hp->id.store(std::thread::id(), std::memory_order_release);
    }
};

std::atomic<void*>& get_hazard_pointer_for_current_thread() {
    thread_local static hp_owner hazard; // static so one persistent instance per thread
    return hazard.get_pointer();
}

bool outstanding_hazard_pointers_for(void* p) {
    for (unsigned int i = 0; i < max_hazard_pointers; i++) {
        // Pair with SC hazard publication, head validation and removal.
        if (hazard_pointers[i].pointer.load(std::memory_order_seq_cst) == p) return true;
    }
    return false;
}


std::atomic<data_to_reclaim*> nodes_to_reclaim{nullptr};

void add_to_reclaim_list(data_to_reclaim* node) {
    node->next = nodes_to_reclaim.load(std::memory_order_relaxed);
    while (!nodes_to_reclaim.compare_exchange_weak(node->next, node,
                                                 std::memory_order_release,
                                                 std::memory_order_relaxed)) {} // CAS loop to add to free list
}

void delete_nodes_with_no_hazards() {
    data_to_reclaim* current = nodes_to_reclaim.exchange(nullptr, std::memory_order_acquire); // claim free list
    while (current) {
        data_to_reclaim* const next = current->next;
        if (!outstanding_hazard_pointers_for(current->data)) {
            delete current;
        }
        else {
            add_to_reclaim_list(current);
        }
        current = next;
    }
}
