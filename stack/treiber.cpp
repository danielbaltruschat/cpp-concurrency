#include <atomic>
#include <memory>
#include <folly/synchronization/Hazptr.h>
#include "../hazard_pointers/hazard_pointer.hpp"

namespace {
    template <typename T>
    struct node {
        std::unique_ptr<T> data;
        node* next;
        node* reclaim_next{nullptr};

        node(T const& val) : data(std::make_unique<T>(val)) {}
    };
}

template <typename T>
struct lock_free_stack_leaking {
    ~lock_free_stack_leaking() {
        auto* current = head.load();
        while (current) {
            auto* next = current->next;
            delete current;
            current = next;
        }
    }

    void push(T const& val) {
        auto* new_node = new node<T>(val);
        new_node->next = head.load(std::memory_order_relaxed);
        while (!head.compare_exchange_weak(new_node->next, new_node,
                                          std::memory_order_release,
                                          std::memory_order_relaxed)) {}
    }

    std::unique_ptr<T> pop() { // deliberately leak old_head for now
        auto* old_head = head.load(std::memory_order_acquire);
        while (old_head && !head.compare_exchange_weak(old_head, old_head->next,
                                                     std::memory_order_acquire,
                                                     std::memory_order_acquire)) {}
        return old_head ? std::move(old_head->data) : nullptr;
    }

private:
    std::atomic<node<T>*> head{nullptr};
};

template <typename T>
struct lock_free_stack_keep_thread_count {
    ~lock_free_stack_keep_thread_count() {
        while (pop()) {}
    }

    void push(T const& val) {
        auto* new_node = new node<T>(val);
        new_node->next = head.load();
        while (!head.compare_exchange_weak(new_node->next, new_node)) {}
    }

    std::unique_ptr<T> pop() {
        ++threads_in_pop;
        auto* old_head = head.load();
        while (old_head && !head.compare_exchange_weak(old_head, old_head->next)) {}
        std::unique_ptr<T> result;
        if (old_head) result.swap(old_head->data);
        try_reclaim(old_head);
        return result;
    }

private:
    std::atomic<node<T>*> head{nullptr};
    std::atomic<unsigned int> threads_in_pop{0};
    std::atomic<node<T>*> freelist_head{nullptr};

    void try_reclaim(node<T>* old_head) {
        if (threads_in_pop.load() == 1) {
            node<T>* free_list = freelist_head.exchange(nullptr);
            if (!--threads_in_pop) { // only thread in pop?
                delete_nodes(free_list);
            }
            else if (free_list) { 
                append_pending_nodes(free_list);
            }
            delete old_head;
        }
        else {
            if (old_head) append_pending_node(old_head);
            --threads_in_pop;
        }
    }

    static void delete_nodes(node<T>* nodes) {
        while (nodes) {
            node<T>* next = nodes->reclaim_next;
            delete nodes;
            nodes = next;
        }
    }

    void append_pending_nodes(node<T>* nodes) {
        node<T>* last = nodes;
        while (node<T>* const next = last->reclaim_next) {
            last = next;
        }
        chain_pending_nodes(nodes, last);
    }

    void chain_pending_nodes(node<T>* first, node<T>* last) {
        last->reclaim_next = freelist_head.load(); // chain freelist to end

        while (!freelist_head.compare_exchange_weak(last->reclaim_next, first)) {}
    }

    void append_pending_node(node<T>* node) {
        chain_pending_nodes(node, node);
    }
};

template <typename T>
struct lock_free_stack_hazard_pointer_simple {
    ~lock_free_stack_hazard_pointer_simple() {
        auto* current = head.load();
        while (current) {
            auto* next = current->next;
            delete current;
            current = next;
        }
        delete_nodes_with_no_hazards();
    }

    void push(T const& val) {
        auto* new_node = new node<T>(val);
        new_node->next = head.load(std::memory_order_relaxed);
        while (!head.compare_exchange_weak(new_node->next, new_node,
                                          std::memory_order_release,
                                          std::memory_order_relaxed)) {}
    }

    std::unique_ptr<T> pop() {
        // set hazard pointer
        auto& hp = get_hazard_pointer_for_current_thread();
        auto* old_head = head.load(std::memory_order_relaxed);
        do {
            node<T>* temp;
            do {
                temp = old_head;
                // SC operations coordinate with removal and hazard scans
                hp.store(old_head, std::memory_order_seq_cst);
                old_head = head.load(std::memory_order_seq_cst);
            } while (old_head != temp);
        } while (old_head && !head.compare_exchange_weak(old_head, old_head->next,
                                                       std::memory_order_seq_cst,
                                                       std::memory_order_relaxed));
        hp.store(nullptr, std::memory_order_release);
        std::unique_ptr<T> res;
        if (old_head) {
            // try delete
            res.swap(old_head->data);
            if (outstanding_hazard_pointers_for(old_head)) {
                reclaim_later(old_head);
            }
            else {
                delete old_head;
            }
        }
        delete_nodes_with_no_hazards();
        return res;
    }

private:
    std::atomic<node<T>*> head{nullptr};
};

namespace {
    template <typename T>
    struct folly_node : folly::hazptr_obj_base<folly_node<T>> {
        std::unique_ptr<T> data;
        folly_node* next;
        folly_node* reclaim_next{nullptr};

        explicit folly_node(T const& val) : data(std::make_unique<T>(val)) {}
    };
}

template <typename T>
struct lock_free_stack_folly {
    ~lock_free_stack_folly() {
        auto* current = head.load();
        while (current) {
            auto* next = current->next;
            delete current;
            current = next;
        }
    }

    void push(T const& val) {
        auto* new_node = new folly_node<T>(val);
        new_node->next = head.load(std::memory_order_relaxed);
        while (!head.compare_exchange_weak(new_node->next, new_node,
                                          std::memory_order_release,
                                          std::memory_order_relaxed)) {}
    }

    std::unique_ptr<T> pop() {
        auto hp = folly::make_hazard_pointer();
        auto* old_head = head.load(std::memory_order_relaxed);
        do {
            // folly provides acquire validation and the reclamation fences
            while (!hp.try_protect(old_head, head)) {}
        } while (old_head && !head.compare_exchange_weak(old_head, old_head->next,
                                                       std::memory_order_relaxed,
                                                       std::memory_order_relaxed));

        std::unique_ptr<T> result;
        if (old_head) result.swap(old_head->data);
        hp.reset_protection();
        if (old_head) old_head->retire();
        return result;
    }

private:
    std::atomic<folly_node<T>*> head{nullptr};
};
