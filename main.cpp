#include <iostream>
#include <stdio.h>
#include <string>
#include <random>
#include <thread>
#include <utility>

#include "TaskBlock.hpp"

int main(int argc, char *argv[])
{
    auto begTime = std::chrono::high_resolution_clock::now();

    auto pool1 = std::make_shared<ThreadPool>(12);

    auto vectorCheckSorted = TaskBlock<void, double>::create([](double time)
    {
        printf("Sorting took %.3fms\n", time);
    }, pool1);

    auto vectorSorter = TaskBlock<double, std::vector<int>>::create(vectorCheckSorted, [](std::vector<int> &&vec)
    {
        auto begTime = std::chrono::high_resolution_clock::now();
        std::sort(vec.begin(), vec.end());
        auto endTime = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(endTime - begTime).count();
    }, pool1);

    auto vectorShuffler = TaskBlock<std::vector<int>, std::vector<int>>::create(vectorSorter, [](const std::vector<int> &vec)
    {
        static thread_local std::mt19937 engine(std::random_device{}());
        auto copy = vec;
        std::shuffle(copy.begin(), copy.end(), engine);
        return copy;
    }, pool1);

    auto postBegTime = std::chrono::high_resolution_clock::now();

    std::vector<int> vector;

    vector.resize(10'000'000);

    const size_t num_items = 20;

    for (size_t n = 0; n < vector.size(); n++)
        vector[n] = n + 1;

    vectorShuffler->set_max_queued(1);

    for (size_t n = 0; n < num_items; n++)
    {
        vectorShuffler->post(vector);
    }

    vectorShuffler->complete();

    vectorCheckSorted->wait_for_completion();

    auto endTime = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration<double, std::milli>(endTime - begTime);

    auto postSetupDuration = std::chrono::duration<double, std::milli>(endTime - postBegTime);

    printf("Completed in %.3fms!\n", duration.count());
    printf("Without setup: %.3fms!\n", postSetupDuration.count());

	return 0;
}
