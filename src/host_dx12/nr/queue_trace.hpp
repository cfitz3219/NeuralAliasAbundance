#pragma once
// Bounded freeze evidence. Never wait, retain game objects, or write files in
// a game callback. Pointer values are identities only; Snapshot never derefs.
#include <windows.h>
#include <atomic>
#include <array>
#include <mutex>
#include <cstdio>
#include "host_dx12/settings.hpp"
namespace na::nr::trace {
struct Event { uint64_t sequence=0,tick=0,queue=0,object=0,value=0,completed=0;DWORD thread=0;const char* operation=nullptr;HRESULT result=0;uintptr_t callers[4]{}; };
inline constexpr size_t Capacity=4096;
inline std::array<Event,Capacity> events;
inline std::mutex mutex;
inline uint64_t sequence=0;
inline std::atomic<uint64_t> dropped{0};
inline std::atomic<uint32_t> sync_hooks{0};
inline void Record(const char* operation,const void* queue,const void* object=nullptr,uint64_t value=0,uint64_t completed=0,HRESULT result=0,void* const* callers=nullptr) {
    if(!settings::Get().nr_enabled.load(std::memory_order_relaxed))return;
    std::unique_lock<std::mutex> lock(mutex,std::try_to_lock);
    if(!lock.owns_lock()){dropped.fetch_add(1,std::memory_order_relaxed);return;}
    const auto id=++sequence;
    events[(id-1)%Capacity]={id,GetTickCount64(),reinterpret_cast<uintptr_t>(queue),reinterpret_cast<uintptr_t>(object),value,completed,GetCurrentThreadId(),operation,result};
    if(callers)for(unsigned i=0;i<4;++i)events[(id-1)%Capacity].callers[i]=reinterpret_cast<uintptr_t>(callers[i]);
}
inline void Dump(HANDLE file) {
    // Called only by the existing background freeze logger.
    std::array<Event,Capacity> snapshot;uint64_t end;
    {std::lock_guard<std::mutex> lock(mutex);snapshot=events;end=sequence;}
    char line[768];DWORD written=0;
    auto n=snprintf(line,sizeof(line),"nr_queue_trace events=%llu dropped=%llu capacity=%zu sync_hooks=%u (bootstrap queue implementation only)\r\n",static_cast<unsigned long long>(end),static_cast<unsigned long long>(dropped.load()),Capacity,sync_hooks.load());
    WriteFile(file,line,static_cast<DWORD>(n),&written,nullptr);
    const auto start=end>Capacity?end-Capacity+1:1;
    for(auto i=start;i<=end;++i){const auto& e=snapshot[(i-1)%Capacity];
        n=snprintf(line,sizeof(line),"nr_queue_event seq=%llu tick=%llu tid=%lu op=%s queue=0x%llx object=0x%llx value=%llu completed=%llu hr=0x%08lx callers=%llx,%llx,%llx,%llx\r\n",
            static_cast<unsigned long long>(e.sequence),static_cast<unsigned long long>(e.tick),e.thread,e.operation,
            static_cast<unsigned long long>(e.queue),static_cast<unsigned long long>(e.object),static_cast<unsigned long long>(e.value),static_cast<unsigned long long>(e.completed),static_cast<unsigned long>(e.result),
            static_cast<unsigned long long>(e.callers[0]),static_cast<unsigned long long>(e.callers[1]),static_cast<unsigned long long>(e.callers[2]),static_cast<unsigned long long>(e.callers[3]));
        WriteFile(file,line,static_cast<DWORD>(n),&written,nullptr);
    }
    // Resolve module ownership only on this background logger, never on a
    // queue callback. The dump also retains the complete loaded-module table.
    std::array<uintptr_t,256> seen{};size_t used=0;
    for(auto i=start;i<=end;++i)for(auto caller:snapshot[(i-1)%Capacity].callers){
        if(!caller)continue;bool known=false;for(size_t j=0;j<used;++j)known|=seen[j]==caller;
        if(known || used==seen.size())continue;seen[used++]=caller;
        HMODULE module=nullptr;char path[MAX_PATH]{};
        if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCSTR>(caller),&module))GetModuleFileNameA(module,path,MAX_PATH);
        n=snprintf(line,sizeof(line),"nr_queue_caller address=0x%llx base=0x%llx offset=0x%llx module=%s\r\n",static_cast<unsigned long long>(caller),static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(module)),static_cast<unsigned long long>(caller-reinterpret_cast<uintptr_t>(module)),path);
        WriteFile(file,line,static_cast<DWORD>(n),&written,nullptr);
    }
}
}
