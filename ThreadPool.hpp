#include <queue>
#include <mutex>
#include <future>
#include <memory>
#include <condition_variable>
#include <type_traits>
#include <functional>

class ThreadPool
{
	std::vector<std::thread> workerThreads_;
	std::queue<std::function<void()>> fnQueue_;

    std::mutex fnQueue_mutex_;

	std::condition_variable fnQueue_variable_;

	bool working_ = true;

    std::atomic_size_t num_queued_or_running_ = 0;

	void worker_loop()
	{
		while (true)
		{
			std::function<void()> fn;
			{
				std::unique_lock<std::mutex> lock(fnQueue_mutex_);
				fnQueue_variable_.wait(lock, [this]() { return !this->working_ || !this->fnQueue_.empty(); });

				if (!working_ && fnQueue_.empty())
					return;

				fn = std::move(fnQueue_.front());
				fnQueue_.pop();
			}
			fn();
            --num_queued_or_running_;
		}
	}

public:
	template <class Fn, class... Args>
	std::future<std::invoke_result_t<Fn, Args...>> post(Fn &&fn, Args&&... args)
	{
		using ReturnType = std::invoke_result_t<Fn, Args...>;

		auto promise = std::make_shared<std::promise<ReturnType>>();
		{
            std::lock_guard<std::mutex> lock(fnQueue_mutex_);

			if constexpr (std::is_void_v<ReturnType>)
			{
                fnQueue_.emplace(
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

                fnQueue_.emplace(
                    [
                        promise,
                        func = std::forward<Fn>(fn),
                        args = std::make_tuple(std::forward<Args>(args)...)
                    ]() mutable
                {
                    promise->set_value(std::apply(func, std::move(args)));
                });
			}
		}

		fnQueue_variable_.notify_one();

        ++num_queued_or_running_;

		return promise->get_future();
	}

    template <class Fn, class... Args>
    void post_no_future(Fn &&fn, Args&&... args)
    {
        {
            std::lock_guard<std::mutex> lock(fnQueue_mutex_);

            fnQueue_.emplace([func = std::forward<Fn>(fn),
                              args = std::make_tuple(std::forward<Args>(args)...)]() mutable
            {
                std::apply(func, std::move(args));
            });
        }
        ++num_queued_or_running_;

        fnQueue_variable_.notify_one();
    }

	ThreadPool(size_t numWorkers)
	{
		for (size_t n = 0; n < numWorkers; n++)
			workerThreads_.emplace_back(&ThreadPool::worker_loop, this);
	}

	~ThreadPool()
	{
		working_ = false;

		fnQueue_variable_.notify_all();

		for (auto &worker : workerThreads_)
			worker.join();
	}
};
