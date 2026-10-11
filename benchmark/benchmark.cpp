#include <benchmark/benchmark.h>

#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <vector>

#include "../stack/treiber.cpp"
#include "../stack/mutex_stack.cpp"
#include "../stack/elimination_backoff.cpp"
#include "../stack/timestamped.cpp"

std::uint64_t run_seed;
bool sample_export_failed = false;

struct alignas(std::hardware_destructive_interference_size) operation_samples {
    std::vector<std::int64_t> pushes;
    std::vector<std::int64_t> pops;
};

void write_samples(char const* stack_name, std::int64_t wait,
                   std::vector<operation_samples> const& samples) {
    static_assert(std::endian::native == std::endian::little);
    auto const* configured = std::getenv("STACK_BENCHMARK_SAMPLES_DIR");
    std::filesystem::path directory(configured ? configured : "benchmark/samples");
    std::filesystem::create_directories(directory);
    auto const stem = std::string(stack_name) + "_wait_" + std::to_string(wait);
    std::ofstream push_file(directory / (stem + ".push.bin"), std::ios::binary);
    std::ofstream pop_file(directory / (stem + ".pop.bin"), std::ios::binary);
    push_file.exceptions(std::ios::failbit | std::ios::badbit);
    pop_file.exceptions(std::ios::failbit | std::ios::badbit);
    for (auto const& thread : samples) {
        push_file.write(reinterpret_cast<char const*>(thread.pushes.data()),
                        static_cast<std::streamsize>(thread.pushes.size() * sizeof(std::int64_t)));
        pop_file.write(reinterpret_cast<char const*>(thread.pops.data()),
                       static_cast<std::streamsize>(thread.pops.size() * sizeof(std::int64_t)));
    }
    push_file.close();
    pop_file.close();
}

template <typename Stack>
void BM_RandomPushPopRandomWait(benchmark::State& state, char const* stack_name) {
    static std::unique_ptr<Stack> stack;
    static std::vector<operation_samples> samples;
    if (state.thread_index() == 0) {
        stack = std::make_unique<Stack>();
        samples.assign(state.threads(), {});
        for (auto& thread : samples) {
            thread.pushes.reserve(state.max_iterations);
            thread.pops.reserve(state.max_iterations);
        }
        for (int i = 0; i < 10000; ++i) stack->push(i);
    }

    std::mt19937_64 operations(run_seed + 2 * state.thread_index());
    std::mt19937_64 delays(run_seed + 2 * state.thread_index() + 1);
    std::bernoulli_distribution choose_push(0.5);
    std::uniform_int_distribution<std::int64_t> wait_ns(0, state.range(0));
    std::int64_t empty_pops = 0;

    for (auto _ : state) {
        std::chrono::nanoseconds elapsed;
        auto& thread = samples[state.thread_index()];
        if (choose_push(operations)) {
            auto const start = std::chrono::steady_clock::now();
            stack->push(state.thread_index());
            elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start);
            thread.pushes.push_back(elapsed.count());
        } else {
            decltype(stack->pop()) value;
            auto const start = std::chrono::steady_clock::now();
            value = stack->pop();
            elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start);
            thread.pops.push_back(elapsed.count());
            if (value) benchmark::DoNotOptimize(*value);
            else ++empty_pops;
        }
        state.SetIterationTime(std::chrono::duration<double>(elapsed).count());
        auto const delay = std::chrono::nanoseconds(wait_ns(delays));
        if (delay.count() != 0) {
            auto const deadline = std::chrono::steady_clock::now() + delay;
            while (std::chrono::steady_clock::now() < deadline) {}
        }
    }
    state.counters["empty_pops"] = static_cast<double>(empty_pops);
    if (state.thread_index() == 0) {
        try {
            write_samples(stack_name, state.range(0), samples);
        } catch (std::exception const& error) {
            sample_export_failed = true;
            state.SkipWithError(error.what());
        }
        samples.clear();
        stack.reset();
    }
}

struct stack_benchmark {
    char const* name;
    void (*run)(benchmark::State&, char const*);
};

constexpr stack_benchmark stacks[] = {
    {"lock_free_stack_leaking", BM_RandomPushPopRandomWait<lock_free_stack_leaking<int>>},
    {"lock_free_stack_keep_thread_count", BM_RandomPushPopRandomWait<lock_free_stack_keep_thread_count<int>>},
    {"lock_free_stack_hazard_pointer_simple", BM_RandomPushPopRandomWait<lock_free_stack_hazard_pointer_simple<int>>},
    {"lock_free_stack_folly", BM_RandomPushPopRandomWait<lock_free_stack_folly<int>>},
    {"ebs_stack_folly", BM_RandomPushPopRandomWait<ebs_stack_folly<int>>},
    {"timestamped_stack", BM_RandomPushPopRandomWait<timestamped_stack<int>>},
    {"mutex_stack", BM_RandomPushPopRandomWait<mutex_stack<int>>},
};
constexpr std::int64_t wait_bounds[] = {0, 100, 1000, 10000};
constexpr int worker_threads = 10;
constexpr std::int64_t iterations_per_thread = 1000000;

int main(int argc, char** argv) {
    for (auto const& stack : stacks) {
        auto* registration = benchmark::RegisterBenchmark(
            std::string("BM_RandomPushPopRandomWait<") + stack.name + "<int>>",
            [stack](benchmark::State& state) { stack.run(state, stack.name); });
        registration->ArgName("max_wait_ns");
        for (auto wait : wait_bounds) registration->Arg(wait);
        registration->Threads(worker_threads)->Iterations(iterations_per_thread)->UseManualTime();
    }

    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    try {
        auto const* configured = std::getenv("STACK_BENCHMARK_SEED");
        std::random_device source;
        run_seed = configured ? std::stoull(configured) : source();
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return sample_export_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
