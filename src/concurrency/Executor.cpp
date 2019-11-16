#include <afina/concurrency/Executor.h>

namespace Afina {
namespace Concurrency {

void Executor::Stop(bool await) {
    std::unique_lock<std::mutex> lock(mutex);
    state = State::kStopping;
    while (tasks.size() > 0)
        empty_condition.notify_one();
    if (await)
        while (state == State::kStopping)
            stop_condition.wait(lock);
}
void perform(Executor *executor) {
    std::function<void()> task;
    while (executor->state == Executor::State::kRun) {
        {
            std::unique_lock<std::mutex> lock(executor->mutex);
            auto timeout = std::chrono::system_clock::now() + executor->_idle_time;
            while ((executor->state == Executor::State::kRun) && executor->tasks.empty()) {
                ++executor->_free_threads;
                if ((executor->empty_condition.wait_until(lock, timeout) == std::cv_status::timeout) &&
                    (executor->threads.size() > executor->_low_watermark)) {
                    auto it =executor->threads.find(std::this_thread::get_id());
                    if (it != executor->threads.end()) {
                        --executor->_free_threads;
                        executor->threads.erase(it);
                    }
                    return;
                } else {
                    executor->empty_condition.wait(lock);
                }
                --executor->_free_threads;
            }
            if (executor->tasks.empty()) {
                continue;
            }
            task = executor->tasks.front();
            executor->tasks.pop_front();
        }
        task();
        {
            std::unique_lock<std::mutex> lock(executor->mutex);
            if (executor->state == Executor::State::kStopping) {
                auto it =executor->threads.find(std::this_thread::get_id());
                if (it != executor->threads.end()) {
                    --executor->_free_threads;
                    it->second.detach();
                    executor->threads.erase(it);
                }
                if (executor->threads.size() == 0) {
                    executor->state = Executor::State::kStopped;
                    executor->stop_condition.notify_all();
                }
            }
        }
    }
}

void Executor::Start()
{
    std::unique_lock<std::mutex> lock(mutex);
    state=State::kRun;
    for(std::size_t i=0;i<_low_watermark;++i){
        std::thread t(&(perform), this);
        threads.insert(std::move(std::make_pair(t.get_id(), std::move(t))));
    }
}
} // namespace Concurrency
} // namespace Afina
