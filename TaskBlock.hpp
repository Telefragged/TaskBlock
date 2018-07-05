#pragma once

#include <queue>
#include <memory>
#include <mutex>

#include "ThreadPool.hpp"

template <class OutputType, class... InputTypes>
class TaskBlock
{
private:
    template <class OtherOutputType, class... OtherInputTypes>
    friend class TaskBlock;

	using transform_fn_type = std::function<OutputType(InputTypes...)>;
	using internal_fn_type = std::function<void(InputTypes...)>;
    using predicate_fn_type = std::function<bool(OutputType&)>;

	std::shared_ptr<ThreadPool> pool_;

	transform_fn_type transform_fn_;
	internal_fn_type fn_;

    std::vector<std::tuple<predicate_fn_type, std::function<void(OutputType)>, std::function<void()>>> child_blocks_;

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

    std::atomic_size_t required_signal_num_ = 1;
    std::atomic_size_t completion_signaled_num_ = 0;
    std::atomic_size_t num_parents_ = 0;
    std::atomic_size_t num_children_ = 0;

    bool is_exclusive_output_mode_ = true;

    bool decrement_and_check_completion()
    {
        --num_queued_or_running_;
        bool isComplete = is_complete();

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
    }


public:
    bool is_complete() const
    {
        return completion_signaled_num_ == required_signal_num_ && num_queued_or_running_ == 0;
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

    void set_exclusive_output_mode(bool newMode)
    {
        is_exclusive_output_mode_ = newMode;
    }

    template <class OtherOutputType>
    void add_output_block(std::shared_ptr<TaskBlock<OtherOutputType, OutputType>> nextBlock,
                        predicate_fn_type &&predicate = [](const OutputType& val) { return true; })
    {
        ++nextBlock->required_signal_num_;
        size_t numParents = nextBlock->num_parents_++;
        if (numParents == 0)
            --nextBlock->required_signal_num_;

        child_blocks_.emplace_back(std::forward<predicate_fn_type>(predicate),
                                  [nextBlock] (OutputType &&value) {
            nextBlock->post(std::forward<OutputType>(value)); 
        },
                                  [nextBlock] { nextBlock->complete(); });

        size_t numChildren = num_children_++;

        if (numChildren == 0)
        {
            fn_ = [this, nextBlock](InputTypes&&... values)
            {
                this->on_function_enter();
                OutputType val = this->transform_fn_(std::forward<InputTypes>(values)...);
                for (auto &block : this->child_blocks_)
                {
                    auto &[pred, post, complete] = block;
                    if (pred(val))
                    {
                        if (this->is_exclusive_output_mode_ || this->child_blocks_.size() == 1)
                        {
                            post(std::move(val));
                            break;
                        }
                        else
                        {
                            post(val);
                        }
                    }
                }
                this->on_function_done();
            };

            complete_fn_ = [this]
            {
                for (auto &block : this->child_blocks_)
                {
                    auto &[pred, post, complete] = block;
                    complete();
                }
                this->signal_completion();
            };
        }
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

	void post(InputTypes&&... data)
	{
        if (completion_signaled_num_ == required_signal_num_)
            throw std::exception();

        if(num_children_ == 0 && !queue_)
            queue_ = std::make_unique<std::queue<OutputType>>();

        ++num_queued_or_running_;

        std::unique_lock<std::mutex> lock(post_mutex_);

        post_variable_.wait(lock, [this] {
            size_t max_queued = this->max_queued_;
            return max_queued == 0 || this->num_queued_ < max_queued;
        });

        ++num_queued_;

		pool_->post_no_future(fn_, std::forward<InputTypes>(data)...);
	}

    // void post(const InputTypes&... data)
    // {
    //     if (completion_signaled_num_ == required_signal_num_)
    //         throw std::exception();

    //     if(num_children_ == 0 && !queue_)
    //         queue_ = std::make_unique<std::queue<OutputType>>();

    //     ++num_queued_or_running_;

    //     std::unique_lock<std::mutex> lock(post_mutex_);

    //     post_variable_.wait(lock, [this]{
    //         size_t max_queued = this->max_queued_;
    //         return max_queued == 0 || this->num_queued_ < max_queued;
    //     });
        
    //     ++num_queued_;

    //     pool_->post_no_future(fn_, data...);
    // }

    void complete()
    {
        ++completion_signaled_num_;
        if(is_complete())
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
            throw std::exception();

		std::unique_lock<std::mutex> lock(queue_mutex_);
		queue_variable_.wait(lock, [this] { return !this->queue_->empty() || (!this->has_more_data() && this->is_complete()); });

        if (!this->has_more_data() && is_complete())
            throw std::exception();

		OutputType ret = std::move(queue_->front());
		queue_->pop();
		return ret;
	}

    ~TaskBlock()
    {
        if (num_parents_ == 0 && completion_signaled_num_ != 1)
            complete();

        wait_for_completion();
    }
};

template <class... InputTypes>
class TaskBlock<void, InputTypes...>
{
private:
    template <class OtherOutputType, class... OtherInputTypes>
    friend class TaskBlock;

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

    std::atomic_size_t required_signal_num_ = 1;
    std::atomic_size_t completion_signaled_num_ = 0;
    std::atomic_size_t num_parents_ = 0;

    bool decrement_and_check_completion()
    {
        --num_queued_or_running_;
        bool isComplete = is_complete();

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
        return completion_signaled_num_ == required_signal_num_ && num_queued_or_running_ == 0;
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
        if (completion_signaled_num_ == required_signal_num_)
            throw std::exception();

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
        ++completion_signaled_num_;
        if(is_complete())
            signal_completion();
    }

    void wait_for_completion()
    {
        if (is_complete())
            return;

        std::unique_lock<std::mutex> lock(completion_mutex_);
        completion_variable_.wait(lock, [this]() {return this->is_complete(); });
    }

    ~TaskBlock()
    {
        if (num_parents_ == 0 && completion_signaled_num_ != 1)
            complete();

        wait_for_completion();
    }
};
