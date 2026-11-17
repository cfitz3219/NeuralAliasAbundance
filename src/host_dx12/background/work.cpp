#include "work.hpp"
#include "host_dx12/observe/producers.hpp"
#include <windows.h>
#include <taskflow/taskflow.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <memory>
namespace na::background {
namespace {
constexpr unsigned kWorkers=2,kPendingLimit=32;
std::atomic<bool> exit_requested{false};
thread_local bool on_worker=false;
struct Counters {std::atomic<uint64_t> accepted{0},completed{0},failed{0},rejected{0};std::atomic<uint32_t> active{0},workers{0},peak{0};};
struct Workers final:tf::WorkerInterface {
    Counters& counters;explicit Workers(Counters& c):counters(c){}
    void scheduler_prologue(tf::Worker&)override{SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);on_worker=true;++counters.workers;}
    void scheduler_epilogue(tf::Worker&,std::exception_ptr)override{--counters.workers;on_worker=false;}
};
struct State {
    std::mutex mutex,stop_mutex;std::condition_variable cv;Counters counters;
    std::unique_ptr<tf::Executor> executor;uint32_t pending=0;bool stopping=false;
};
// The host explicitly drains/joins via Stop. Process detach only sets exit_requested;
// never invoke Taskflow's joining destructor during static teardown / loader lock.
State& state(){static auto* s=new State;return *s;}
bool start_locked(State& s){
    if(exit_requested.load(std::memory_order_acquire)||s.stopping)return false;
    if(s.executor)return true;
    try{s.executor=std::make_unique<tf::Executor>(kWorkers,std::make_shared<Workers>(s.counters));return true;}
    catch(...){++s.counters.failed;return false;}
}
}
bool Start(){auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);return start_locked(s);}
bool Submit(std::function<void()> job){
    if(!job)return false;auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);
    if(!start_locked(s)||s.pending>=kPendingLimit){++s.counters.rejected;return false;}
    ++s.pending;++s.counters.accepted;
    try{s.executor->silent_async([&s,job=std::move(job)]()mutable{
        na::observe::HostWorkScope own;
        const auto active=++s.counters.active;auto peak=s.counters.peak.load();
        while(active>peak&&!s.counters.peak.compare_exchange_weak(peak,active)){}
        try{job();}catch(...){++s.counters.failed;}
        job={}; // release captured GPU/resource owners while HostWorkScope is still active
        --s.counters.active;++s.counters.completed;
        {std::lock_guard<std::mutex> done(s.mutex);--s.pending;}s.cv.notify_all();
    });return true;}
    catch(...){--s.pending;--s.counters.accepted;++s.counters.failed;s.cv.notify_all();return false;}
}
bool WaitIdle(uint32_t ms){
    if(on_worker)return false;auto& s=state();std::unique_lock<std::mutex> lock(s.mutex);
    return s.cv.wait_for(lock,std::chrono::milliseconds(ms),[&]{return s.pending==0;});
}
bool Stop(uint32_t ms){
    if(on_worker)return false;auto& s=state();std::lock_guard<std::mutex> stop(s.stop_mutex);std::unique_ptr<tf::Executor> retired;
    {std::unique_lock<std::mutex> lock(s.mutex);s.stopping=true;
        if(!s.cv.wait_for(lock,std::chrono::milliseconds(ms),[&]{return s.pending==0;}))return false;
        retired=std::move(s.executor);}
    // All accepted jobs returned before the executor joins its scheduling threads.
    retired.reset();
    {std::lock_guard<std::mutex> lock(s.mutex);s.stopping=false;}return true;
}
void RequestStop(){exit_requested.store(true,std::memory_order_release);}
Stats GetStats(){auto& s=state();Stats out;auto& c=s.counters;
    out.accepted=c.accepted;out.completed=c.completed;out.failed=c.failed;out.rejected=c.rejected;
    out.active=c.active;out.workers=c.workers;out.peak_active=c.peak;
    {std::lock_guard<std::mutex> lock(s.mutex);out.pending=s.pending;out.stopping=s.stopping||exit_requested.load();}return out;
}
}
