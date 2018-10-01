#pragma once

#include <queue>
#include <memory>
#include <mutex>

#include "ThreadPool.hpp"

#include "blockingconcurrentqueue.h"

template <class Type, class ...Args>
constexpr size_t count_type()
{
    return ((std::is_same_v<Type, Args> ? 1 : 0) + ...);
}

template <class Type, class ...Args>
struct is_unique
{
    static constexpr bool value = count_type<Type, Args...>() == 1;
};

template <class Type, class ...Args>
inline constexpr bool is_unique_v = is_unique<Type, Args...>::value;

template <class ...Types, class = std::enable_if_t<sizeof...(Types) == 0>>
std::tuple<> get_uniqued_queue_tuple()
{
    return std::make_tuple();
}

template <class Type, class ...Args>
constexpr auto get_uniqued_queue_tuple()
{
    if constexpr (sizeof...(Args) == 0)
        return std::tuple<moodycamel::ConcurrentQueue<Type>>{};
    else if constexpr (is_unique_v<Type, Type, Args...>)
        return std::tuple_cat(std::tuple<moodycamel::ConcurrentQueue<Type>>{}, get_uniqued_queue_tuple<Args...>());
    else
        return get_uniqued_queue_tuple<Args...>();
}

template <class CmpTy, class Ty, class ...Args>
constexpr ptrdiff_t get_uniqued_index(ptrdiff_t index = 0)
{
    if constexpr(count_type<CmpTy, Ty, Args...>() == 0)
        return -1;
    else if constexpr (sizeof...(Args) == 0)
        return index;
    else if constexpr(is_unique_v<Ty, Ty, Args...> && std::is_same_v<CmpTy, Ty>)
        return index;
    else if constexpr(is_unique_v<Ty, Ty, Args...>)
        return get_uniqued_index<CmpTy, Args...>(index + 1);
    else
        return get_uniqued_index<CmpTy, Args...>(index);
}

template <class OutputType, class... InputTypes>
class TaskBlock
{
private:
    template <class OtherOutputType, class... OtherInputTypes>
    friend class TaskBlock;

    using transform_fn_type = std::function<OutputType(InputTypes...)>;
    using internal_fn_type = std::function<void(InputTypes...)>;
    using predicate_fn_type = std::function<bool(OutputType&)>;

    using queue_tuple_t = decltype(get_uniqued_queue_tuple<InputTypes...>());

    queue_tuple_t input_queues_ = get_uniqued_queue_tuple<InputTypes...>();

    std::shared_ptr<ThreadPool> pool_;

    transform_fn_type transform_fn_;
    internal_fn_type fn_;

    std::vector<std::tuple<predicate_fn_type, std::function<void(OutputType)>, std::function<void()>>> child_blocks_;

    std::function<void()> complete_fn_;

    std::unique_ptr<moodycamel::BlockingConcurrentQueue<OutputType>> queue_;

    std::mutex completion_mutex_;

    std::condition_variable completion_variable_;

    std::atomic_size_t num_queued_or_running_ = 0;

    std::atomic_size_t required_signal_num_ = 1;
    std::atomic_size_t completion_signaled_num_ = 0;
    std::atomic_size_t num_parents_ = 0;
    std::atomic_size_t num_children_ = 0;

    bool is_exclusive_output_mode_ = true;

    template<typename Type>
    size_t get_queue_size() const
    {
        const auto ind = get_uniqued_index<Type, InputTypes...>();

        static_assert(ind >= 0);

        auto &queue = std::get<ind>(input_queues_);

        return queue.size_approx();
    }

    size_t num_postable() const
    {
        if constexpr (sizeof...(InputTypes) > 0)
            return std::min({ (get_queue_size<InputTypes>() / count_type<InputTypes, InputTypes...>())... });
        else
            return std::numeric_limits<size_t>::max();
    }

    template<typename Type>
    Type get_from_queue()
    {
        const auto ind = get_uniqued_index<Type, InputTypes...>();

        static_assert(ind >= 0);

        auto &queue = std::get<ind>(input_queues_);

        Type val;

        while (!queue.try_dequeue(val));

        return val;
    }

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

            this->queue_->enqueue(std::move(val));

            this->on_function_done();
        }),
        complete_fn_([this] { this->signal_completion(); })
    {
    }

public:
    bool is_complete() const
    {
        bool postable_check = true;

        if constexpr(sizeof...(InputTypes) > 0)
            postable_check = num_postable() == 0;

        return completion_signaled_num_ == required_signal_num_ && num_queued_or_running_ == 0 && postable_check;
    }

    bool has_more_data() const
    {
        return num_queued_or_running_ + queue_->size_approx() > 0;
    }

    size_t num_ready_data() const
    {
        return queue_->size_approx();
    }

    void set_exclusive_output_mode(bool newMode)
    {
        is_exclusive_output_mode_ = newMode;
    }

    template <class OtherOutputType, class ...OtherInputTypes>
    void add_output_block(std::shared_ptr<TaskBlock<OtherOutputType, OtherInputTypes...>> nextBlock,
                        predicate_fn_type &&predicate = [](const OutputType& val) { return true; })
    {
        ++nextBlock->required_signal_num_;
        size_t numParents = nextBlock->num_parents_++;
        if (numParents == 0)
            --nextBlock->required_signal_num_;

        child_blocks_.emplace_back(std::forward<predicate_fn_type>(predicate),
                                  [nextBlock] (OutputType &&value) {
            nextBlock->post(std::forward<OutputType>(value)); 
        },[nextBlock] { nextBlock->complete(); });

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

    static std::shared_ptr<TaskBlock<OutputType, InputTypes...>>
        create(transform_fn_type &&transform_fn, std::shared_ptr<ThreadPool> pool = std::make_shared<ThreadPool>(1))
    {
        return std::shared_ptr<TaskBlock<OutputType, InputTypes...>>
            (new TaskBlock<OutputType, InputTypes...>(std::forward<transform_fn_type>(transform_fn), pool));
    }

    template<typename Type, class = std::enable_if_t<!(sizeof...(InputTypes) <= 1 && count_type<Type, InputTypes...>() >= 1), void>>
    void post(Type&& value)
    {
        if (completion_signaled_num_ == required_signal_num_)
            throw std::exception();

        const auto ind = get_uniqued_index<Type, InputTypes...>();

        static_assert(ind >= 0);

        auto &queue = std::get<ind>(input_queues_);

        queue.enqueue(std::forward<Type>(value));

        if (num_postable() > 0)
            post(std::move(get_from_queue<InputTypes>())...);
    }

    void post(InputTypes&&... data)
    {
        if (completion_signaled_num_ == required_signal_num_)
            throw std::exception();

        if(num_children_ == 0 && !queue_)
            queue_ = std::make_unique<moodycamel::BlockingConcurrentQueue<OutputType>>();

        ++num_queued_or_running_;

        pool_->post_no_future(fn_, std::forward<InputTypes>(data)...);
    }

    std::enable_if_t<(sizeof...(InputTypes) != 0), void>
    post(const InputTypes&... data)
    {
        if (completion_signaled_num_ == required_signal_num_)
            throw std::exception();

        if(num_children_ == 0 && !queue_)
            queue_ = std::make_unique<moodycamel::BlockingConcurrentQueue<OutputType>>();

        ++num_queued_or_running_;

        pool_->post_no_future(fn_, data...);
    }

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
        if (queue_ == nullptr && num_children_ != 0)
            throw std::exception();
        else
            queue_ = std::make_unique<moodycamel::BlockingConcurrentQueue<OutputType>>();

        if (!this->has_more_data() && is_complete())
            throw std::exception();

        OutputType ret;

        bool got_value = false;

        if (!queue_->try_dequeue(ret))
            while (!is_complete() && !got_value)
                got_value = queue_->wait_dequeue_timed(ret, std::chrono::milliseconds(10));

        if (!got_value)
            throw std::exception();

        return ret;
    }

    bool try_get(OutputType &val)
    {
        if (queue_ == nullptr)
            throw std::exception();

        if (!this->has_more_data() && is_complete())
            throw std::exception();

        return queue_->try_dequeue(val);
    }

    ~TaskBlock()
    {
        if (num_parents_ == 0 && completion_signaled_num_ != 1)
            complete();

        pool_ = nullptr;

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

    using queue_tuple_t = decltype(get_uniqued_queue_tuple<InputTypes...>());

    queue_tuple_t input_queues_ = get_uniqued_queue_tuple<InputTypes...>();

    template<typename Type>
    size_t get_queue_size() const
    {
        const auto ind = get_uniqued_index<Type, InputTypes...>();

        static_assert(ind >= 0);

        auto &queue = std::get<ind>(input_queues_);

        return queue.size_approx();
    }

    size_t num_postable() const
    {
        if constexpr (sizeof...(InputTypes) > 0)
            return std::min({ (get_queue_size<InputTypes>() / count_type<InputTypes, InputTypes...>())... });
        else
            return std::numeric_limits<size_t>::max();
    }

    template<typename Type>
    Type get_from_queue()
    {
        const auto ind = get_uniqued_index<Type, InputTypes...>();

        static_assert(ind >= 0);

        auto &queue = std::get<ind>(input_queues_);

        Type val;

        while (!queue.try_dequeue(val));

        return val;
    }

    std::shared_ptr<ThreadPool> pool_;
    transform_fn_type transform_fn_;
    transform_fn_type fn_;

    std::mutex completion_mutex_;

    std::condition_variable completion_variable_;

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
        bool postable_check = true;

        if constexpr(sizeof...(InputTypes) > 0)
            postable_check = num_postable() == 0;

        return completion_signaled_num_ == required_signal_num_ && num_queued_or_running_ == 0 && postable_check;
    }

    static std::shared_ptr<TaskBlock<void, InputTypes...>>
    create(transform_fn_type &&transform_fn, std::shared_ptr<ThreadPool> pool = std::make_shared<ThreadPool>(1))
    {
        return std::shared_ptr<TaskBlock<void, InputTypes...>>
            (new TaskBlock<void, InputTypes...>(std::forward<transform_fn_type>(transform_fn), pool));
    }

    template<typename Type, class = std::enable_if_t<!(sizeof...(InputTypes) <= 1 && count_type<Type, InputTypes...>() >= 1), void>>
    void post(Type&& value)
    {
        if (completion_signaled_num_ == required_signal_num_)
            throw std::exception();

        const auto ind = get_uniqued_index<Type, InputTypes...>();

        static_assert(ind >= 0);

        auto &queue = std::get<ind>(input_queues_);

        queue.enqueue(std::forward<Type>(value));

        if (num_postable() > 0)
            post(std::move(get_from_queue<InputTypes>())...);
    }

    void post(InputTypes&&... data)
    {
        if (completion_signaled_num_ == required_signal_num_)
            throw std::exception();

        ++num_queued_or_running_;

        pool_->post_no_future(fn_, std::move(data)...);
    }

    template<class = std::enable_if_t<!(sizeof...(InputTypes) == 0), void>>
    void post(const InputTypes&... data)
    {
        if (completion_signaled_num_ == required_signal_num_)
            throw std::exception();

        ++num_queued_or_running_;

        pool_->post_no_future(fn_, data...);
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

        pool_ = nullptr;

        wait_for_completion();
    }
};
