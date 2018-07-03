#include <iostream>
#include <stdio.h>
#include <string>
#include <random>
#include <thread>
#include <utility>
#include <array>
#include <limits>

#include "TaskBlock.hpp"

class XorShift1024s
{
    std::array<uint64_t, 16> state;
    size_t p = 0;

public:
    using result_type = uint64_t;

    static constexpr result_type min() {return std::numeric_limits<result_type>::min(); }
    static constexpr result_type max(){return std::numeric_limits<result_type>::max(); }

    XorShift1024s(uint32_t seed = 3191UL)
    {
        std::seed_seq seq{seed};

        seq.generate(state.begin(), state.end());
    }

    result_type operator()()
    {
        const result_type s0 = state[p++];
        result_type s1 = state[p &= 15];
        s1 ^= s1 << 31; // a
        s1 ^= s1 >> 11; // b
        s1 ^= s0 ^ (s0 >> 30); // c
        state[p] = s1;
        return s1 * (result_type)1181783497276652981;
    }

    void discard(unsigned long long n)
    {
        while(n--)
            this->operator()();
    }
};

int main(int argc, char *argv[])
{
    auto begTime = std::chrono::high_resolution_clock::now();

    auto pool1 = std::make_shared<ThreadPool>(12);

    auto printDouble = TaskBlock<void, double>::create([](double time)
    {
        printf("Sorting took %.3fms\n", time);
    }, pool1);

    auto vectorSorter = TaskBlock<double, std::vector<size_t>>::create(printDouble, [](auto &&vec)
    {
        auto begTime = std::chrono::high_resolution_clock::now();
        std::sort(vec.begin(), vec.end());
        auto endTime = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(endTime - begTime).count();
    }, pool1);

    auto vectorShuffler = TaskBlock<std::vector<size_t>, std::vector<size_t>>::create(vectorSorter, [](const auto &vec)
    {
        // static thread_local std::mt19937 engine(std::random_device{}());
        static thread_local XorShift1024s engine;

        auto copy = vec;
        std::shuffle(copy.begin(), copy.end(), engine);
        return copy;
    }, pool1);

    auto doubleMulter = TaskBlock<double, double, double>::create(printDouble, [](double n, double m){return n * m;}, pool1);

    auto postBegTime = std::chrono::high_resolution_clock::now();

    std::vector<size_t> vector;

    vector.resize(100'000);

    const size_t num_items = 100;

    std::shuffle_order_engine<XorShift1024s, 15> engine;

    for (size_t n = 0; n < vector.size(); n++)
        vector[n] = engine();

    vectorShuffler->set_max_queued(1);

    std::uniform_real_distribution<double> dist(0.0, 100.0);

    for(size_t n = 0; n < num_items; n++)
        doubleMulter->post(dist(engine), dist(engine));

    doubleMulter->complete();

    for (size_t n = 0; n < num_items; n++)
    {
        vectorShuffler->post(vector);
    }

    vectorShuffler->complete();

    printDouble->wait_for_completion();

    auto endTime = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration<double, std::milli>(endTime - begTime);

    auto postSetupDuration = std::chrono::duration<double, std::milli>(endTime - postBegTime);

    printf("Completed in %.3fms!\n", duration.count());
    printf("Without setup: %.3fms!\n", postSetupDuration.count());

	return 0;
}
