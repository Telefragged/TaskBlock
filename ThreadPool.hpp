#pragma once

#include <queue>
#include <mutex>
#include <future>
#include <memory>
#include <condition_variable>
#include <type_traits>
#include <functional>

#include "blockingconcurrentqueue.h"

class ThreadPool
{
	std::vector<std::thread> workerThreads_;

	moodycamel::BlockingConcurrentQueue<std::function<void()>> fnQueue_;

	bool working_ = true;

    std::atomic_size_t num_queued_or_running_ = 0;

	void worker_loop()
	{
		while (true)
		{
			std::function<void()> fn;

			if (!working_ && fnQueue_.size_approx() == 0)
				return;

            bool got_value = false;

            if (!fnQueue_.try_dequeue(fn))
                while (working_ && !got_value)
                    got_value = fnQueue_.wait_dequeue_timed(fn, std::chrono::milliseconds(10));

            else
                got_value = true;

            if (!got_value)
                return;

			fn();
            --num_queued_or_running_;
		}
	}

public:
	template <class Fn, class... Args>
	std::future<std::invoke_result_t<Fn, Args...>> post(Fn &&fn, Args&&... args)
	{
		using ReturnType = std::invoke_result_t<Fn, Args...>;

        if (working_ == false)
            throw std::exception();

		auto promise = std::make_shared<std::promise<ReturnType>>();

		if constexpr (std::is_void_v<ReturnType>)
		{
            fnQueue_.enqueue(
            [
                promise,
                func = std::forward<Fn>(fn),
                args = std::make_tuple(std::forward<Args>(args)...)
            ]() mutable
            {
                std::apply(func, std::move(args));
                promise->set_value();
            });
		}
		else
		{

            fnQueue_.enqueue(
                [
                    promise,
                    func = std::forward<Fn>(fn),
                    args = std::make_tuple(std::forward<Args>(args)...)
                ]() mutable
            {
                promise->set_value(std::apply(func, std::move(args)));
            });
		}

        ++num_queued_or_running_;

		return promise->get_future();
	}

    template <class Fn, class... Args>
    void post_no_future(Fn &&fn, Args&&... args)
    {
        if (working_ == false)
            return;

        fnQueue_.enqueue([func = std::forward<Fn>(fn),
                            args = std::make_tuple(std::forward<Args>(args)...)]() mutable
        {
            std::apply(func, std::move(args));
        });

        ++num_queued_or_running_;
    }

	ThreadPool(size_t numWorkers)
	{
		for (size_t n = 0; n < numWorkers; n++)
			workerThreads_.emplace_back(&ThreadPool::worker_loop, this);
	}

	~ThreadPool()
	{
		working_ = false;

        for (auto &worker : workerThreads_)
            worker.join();
	}
};
