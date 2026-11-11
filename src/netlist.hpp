#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using NodeId = std::int32_t;

// Node kinds. A primary input is a node but not an operation.
enum class Kind : std::uint8_t { Pi, And, Or, Not };

// The three operation kinds, in the order the CLI and the output use (AND, OR, NOT).
constexpr int OP_KINDS = 3;
constexpr std::array<Kind, OP_KINDS> OP_KIND_LIST = {Kind::And, Kind::Or, Kind::Not};

// Index into per-kind arrays: And = 0, Or = 1, Not = 2. Only valid for operations.
constexpr int op_index(Kind k) { return static_cast<int>(k) - 1; }

constexpr char const *kind_name(Kind k) {
    switch (k) {
    case Kind::Pi: return "PI";
    case Kind::And: return "AND";
    case Kind::Or: return "OR";
    case Kind::Not: return "NOT";
    }
    return "?";
}

// Read-only view of a contiguous range (std::span is C++20).
template <class T>
struct Span {
    T const *first = nullptr;
    T const *last = nullptr;

    T const *begin() const { return first; }
    T const *end() const { return last; }
    std::size_t size() const { return static_cast<std::size_t>(last - first); }
    bool empty() const { return first == last; }
    T const &operator[](std::size_t i) const { return first[i]; }
};

// Compressed sparse rows: row i is item[offset[i] .. offset[i + 1]).
struct Csr {
    std::vector<std::int32_t> offset; // rows + 1 entries
    std::vector<NodeId> item;

    Span<NodeId> row(NodeId i) const {
        auto const *base = item.data();
        return {base + offset[i], base + offset[i + 1]};
    }
};

// The data-flow graph, stored as structure of arrays indexed by NodeId.
//
// Node ids: primary inputs first (in .inputs order), then operations in the order their .names
// blocks appear in the BLIF file. Printing a step's operations in id order therefore follows the
// file order, as in the assignment's example.
struct Netlist {
    std::string model;

    // Per node
    std::vector<std::string> name;
    std::vector<Kind> kind;
    Csr fanin;  // drivers of each node (empty for primary inputs)
    Csr fanout; // nodes each node drives

    std::vector<NodeId> inputs;  // primary inputs, .inputs order
    std::vector<NodeId> outputs; // primary outputs, .outputs order (may repeat a node)
    std::vector<NodeId> topo;    // all operations in topological order (fanins first)
    std::array<std::int32_t, OP_KINDS> op_count{}; // number of operations of each kind

    NodeId size() const { return static_cast<NodeId>(name.size()); }
    NodeId num_ops() const { return static_cast<NodeId>(topo.size()); }
    bool is_op(NodeId i) const { return kind[i] != Kind::Pi; }
};
