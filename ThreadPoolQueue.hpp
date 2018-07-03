#pragma once

#include <queue>
#include <mutex>
#include <future>
#include <memory>
#include <condition_variable>
#include <type_traits>
#include <functional>
#include <optional>
#include <atomic>

template <class OutputType>
class ThreadPoolQueue
{
	std::atomic_size_t numQueuedOrReady_ = 0;

    std::vector<std::thread> workerThreads_;
    std::queue<OutputType> outQueue_;
    std::queue<std::function<OutputType()>> fnQueue_;

    std::mutex outQueue_mutex_;
    std::mutex fnQueue_mutex_;

    std::condition_variable outQueue_variable_;
    std::condition_variable fnQueue_variable_;

    bool working_ = true;

    void worker_loop()
    {
        while(true)
        {
            std::function<OutputType()> fn;
            {
                std::unique_lock<std::mutex> lock(fnQueue_mutex_);
				fnQueue_variable_.wait(lock, [this](){ return !this->working_ || !this->fnQueue_.empty(); });

                if(!working_ && fnQueue_.empty())
                    return;

                fn = std::move(fnQueue_.front());
                fnQueue_.pop();
            }
            auto result = fn();
			{
				std::lock_guard<std::mutex> lock(outQueue_mutex_);
				outQueue_.emplace(std::move(result));
			}
			outQueue_variable_.notify_one();
        }
    }

public:

	bool has_more_data() const
	{
		return numQueuedOrReady_ > 0;
	}

    template <class Fn, class... Args>
    void post(Fn &&fn, Args&&... args)
    {
        auto task = std::bind(std::forward<Fn>(fn), std::forward<Args>(args)...);

        {
            std::lock_guard<std::mutex> lock(fnQueue_mutex_);

			fnQueue_.emplace(std::move(task));
        }

		++numQueuedOrReady_;

        fnQueue_variable_.notify_one();
    }

	void push(OutputType &&value)
	{
		{
			std::lock_guard<std::mutex> lock(outQueue_mutex_);
			outQueue_.emplace(std::forward<OutputType>(value));
		}

		++numQueuedOrReady_;

		outQueue_variable_.notify_one();
	}

	std::optional<OutputType> pop()
	{
		std::unique_lock<std::mutex> lock(outQueue_mutex_);
		outQueue_variable_.wait(lock, [this] {return !this->working_ || !this->outQueue_.empty(); });

		if (!this->working_ && this->outQueue_.empty())
			return std::nullopt;

		std::optional<OutputType> value(std::move(outQueue_.front()));

		outQueue_.pop();

		--numQueuedOrReady_;

		return value;
	}

    ThreadPoolQueue(size_t numWorkers)
    {
        for(size_t n = 0; n < numWorkers; n++)
        {
            workerThreads_.emplace_back(&ThreadPoolQueue::worker_loop, this);
        }
    }

    ~ThreadPoolQueue()
    {
        working_ = false;
        
        fnQueue_variable_.notify_all();

        for(auto &worker : workerThreads_)
            worker.join();

		outQueue_variable_.notify_all();
    }
};
