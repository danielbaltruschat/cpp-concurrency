#include <memory>
#include <mutex>
#include <utility>

template <typename T>
struct mutex_stack {
    mutex_stack() = default;
    mutex_stack(mutex_stack const&) = delete;
    mutex_stack& operator=(mutex_stack const&) = delete;

    ~mutex_stack() {
        while (head) {
            auto* old_head = head;
            head = old_head->next;
            delete old_head;
        }
    }

    void push(T const& val) {
        auto new_node = std::make_unique<node>(val);
        std::scoped_lock lock(mutex);
        new_node->next = head;
        head = new_node.release();
    }

    std::unique_ptr<T> pop() {
        std::unique_ptr<node> old_head;
        {
            std::scoped_lock lock(mutex);
            if (!head) return nullptr;

            old_head.reset(head);
            head = old_head->next;
        }
        return std::move(old_head->data);
    }

private:
    struct node {
        std::unique_ptr<T> data;
        node* next{nullptr};

        explicit node(T const& val) : data(std::make_unique<T>(val)) {}
    };

    node* head{nullptr};
    std::mutex mutex;
};
