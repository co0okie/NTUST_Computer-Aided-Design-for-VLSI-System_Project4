#include "blif.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace {

struct Cube {
    std::string pattern; // one of '0', '1', '-' per input
    char output = '1';
};

// One .names block as written in the file.
struct NamesBlock {
    std::vector<std::string> inputs;
    std::string output;
    std::vector<Cube> cubes;
    int line = 0;
};

struct RawBlif {
    std::string model;
    std::vector<std::string> inputs, outputs;
    std::vector<NamesBlock> blocks;
};

std::string at(std::string const &path, int line) { return path + ":" + std::to_string(line); }

std::vector<std::string> split(std::string const &s) {
    std::vector<std::string> tokens;
    std::istringstream in(s);
    for (std::string t; in >> t;) tokens.push_back(std::move(t));
    return tokens;
}

bool directive_is_unsupported(std::string const &d) {
    return d == ".latch" || d == ".mlatch" || d == ".subckt" || d == ".gate" || d == ".search" ||
           d == ".start_kiss" || d == ".exdc";
}

// Pass 1: tokens to raw records. Handles '#' comments and '\' line continuation.
Result<RawBlif> parse(std::string const &path) {
    std::ifstream file(path);
    if (!file) return err("cannot open " + path);

    RawBlif raw;
    bool in_names = false;
    bool ended = false;
    std::string logical;
    int logical_line = 0;
    int line_no = 0;
    for (std::string physical; !ended && std::getline(file, physical);) {
        ++line_no;
        if (auto const hash = physical.find('#'); hash != std::string::npos) physical.resize(hash);
        while (!physical.empty() && (physical.back() == '\r' || physical.back() == ' ' ||
                                     physical.back() == '\t'))
            physical.pop_back();
        if (logical.empty()) logical_line = line_no;
        if (!physical.empty() && physical.back() == '\\') {
            physical.pop_back();
            logical += physical;
            logical += ' ';
            continue;
        }
        logical += physical;
        auto const tokens = split(logical);
        logical.clear();
        if (tokens.empty()) continue;

        auto const &head = tokens[0];
        if (head[0] == '.') {
            in_names = false;
            if (head == ".model") {
                if (tokens.size() > 1) raw.model = tokens[1];
            } else if (head == ".inputs") {
                raw.inputs.insert(raw.inputs.end(), tokens.begin() + 1, tokens.end());
            } else if (head == ".outputs") {
                raw.outputs.insert(raw.outputs.end(), tokens.begin() + 1, tokens.end());
            } else if (head == ".names") {
                if (tokens.size() < 2) return err(at(path, logical_line) + ": .names without signals");
                NamesBlock block;
                block.inputs.assign(tokens.begin() + 1, tokens.end() - 1);
                block.output = tokens.back();
                block.line = logical_line;
                raw.blocks.push_back(std::move(block));
                in_names = true;
            } else if (head == ".end") {
                ended = true;
            } else if (directive_is_unsupported(head)) {
                return err(at(path, logical_line) + ": unsupported directive " + head);
            }
            // Other directives (.default_input_arrival, .area, ...) carry no logic: ignored.
            continue;
        }

        if (!in_names) return err(at(path, logical_line) + ": cube line outside a .names block");
        auto &block = raw.blocks.back();
        auto const k = block.inputs.size();
        Cube cube;
        if (k == 0 && tokens.size() == 1) {
            cube.output = tokens[0][0];
        } else if (k > 0 && tokens.size() == 2) {
            cube.pattern = tokens[0];
            cube.output = tokens[1][0];
        } else {
            return err(at(path, logical_line) + ": malformed cube for " + block.output);
        }
        bool const valid_pattern =
            cube.pattern.size() == k &&
            std::all_of(cube.pattern.begin(), cube.pattern.end(),
                        [](char c) { return c == '0' || c == '1' || c == '-'; });
        bool const valid_output = (cube.output == '0' || cube.output == '1') &&
                                  (k == 0 ? tokens[0].size() == 1 : tokens[1].size() == 1);
        if (!valid_pattern || !valid_output)
            return err(at(path, logical_line) + ": malformed cube for " + block.output);
        block.cubes.push_back(std::move(cube));
    }
    if (!logical.empty()) return err(path + ": file ends inside a continued line");
    return raw;
}

Result<Kind> classify(std::string const &path, NamesBlock const &b) {
    auto const fail = [&](char const *why) {
        return err(at(path, b.line) + ": node " + b.output + ": " + why +
                   " (only AND, OR and NOT are supported)");
    };
    auto const k = b.inputs.size();
    if (k == 0) return fail("constant node");
    if (k == 1) {
        bool const is_not = b.cubes.size() == 1 &&
                            ((b.cubes[0].pattern == "0" && b.cubes[0].output == '1') ||
                             (b.cubes[0].pattern == "1" && b.cubes[0].output == '0'));
        if (is_not) return Kind::Not;
        return fail("single-input node that is not an inverter");
    }
    for (auto const &c : b.cubes)
        if (c.output != '1') return fail("off-set cubes");
    if (b.cubes.size() == 1 && b.cubes[0].pattern == std::string(k, '1')) return Kind::And;
    if (b.cubes.size() == k) {
        std::vector<bool> seen(k, false);
        bool is_or = true;
        for (auto const &c : b.cubes) {
            auto const one = c.pattern.find('1');
            bool const single_one = one != std::string::npos &&
                                    std::count(c.pattern.begin(), c.pattern.end(), '1') == 1 &&
                                    std::count(c.pattern.begin(), c.pattern.end(), '-') ==
                                        static_cast<std::ptrdiff_t>(k - 1);
            if (!single_one || seen[one]) {
                is_or = false;
                break;
            }
            seen[one] = true;
        }
        if (is_or) return Kind::Or;
    }
    return fail("unsupported logic function");
}

Csr transpose(Csr const &csr, NodeId rows) {
    Csr t;
    t.offset.assign(rows + 1, 0);
    for (auto const v : csr.item) ++t.offset[v + 1];
    for (NodeId i = 0; i < rows; ++i) t.offset[i + 1] += t.offset[i];
    t.item.resize(csr.item.size());
    auto cursor = t.offset;
    for (NodeId u = 0; u < rows; ++u)
        for (auto const v : csr.row(u)) t.item[cursor[v]++] = u;
    return t;
}

} // namespace

Result<Netlist> read_blif(std::string const &path) {
    TRY(auto raw, parse(path));

    Netlist n;
    n.model = raw.model;
    std::unordered_map<std::string, NodeId> id_of;
    id_of.reserve(raw.inputs.size() + raw.blocks.size());
    auto const define = [&](std::string const &name, Kind kind) -> Result<NodeId> {
        auto const id = static_cast<NodeId>(n.name.size());
        if (!id_of.emplace(name, id).second) return err(path + ": signal " + name + " defined twice");
        n.name.push_back(name);
        n.kind.push_back(kind);
        return id;
    };

    for (auto const &name : raw.inputs) {
        TRY(auto const id, define(name, Kind::Pi));
        n.inputs.push_back(id);
    }
    for (auto const &block : raw.blocks) {
        TRY(auto const kind, classify(path, block));
        TRY(auto const id, define(block.output, kind));
        (void)id;
        ++n.op_count[op_index(kind)];
    }

    // Fanin CSR, duplicate fanins removed
    n.fanin.offset.assign(1, 0);
    for (NodeId i = 0; i < static_cast<NodeId>(raw.inputs.size()); ++i)
        n.fanin.offset.push_back(0);
    for (auto const &block : raw.blocks) {
        auto const begin = n.fanin.item.size();
        for (auto const &in : block.inputs) {
            auto const it = id_of.find(in);
            if (it == id_of.end())
                return err(at(path, block.line) + ": signal " + in + " (input of " + block.output +
                           ") is never defined");
            n.fanin.item.push_back(it->second);
        }
        std::sort(n.fanin.item.begin() + begin, n.fanin.item.end());
        n.fanin.item.erase(std::unique(n.fanin.item.begin() + begin, n.fanin.item.end()),
                           n.fanin.item.end());
        n.fanin.offset.push_back(static_cast<std::int32_t>(n.fanin.item.size()));
    }
    n.fanout = transpose(n.fanin, n.size());

    for (auto const &name : raw.outputs) {
        auto const it = id_of.find(name);
        if (it == id_of.end()) return err(path + ": output " + name + " is never defined");
        n.outputs.push_back(it->second);
    }

    // Kahn's algorithm in id order, so the order is deterministic
    std::vector<std::int32_t> pending(n.size());
    for (NodeId i = 0; i < n.size(); ++i)
        pending[i] = static_cast<std::int32_t>(n.fanin.row(i).size());
    std::vector<NodeId> queue;
    queue.reserve(n.size());
    for (NodeId i = 0; i < n.size(); ++i)
        if (pending[i] == 0) queue.push_back(i);
    for (std::size_t head = 0; head < queue.size(); ++head) {
        auto const u = queue[head];
        if (n.is_op(u)) n.topo.push_back(u);
        for (auto const v : n.fanout.row(u))
            if (--pending[v] == 0) queue.push_back(v);
    }
    if (static_cast<NodeId>(queue.size()) != n.size()) {
        for (NodeId i = 0; i < n.size(); ++i)
            if (pending[i] > 0) return err(path + ": combinational cycle through " + n.name[i]);
    }
    return n;
}
