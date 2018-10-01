#include <iostream>
#include <numeric>
#include <stdio.h>
#include <string>
#include <random>
#include <thread>
#include <utility>
#include <array>
#include <limits>
#include <cstddef>
#include <queue>

#include "TaskBlock.hpp"

int main(int argc, char *argv[])
{
    auto pool = std::make_shared<ThreadPool>(1);

    using tp = std::chrono::high_resolution_clock::time_point;

    auto block = TaskBlock<tp, tp>::create([](tp val) {
        return std::chrono::high_resolution_clock::now();
    }, pool);

    auto block2 = TaskBlock<std::string, tp>::create([](tp b) {
        return std::to_string((std::chrono::high_resolution_clock::now() - b).count());
    }, pool);

    auto block3 = TaskBlock<void, std::string>::create([](std::string str) {
        std::cout << str << " nanoseconds\n";
    });

    block->add_output_block(block2);

    block2->add_output_block(block3);

    block->post(std::chrono::high_resolution_clock::now());

    block->post(std::chrono::high_resolution_clock::now());
    block->post(std::chrono::high_resolution_clock::now());
    block->post(std::chrono::high_resolution_clock::now());
    block->post(std::chrono::high_resolution_clock::now());
    block->post(std::chrono::high_resolution_clock::now());
    block->post(std::chrono::high_resolution_clock::now());
    block->post(std::chrono::high_resolution_clock::now());

    return 0;
}
