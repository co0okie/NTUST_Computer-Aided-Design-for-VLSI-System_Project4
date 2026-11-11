#pragma once

#include <string>

#include "netlist.hpp"
#include "result.hpp"

// Reads a combinational BLIF file whose every .names block is a plain AND, OR or NOT:
//   AND: one cube, all inputs '1'          (e.g. "111 1")
//   OR:  one cube per input, a single '1'  (e.g. "1- 1" / "-1 1")
//   NOT: one input, "0 1" (or "1 0")
// Anything else (latches, subcircuits, constants, buffers, other functions) is an error.
Result<Netlist> read_blif(std::string const &path);
