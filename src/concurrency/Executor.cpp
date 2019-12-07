#include <afina/concurrency/Executor.h>

namespace Afina {
namespace Concurrency {

void Executor::Stop(bool await) {
    std::unique_lock<std::mutex> lock(mutex);
    if(state==State::kRun){
        state=State::kStopping;
        if(await){
            stop_condition.wait(lock,[&](){ return _active_threads+_free_threads==0;});
        }
    }else{
        state=State::kStopped;
    }
}
void perform(Executor *executor) {
    std::function<void()> task;
    while (executor->state == Executor::State::kRun && !executor->tasks.empty()) {
        {
            std::unique_lock<std::mutex> lock(executor->mutex);
            auto timeout = std::chrono::system_clock::now() + executor->_idle_time;
            while ((executor->state == Executor::State::kRun) && executor->tasks.empty()) {
                if ((executor->empty_condition.wait_until(lock, timeout) == std::cv_status::timeout) &&
                    (executor->_active_threads+executor->_free_threads > executor->_low_watermark)) {
                    break;
                } else {
                    executor->empty_condition.wait(lock);
                }
            }
            if (executor->tasks.empty()) {
                continue;
            }
            --executor->_free_threads;
            ++executor->_active_threads;
            task = executor->tasks.front();
            executor->tasks.pop_front();
        }
        try{
            task();
        }catch(...){
            std::terminate();
        }
        {
             std::unique_lock<std::mutex> lock(executor->mutex);
            ++executor->_free_threads;
            --executor->_active_threads;
        }
    }
    {
        std::unique_lock<std::mutex> lock(executor->mutex);
        --executor->_free_threads;
        if (executor->state == Executor::State::kStopping && executor->tasks.size()==0) {
            executor->state = Executor::State::kStopped;
            executor->stop_condition.notify_all();
        }
    }
}

void Executor::Start()
{
    std::unique_lock<std::mutex> lock(mutex);
    state=State::kRun;
    for(std::size_t i=0;i<_low_watermark;++i){
        std::thread t(&(perform), this);
        t.detach();
        ++_free_threads;
    }
}
} // namespace Concurrency
} // namespace Afina
