#include <afina/concurrency/Executor.h>

namespace Afina {
namespace Concurrency {

void Executor::Stop(bool await) {
    std::unique_lock<std::mutex> lock(mutex);
    if(state==State::kRun){
        state=State::kStopping;
        if(await && threads>0){
            stop_condition.wait(lock,[&](){ return threads==0;});
        }
    }else{
        state=State::kStopped;
    }
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
                    (executor->threads > executor->_low_watermark)) {
                    --executor->_free_threads;
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
        try{
            task();
        }catch(...){
            //std::terminate();
        }
        {
            std::unique_lock<std::mutex> lock(executor->mutex);
            if (executor->state == Executor::State::kStopping && executor->threads==0) {
                    executor->state = Executor::State::kStopped;
                    executor->stop_condition.notify_all();
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
        t.detach();
        ++threads;
    }
}
} // namespace Concurrency
} // namespace Afina
