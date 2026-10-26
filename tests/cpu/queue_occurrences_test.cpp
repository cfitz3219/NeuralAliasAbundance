#include "test_support.hpp"
#include "host_dx12/observe/queue_occurrences.hpp"
#include <atomic>
#include <thread>
#include <vector>
using na::observe::QueueOccurrences;
int main(){return test::run({
    {"isolated calls and queue-local tickets",[]{QueueOccurrences q,other;auto a=q.Begin();CHECK(!q.End(a));auto b=q.Begin();CHECK(!q.End(b));CHECK(a.ticket==1&&b.ticket==2);auto c=other.Begin();CHECK(c.ticket==1&&!other.End(c));}},
    {"both nesting directions mark both occurrences",[]{{QueueOccurrences q;auto outer=q.Begin();auto inner=q.Begin();CHECK(q.End(inner));CHECK(q.End(outer));auto after=q.Begin();CHECK(!q.End(after));}}},
    {"crossing windows remain ambiguous after either finishes",[]{QueueOccurrences q;auto a=q.Begin();auto b=q.Begin();CHECK(q.End(a));auto c=q.Begin();CHECK(q.End(b));CHECK(q.End(c));}},
    {"delivery order does not change finalized flags",[]{QueueOccurrences q;auto a=q.Begin();auto b=q.Begin();const bool inner=q.End(b);auto c=q.Begin();const bool third=q.End(c);const bool outer=q.End(a);CHECK(inner&&third&&outer);auto d=q.Begin();CHECK(!q.End(d));}},
    {"an overlap ended before a later entry does not contaminate it",[]{QueueOccurrences q;auto a=q.Begin();auto b=q.Begin();CHECK(q.End(a));CHECK(q.End(b));auto c=q.Begin();CHECK(!q.End(c));}},
    {"concurrent boundaries serialize without holding the original call",[]{
        QueueOccurrences q; constexpr unsigned n=8;std::atomic<unsigned> ready{0},bad{0};
        std::vector<std::thread> threads;std::vector<uint64_t> tickets(n);
        for(unsigned i=0;i<n;++i)threads.emplace_back([&,i]{auto t=q.Begin();tickets[i]=t.ticket;++ready;while(ready.load()!=n)std::this_thread::yield();if(!q.End(t))++bad;});
        for (auto& t : threads) { t.join(); }
        CHECK(bad.load()==0);
        for(unsigned i=0;i<n;++i)for(unsigned j=i+1;j<n;++j)CHECK(tickets[i]!=tickets[j]);
        auto after=q.Begin();CHECK(!q.End(after));
    }},
});}
