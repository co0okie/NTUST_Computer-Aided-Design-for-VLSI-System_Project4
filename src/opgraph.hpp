#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "netlist.hpp"
#include "schedule.hpp"

// The operations only (no primary inputs), renumbered 0..n-1 in topological order. All the
// scheduling algorithms work on this compact form; `reversed` gives the mirror problem (every
// edge flipped), in which a tail of the original is a head and vice versa.
struct OpGraph {
    std::int32_t n = 0;
    std::vector<std::uint8_t> kind; // 0 = AND, 1 = OR, 2 = NOT
    Csr succ;                       // op -> ops it drives (larger indices)
    Csr pred;                       // op -> ops driving it (smaller indices)
    std::vector<NodeId> node;       // op index -> netlist node id
    std::array<std::int32_t, OP_KINDS> limit{};
    std::array<std::int32_t, OP_KINDS> count{};
};

OpGraph make_op_graph(Netlist const &n, Resources const &r);

// Mirror image: op i becomes n-1-i, successors and predecessors swap.
OpGraph reversed(OpGraph const &g);

// Schedule (1-based step per op index) of the mirror problem -> schedule of the original.
std::vector<std::int32_t> mirror_steps(std::vector<std::int32_t> const &step);

Schedule to_schedule(Netlist const &n, OpGraph const &g, std::vector<std::int32_t> const &step);
