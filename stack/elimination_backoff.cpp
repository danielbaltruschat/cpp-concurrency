#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include <folly/synchronization/Hazptr.h>
#include <folly/portability/Asm.h>

template <typename T, std::size_t EliminationSlots = 16, std::size_t WaitSpins = 64,
          std::size_t MaxWaitSpins = 1024>
class ebs_stack_folly {
    static_assert(EliminationSlots > 0, "An elimination array needs at least one slot");
    static_assert(WaitSpins <= MaxWaitSpins, "Initial wait must not exceed the backoff cap");

    struct node : folly::hazptr_obj_base<node> {
        std::unique_ptr<T> data;
        node* next{nullptr};

        explicit node(T const& value) : data(std::make_unique<T>(value)) {}
    };

    // stop false sharing
    struct alignas(std::hardware_destructive_interference_size) elimination_slot {
        std::atomic<node*> offer{nullptr};
    };

    class backoff {
        std::size_t window = WaitSpins;

    public:
        std::size_t wait_spins() const {
            return window == 0 ? 0 : 1 + next_random() % window;
        }

        void failed_attempt() {
            window = window > MaxWaitSpins / 2 ? MaxWaitSpins : window * 2;
        }
    };

public:
    ebs_stack_folly() = default;
    ebs_stack_folly(ebs_stack_folly const&) = delete;
    ebs_stack_folly& operator=(ebs_stack_folly const&) = delete;

    ~ebs_stack_folly() {
        auto* current = head.load();
        while (current) {
            auto* next = current->next;
            delete current;
            current = next;
        }
    }

    void push(T const& value) {
        auto new_node = std::make_unique<node>(value);
        std::optional<folly::hazptr_holder<>> hp;
        backoff retry;
        auto* expected = head.load();
        while (true) {
            new_node->next = expected;
            if (head.compare_exchange_weak(expected, new_node.get())) {
                new_node.release();
                return;
            }
            if (!hp) hp.emplace(folly::make_hazard_pointer());
            if (try_eliminate_push(new_node, *hp, retry.wait_spins())) return;
            retry.failed_attempt();
            expected = head.load();
        }
    }

    std::unique_ptr<T> pop() {
        auto hp = folly::make_hazard_pointer();
        while (true) {
            auto* expected = hp.protect(head);
            if (!expected) {
                return try_eliminate_pop(hp);
            }
            auto* removed = expected;
            if (head.compare_exchange_weak(expected, removed->next)) {
                auto result = std::move(removed->data);
                hp.reset_protection();
                removed->retire();
                return result;
            }
            hp.reset_protection();
            if (auto result = try_eliminate_pop(hp)) return result;
        }
    }

private:
    std::atomic<node*> head{nullptr};
    std::array<elimination_slot, EliminationSlots> slots{};

    static std::uint64_t next_random() {
        thread_local std::uint64_t state =
            static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())) | 1;
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }

    static std::size_t random_slot() {
        return next_random() % EliminationSlots;
    }

    bool try_eliminate_push(std::unique_ptr<node>& offered, folly::hazptr_holder<>& hp,
                            std::size_t wait_spins) {
        auto& slot = slots[random_slot()].offer;
        auto* candidate = offered.get();
        hp.reset_protection(candidate);
        node* empty = nullptr;
        if (!slot.compare_exchange_strong(empty, candidate)) return false;

        for (std::size_t spin = 0; spin < wait_spins; ++spin) {
            if (slot.load() != candidate) {
                offered.release();
                return true;
            }
            folly::asm_volatile_pause();
        }

        auto* expected = candidate;
        if (slot.compare_exchange_strong(expected, nullptr)) {
            return false;
        }
        offered.release();
        return true;
    }

    std::unique_ptr<T> try_eliminate_pop(folly::hazptr_holder<>& hp) {
        auto const start = random_slot();
        for (std::size_t offset = 0; offset < EliminationSlots; ++offset) {
            auto& slot = slots[(start + offset) % EliminationSlots].offer;
            auto* candidate = slot.load();
            if (!candidate || !hp.try_protect(candidate, slot)) continue;
            auto* claimed = candidate;
            if (slot.compare_exchange_strong(candidate, nullptr)) {
                auto result = std::move(claimed->data);
                hp.reset_protection();
                claimed->retire();
                return result;
            }
        }
        hp.reset_protection();
        return nullptr;
    }
};
