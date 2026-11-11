// Independent exact subset-DP oracle for the resource-window lower bound.
// Run from the repository root (no third-party dependencies):
//   c++ -std=c++17 -O2 -Isrc notes/check_window_bound.cpp src/schedule.cpp -o build/check_window_bound
//   taskset -c 4 build/check_window_bound
// Covers all ordered DAGs through four operations, every 3-kind assignment,
// three capacity vectors, 3,000 deterministic random larger DAGs, and no-op input.
// The DP enumerates all capacity-feasible subsets of currently ready operations;
// it does not use the EDF algorithm or its time-window relaxation.

#include "schedule.hpp"
#include "heuristic.hpp"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <random>
#include <vector>

Netlist make(int m, std::vector<unsigned> const& pred, std::vector<int> const& kinds) {
    Netlist n;
    n.name.resize(m + 1);
    n.kind.push_back(Kind::Pi);
    n.inputs.push_back(0);
    n.fanin.offset.push_back(0);
    n.fanin.offset.push_back(0);
    std::vector<std::vector<int>> out(m + 1);
    for (int i = 0; i < m; ++i) {
        n.kind.push_back(OP_KIND_LIST[kinds[i]]);
        ++n.op_count[kinds[i]];
        n.topo.push_back(i + 1);
        if (!pred[i]) { n.fanin.item.push_back(0); out[0].push_back(i+1); }
        for (int j=0;j<i;++j) if (pred[i] & (1U<<j)) {
            n.fanin.item.push_back(j+1); out[j+1].push_back(i+1);
        }
        n.fanin.offset.push_back(static_cast<int>(n.fanin.item.size()));
    }
    n.fanout.offset.push_back(0);
    for (auto const& row: out) {
        n.fanout.item.insert(n.fanout.item.end(), row.begin(), row.end());
        n.fanout.offset.push_back(static_cast<int>(n.fanout.item.size()));
    }
    return n;
}
int optimum(std::vector<unsigned> const& pred, std::vector<int> const& kinds, Resources const& r) {
    unsigned const end = 1U << pred.size();
    std::vector<int> dp(end, 1000); dp[0]=0;
    for (unsigned mask=0;mask<end;++mask) {
        unsigned ready=0;
        for(unsigned i=0;i<pred.size();++i) if (!(mask&(1U<<i)) && (pred[i]&mask)==pred[i]) ready|=1U<<i;
        for(unsigned sub=ready;sub;sub=(sub-1)&ready) {
            std::array<int,3> used{};
            for(unsigned i=0;i<pred.size();++i) if(sub&(1U<<i)) ++used[kinds[i]];
            if(used[0]<=r.limit[0]&&used[1]<=r.limit[1]&&used[2]<=r.limit[2]) dp[mask|sub]=std::min(dp[mask|sub],dp[mask]+1);
        }
    }
    return dp.back();
}
int main() {
    std::size_t checked=0, improved=0, tight=0;
    auto test=[&](auto const& pred, auto const& kinds, Resources const& r) {
        auto const n=make(static_cast<int>(pred.size()),pred,kinds);
        auto const bound=lower_bound(n,r);
        auto const opt=optimum(pred,kinds,r);
        auto const early=asap(n); int old=0;
        for(auto i:n.topo)old=std::max(old,early[i]);
        for(int k=0;k<3;++k) if(r.limit[k]) old=std::max(old,n.op_count[k]/r.limit[k]+(n.op_count[k]%r.limit[k]!=0));
        if(bound>opt||bound<old) { std::cerr<<"bad "<<bound<<" "<<opt<<" "<<old<<"\n"; std::abort(); }
        ++checked; improved+=bound>old; tight+=bound==opt;
    };
    for(int m=0;m<=4;++m) {
        int const edges=m*(m-1)/2;
        int patterns=1;for(int i=0;i<m;++i)patterns*=3;
        for(unsigned graph=0;graph<(1U<<edges);++graph) {
            std::vector<unsigned> pred(m);int edge=0;
            for(int i=0;i<m;++i)for(int j=0;j<i;++j,++edge) if(graph&(1U<<edge))pred[i]|=1U<<j;
            for(int pattern=0;pattern<patterns;++pattern) {
                std::vector<int> kinds(m);int p=pattern;for(auto &k:kinds){k=p%3;p/=3;}
                test(pred,kinds,Resources{{1,1,1}});
                test(pred,kinds,Resources{{2,1,2}});
                test(pred,kinds,Resources{{2147483647,2147483647,2147483647}});
            }
        }
    }
    std::mt19937 rng(20261111);
    for(int rep=0;rep<3000;++rep) {
        int const m=5+rng()%5;
        std::vector<unsigned> pred(m);std::vector<int> kinds(m);
        for(int i=0;i<m;++i){kinds[i]=rng()%3;for(int j=0;j<i;++j)if(rng()%4==0)pred[i]|=1U<<j;}
        test(pred,kinds,Resources{{1+static_cast<int>(rng()%3),1+static_cast<int>(rng()%3),1+static_cast<int>(rng()%3)}});
    }
    test(std::vector<unsigned>{}, std::vector<int>{}, Resources{{0,0,0}});
    std::cout<<"checked="<<checked<<" improved="<<improved<<" tight="<<tight<<"\n";
}
