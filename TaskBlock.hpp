#pragma once

#include <queue>
#include <memory>
#include <mutex>

#include "ThreadPool.hpp"

template <class OutputType, class... InputTypes>
class TaskBlock
{
	using transform_fn_type = std::function<OutputType(InputTypes...)>;
	using internal_fn_type = std::function<void(InputTypes...)>;

	std::shared_ptr<ThreadPool> pool_;

	transform_fn_type transform_fn_;
	internal_fn_type fn_;

    std::function<void()> complete_fn_;

	std::unique_ptr<std::queue<OutputType>> queue_;

    std::mutex post_mutex_;
	std::mutex queue_mutex_;
    std::mutex completion_mutex_;

    std::condition_variable post_variable_;
	std::condition_variable queue_variable_;
    std::condition_variable completion_variable_;

    std::atomic_size_t max_queued_ = 0;
    std::atomic_size_t num_queued_ = 0;
    std::atomic_size_t num_queued_or_running_ = 0;

    bool completion_signaled_ = false;

    bool decrement_and_check_completion()
    {
        size_t queued_or_running = --num_queued_or_running_;
        bool isComplete = completion_signaled_ && queued_or_running == 0;

        if (isComplete)
            complete_fn_();

        return isComplete;
    }

    void on_function_enter()
    {
        --num_queued_;
        post_variable_.notify_one();
    }

    void on_function_done()
    {
        decrement_and_check_completion();
    }

    void signal_completion()
    {
        completion_variable_.notify_all();
    }

    TaskBlock(transform_fn_type &&transform_fn, std::shared_ptr<ThreadPool> pool = std::make_shared<ThreadPool>(1)) :
        pool_(pool),
        transform_fn_(std::forward<transform_fn_type>(transform_fn)),
        fn_([this](InputTypes&&... values)
        {
            this->on_function_enter();
            OutputType val = this->transform_fn_(std::forward<InputTypes>(values)...);
            {
                std::lock_guard<std::mutex> lock(this->queue_mutex_);
                this->queue_->emplace(std::move(val));
            }
            this->queue_variable_.notify_one();
            this->on_function_done();
        }),
        complete_fn_([this] { this->signal_completion(); })
    {
        queue_ = std::make_unique<std::queue<OutputType>>();
    }

    template <class OtherOutputType>
    TaskBlock(std::shared_ptr<TaskBlock<OtherOutputType, OutputType>> nextBlock,
              transform_fn_type &&transform_fn,
              std::shared_ptr<ThreadPool> pool = std::make_shared<ThreadPool>(1)) :
        pool_(pool),
        transform_fn_(std::forward<transform_fn_type>(transform_fn)),
        fn_([this, nextBlock](InputTypes&&... values) mutable
        {
            this->on_function_enter();
            OutputType val = std::move(this->transform_fn_(std::forward<InputTypes>(values)...));
            nextBlock->post(std::move(val));
            this->on_function_done();
        }),
        complete_fn_([this, nextBlock]()
        {
            nextBlock->complete();
            this->signal_completion();
        })
    {}


public:
    bool is_complete() const
    {
        return completion_signaled_ && num_queued_or_running_ == 0;
    }

    bool has_more_data() const
    {
        return num_queued_or_running_ + queue_->size() > 0;
    }

    size_t num_ready_data() const
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);

        return queue_->size();
    }

    void set_max_queued(size_t max_queued)
    {
        max_queued_ = max_queued;
        post_variable_.notify_all();
    }

    static std::shared_ptr<TaskBlock<OutputType, InputTypes...>>
        create(transform_fn_type &&transform_fn, std::shared_ptr<ThreadPool> pool = std::make_shared<ThreadPool>(1))
    {
        return std::shared_ptr<TaskBlock<OutputType, InputTypes...>>
            (new TaskBlock<OutputType, InputTypes...>(std::forward<transform_fn_type>(transform_fn), pool));
    }

    template <class OtherOutputType>
    static std::shared_ptr<TaskBlock<OutputType, InputTypes...>>
        create(std::shared_ptr<TaskBlock<OtherOutputType, OutputType>> nextBlock,
               transform_fn_type &&transform_fn,
               std::shared_ptr<ThreadPool> pool = std::make_shared<ThreadPool>(1))
    {
        return std::shared_ptr<TaskBlock<OutputType, InputTypes...>>
            (new TaskBlock<OutputType, InputTypes...>(nextBlock, std::forward<transform_fn_type>(transform_fn), pool));
    }

	void post(InputTypes&&... data)
	{
        if (completion_signaled_)
            throw std::exception("Cannot post data to completed TaskBlock");

        ++num_queued_or_running_;

        std::unique_lock<std::mutex> lock(post_mutex_);

        post_variable_.wait(lock, [this] {
            size_t max_queued = this->max_queued_;
            return max_queued == 0 || this->num_queued_ < max_queued;
        });

        ++num_queued_;

		pool_->post_no_future(fn_, std::forward<InputTypes>(data)...);
	}

    void post(const InputTypes&... data)
    {
        if (completion_signaled_)
            throw std::exception("Cannot post data to completed TaskBlock");

        ++num_queued_or_running_;

        std::unique_lock<std::mutex> lock(post_mutex_);

        post_variable_.wait(lock, [this]{
            size_t max_queued = this->max_queued_;
            return max_queued == 0 || this->num_queued_ < max_queued;
        });
        
        ++num_queued_;

        pool_->post_no_future(fn_, data...);
    }

    void complete()
    {
        this->completion_signaled_ = true;
        if (is_complete())
            complete_fn_();
    }

    void wait_for_completion()
    {
        if (is_complete())
            return;

        std::unique_lock<std::mutex> lock(completion_mutex_);
        completion_variable_.wait(lock, [this]() {return this->is_complete(); });
    }

	OutputType get()
	{
        if (queue_ == nullptr)
            throw std::exception("TaskBlock is not an output block");

		std::unique_lock<std::mutex> lock(queue_mutex_);
		queue_variable_.wait(lock, [this] { return !this->queue_->empty() || (!this->has_more_data() && this->is_complete()); });

        if (!this->has_more_data() && is_complete())
            throw std::exception("Tried getting data after block was completed");

		OutputType ret = std::move(queue_->front());
		queue_->pop();
		return ret;
	}
};

template <class... InputTypes>
class TaskBlock<void, InputTypes...>
{
    using transform_fn_type = std::function<void(InputTypes...)>;

    std::shared_ptr<ThreadPool> pool_;
    transform_fn_type transform_fn_;
    transform_fn_type fn_;

    std::mutex post_mutex_;
    std::mutex completion_mutex_;

    std::condition_variable post_variable_;
    std::condition_variable completion_variable_;

    std::atomic_size_t max_queued_ = 0;
    std::atomic_size_t num_queued_ = 0;
    std::atomic_size_t num_queued_or_running_ = 0;

    bool completion_signaled_ = false;

    bool decrement_and_check_completion()
    {
        size_t queued_or_running = --num_queued_or_running_;
        bool isComplete = completion_signaled_ && queued_or_running == 0;

        if (isComplete)
            signal_completion();

        return isComplete;
    }

    void on_function_enter()
    {
        --num_queued_;
        post_variable_.notify_one();
    }

    void on_function_done()
    {
        decrement_and_check_completion();
    }

    void signal_completion()
    {
        completion_variable_.notify_all();
    }

    TaskBlock(transform_fn_type &&transform_fn, std::shared_ptr<ThreadPool> pool = std::make_shared<ThreadPool>(1)) :
        pool_(pool),
        transform_fn_(std::forward<transform_fn_type>(transform_fn)),
        fn_([this](InputTypes&&... data)
        {
            this->on_function_enter();
            this->transform_fn_(std::forward<InputTypes>(data)...);
            this->on_function_done();
        })
    {}

public:

    bool is_complete() const
    {
        return completion_signaled_ && num_queued_or_running_ == 0;
    }

    void set_max_queued(size_t max_queued)
    {
        max_queued_ = max_queued;
        post_variable_.notify_all();
    }

    static std::shared_ptr<TaskBlock<void, InputTypes...>>
    create(transform_fn_type &&transform_fn, std::shared_ptr<ThreadPool> pool = std::make_shared<ThreadPool>(1))
    {
        return std::shared_ptr<TaskBlock<void, InputTypes...>>
            (new TaskBlock<void, InputTypes...>(std::forward<transform_fn_type>(transform_fn), pool));
    }

    void post(InputTypes&&... data)
    {
        if (completion_signaled_)
            throw std::exception("Cannot post data to completed TaskBlock");

        ++num_queued_or_running_;

        std::unique_lock<std::mutex> lock(post_mutex_);

        post_variable_.wait(lock, [this] {
            size_t max_queued = this->max_queued_;
            return max_queued == 0 || this->num_queued_ < max_queued;
        });

        ++num_queued_;

        pool_->post_no_future(fn_, std::forward<InputTypes>(data)...);
    }

    void complete()
    {
        this->completion_signaled_ = true;
        if (is_complete())
            signal_completion();
    }

    void wait_for_completion()
    {
        if (is_complete())
            return;

        std::unique_lock<std::mutex> lock(completion_mutex_);
        completion_variable_.wait(lock, [this]() {return this->is_complete(); });
    }
};