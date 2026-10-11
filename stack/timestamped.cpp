#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

template <typename T>
class timestamped_stack {
    static constexpr auto infinity = std::numeric_limits<std::uint64_t>::max();
    static constexpr std::uintptr_t tag_mask = 7; // three-bit ABA tag

    struct timestamp {
        std::uint64_t start;
        std::uint64_t end;
        bool precedes(timestamp other) const { return end < other.start; }
    };

    struct alignas(64) node {
        std::atomic<node*> next{nullptr};
        std::atomic<bool> taken{true};
        std::optional<T> data;
        std::atomic<std::uint64_t> timestamp_start{infinity};
        std::atomic<std::uint64_t> timestamp_end{infinity};

        timestamp read_timestamp() const {
            return {timestamp_start.load(), timestamp_end.load()};
        }
    };

    using pool_top = std::uintptr_t;
    static_assert(std::atomic<pool_top>::is_always_lock_free);

    static node* target(pool_top top) {
        return reinterpret_cast<node*>(top & ~tag_mask);
    }

    static pool_top tagged(node* pointer, pool_top previous, std::uintptr_t increment = 0) {
        return reinterpret_cast<std::uintptr_t>(pointer) | ((previous + increment) & tag_mask);
    }

    struct node_arena {
        std::unique_ptr<node[]> slots;
        std::size_t const capacity;
        std::size_t cursor = 0;

        explicit node_arena(std::size_t count) : slots(std::make_unique<node[]>(count)), capacity(count) {}
        node_arena(node_arena const&) = delete;
        node_arena& operator=(node_arena const&) = delete;

        node* create(T const& value) {
            auto* result = slots.get() + cursor;
            result->data.emplace(value);
            result->timestamp_start.store(infinity);
            result->timestamp_end.store(infinity);
            result->taken.store(false);
            if (++cursor == capacity) cursor = 0;
            return result;
        }
    };

    struct alignas(64) pool {
        node sentinel;
        std::atomic<pool_top> top;
        pool* next = nullptr; // Immutable after registration.
        node_arena arena; // Accessed only by this pool's producer.

        explicit pool(std::size_t capacity) : top(tagged(&sentinel, 0)), arena(capacity) {
            sentinel.next.store(&sentinel);
            sentinel.timestamp_start.store(0);
            sentinel.timestamp_end.store(0);
        }
    };

    struct thread_context {
        pool* producer_pool = nullptr;
        thread_context* next = nullptr;
    };

    struct context_entry { std::uint64_t stack_id; thread_context* context; };
    struct scan_result { node* candidate; pool_top top; };
    struct empty_snapshot {
        pool* source;
        pool_top top;
        bool unchanged() const { return source->top.load() == top; }
    };

public:
    explicit timestamped_stack(std::size_t nodes_per_pool = 1 << 20)
        : arena_capacity(nodes_per_pool) {
        if (nodes_per_pool == 0) throw std::invalid_argument("node arena must be nonempty");
    }
    timestamped_stack(timestamped_stack const&) = delete;
    timestamped_stack& operator=(timestamped_stack const&) = delete;

    ~timestamped_stack() {
        auto* context = contexts.load();
        while (context) {
            auto* next = context->next;
            delete context;
            context = next;
        }
        auto* source = pools.load();
        while (source) {
            auto* next = source->next;
            delete source; // free the arena
            source = next;
        }
    }

    void push(T const& value) {
        auto* inserted = insert(value);
        set_timestamp(*inserted);
    }

    std::optional<T> pop() {
        auto const start_time = new_timestamp();
        std::array<empty_snapshot, 16> empty;
        std::vector<empty_snapshot> extra_empty;

        for (;;) {
            auto* const first_pool = pools.load();
            if (!first_pool) return std::nullopt;
            node* youngest = nullptr;
            timestamp youngest_time{0, 0};
            pool_top youngest_top = 0;
            pool* selected_pool = nullptr;
            std::size_t empty_count = 0;
            extra_empty.clear();
            bool retry = false;

            for (auto* current = first_pool; current; current = current->next) {
                auto const scanned = get_youngest(*current);
                auto* candidate = scanned.candidate;
                if (candidate) {
                    auto const time = candidate->read_timestamp();
                    if (start_time.precedes(time)) {
                        if (remove(*current, scanned.top, candidate)) return *candidate->data;
                        retry = true;
                        break;
                    }
                    if (!youngest || youngest_time.precedes(time)) {
                        youngest = candidate;
                        youngest_time = time;
                        youngest_top = scanned.top;
                        selected_pool = current;
                    }
                } else {
                    empty_snapshot snapshot{current, scanned.top};
                    if (empty_count < empty.size()) empty[empty_count++] = snapshot;
                    else extra_empty.push_back(snapshot);
                }
            }
            if (retry) continue;
            if (youngest) {
                if (remove(*selected_pool, youngest_top, youngest)) return *youngest->data;
                continue;
            }

            for (std::size_t i = 0; i < empty_count; ++i) {
                if (!empty[i].unchanged()) { retry = true; break; }
            }
            if (!retry) {
                for (auto const& snapshot : extra_empty) {
                    if (!snapshot.unchanged()) { retry = true; break; }
                }
            }
            if (!retry && pools.load() == first_pool) return std::nullopt;
        }
    }

private:

    std::size_t const arena_capacity;
    std::atomic<thread_context*> contexts{nullptr};
    std::atomic<pool*> pools{nullptr};
    std::atomic<std::uint64_t> counter{1};
    inline static std::atomic<std::uint64_t> next_stack_id{1};
    std::uint64_t const stack_id = allocate_stack_id();

    static std::uint64_t allocate_stack_id() {
        auto const id = next_stack_id.fetch_add(1);
        if (id == 0) std::terminate();
        return id;
    }

    thread_context& acquire_context() {
        thread_local std::vector<context_entry> cache;
        for (auto const& entry : cache) {
            if (entry.stack_id == stack_id) return *entry.context;
        }
        auto created = std::make_unique<thread_context>();
        cache.push_back({stack_id, created.get()});
        auto* expected = contexts.load();
        do { created->next = expected; }
        while (!contexts.compare_exchange_weak(expected, created.get()));
        return *created.release();
    }

    pool& producer_pool(thread_context& context) {
        if (context.producer_pool) return *context.producer_pool;
        auto created = std::make_unique<pool>(arena_capacity);
        auto* expected = pools.load();
        do { created->next = expected; }
        while (!pools.compare_exchange_weak(expected, created.get()));
        context.producer_pool = created.release();
        return *context.producer_pool;
    }

    node* insert(T const& value) {
        auto& source = producer_pool(acquire_context());
        auto* inserted = source.arena.create(value);
        auto const observed = source.top.load();
        inserted->next.store(target(observed));
        source.top.store(tagged(inserted, observed, 1)); // Single producer; no head CAS.
        trim_next(*inserted);
        return inserted;
    }

    timestamp new_timestamp() {
        auto const start = counter.load();
        if (start >= infinity - 1) std::terminate();
        auto expected = start;
        if (counter.compare_exchange_strong(expected, start + 1)) return {start, start};
        return {start, counter.load() - 1};
    }

    void set_timestamp(node& inserted) {
        auto const time = new_timestamp();
        inserted.timestamp_start.store(time.start);
        inserted.timestamp_end.store(time.end);
    }

    static scan_result get_youngest(pool& source) {
        auto const top = source.top.load();
        auto* candidate = target(top);
        while (candidate->taken.load()) {
            auto* next = candidate->next.load();
            if (next == candidate) return {nullptr, top}; // Self-linked sentinel.
            candidate = next;
        }
        return {candidate, top};
    }

    static void trim_next(node& parent) {
        auto* candidate = parent.next.load();
        while (candidate->next.load() != candidate && candidate->taken.load()) {
            candidate = candidate->next.load();
        }
        parent.next.store(candidate);
    }

    static bool remove(pool& source, pool_top observed, node* candidate) {
        bool expected = false;
        if (!candidate->taken.compare_exchange_strong(expected, true)) return false;
        auto old_top = observed;
        source.top.compare_exchange_strong(old_top, tagged(candidate, observed));
        if (auto* original_top = target(observed); original_top != candidate) {
            original_top->next.store(candidate);
        }
        trim_next(*candidate);
        return true;
    }
};
