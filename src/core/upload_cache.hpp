#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace dx11_enhancement {
// Copies CPU uploads before Unmap. Never calls DirectX while locked. Resource
// destruction callbacks can run under the driver's lock on another thread, so
// they must never acquire the render-state mutex.
class UploadCache {
    struct Entry {
        std::vector<unsigned char> bytes, valid;
        const unsigned char* mapped=nullptr;
        size_t offset=0, length=0;
    };
    std::unordered_map<uint64_t,Entry> entries_;
    mutable std::recursive_mutex mutex_;
public:
    void create(uint64_t id,size_t size,const void* initial=nullptr) {
        std::lock_guard lock(mutex_);
        erase(id);
        if(!id || !size || size>65536 || entries_.size()>=2048)return;
        auto& e=entries_[id];e.bytes.resize(size);e.valid.resize(size);
        if(initial)update(id,0,size,initial);
    }
    void erase(uint64_t id){std::lock_guard lock(mutex_);entries_.erase(id);}
    void invalidate(uint64_t id){std::lock_guard lock(mutex_);auto it=entries_.find(id);if(it!=entries_.end()){
        std::fill(it->second.valid.begin(),it->second.valid.end(),uint8_t{0});it->second.mapped=nullptr;
    }}
    bool contains(uint64_t id)const{std::lock_guard lock(mutex_);return entries_.find(id)!=entries_.end();}
    void update(uint64_t id,uint64_t offset,uint64_t size,const void* data) {
        std::lock_guard lock(mutex_);
        auto it=entries_.find(id);if(it==entries_.end())return;auto& e=it->second;
        if(size==UINT64_MAX && offset<=e.bytes.size())size=e.bytes.size()-offset;
        if(!data || offset>e.bytes.size() || size>e.bytes.size()-offset){invalidate(id);return;}
        std::memcpy(e.bytes.data()+size_t(offset),data,size_t(size));
        std::fill(e.valid.begin()+size_t(offset),e.valid.begin()+size_t(offset+size),uint8_t{1});
    }
    void map(uint64_t id,uint64_t offset,uint64_t size,const void* data,bool discard) {
        std::lock_guard lock(mutex_);
        auto it=entries_.find(id);if(it==entries_.end())return;auto& e=it->second;
        e.mapped=nullptr;if(discard)invalidate(id);
        if(size==UINT64_MAX && offset<=e.bytes.size())size=e.bytes.size()-offset;
        if(!data || offset>e.bytes.size() || size>e.bytes.size()-offset)return;
        e.mapped=static_cast<const unsigned char*>(data);e.offset=size_t(offset);e.length=size_t(size);
    }
    void unmap(uint64_t id){std::lock_guard lock(mutex_);auto it=entries_.find(id);if(it==entries_.end())return;auto& e=it->second;
        if(e.mapped)update(id,e.offset,e.length,e.mapped);e.mapped=nullptr;
    }
    bool read(uint64_t id,uint64_t offset,void* data,size_t size)const {
        std::lock_guard lock(mutex_);
        auto it=entries_.find(id);if(it==entries_.end())return false;const auto& e=it->second;
        if(e.mapped || !data || offset>e.bytes.size() || size>e.bytes.size()-offset)return false;
        const auto first=e.valid.begin()+size_t(offset);
        if(std::find(first,first+size,0)!=first+size)return false;
        std::memcpy(data,e.bytes.data()+size_t(offset),size);return true;
    }
};
}
