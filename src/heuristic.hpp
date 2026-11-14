#pragma once

#include "netlist.hpp"
#include "clock.hpp"
#include "result.hpp"
#include "schedule.hpp"

// Height-priority incumbent, adopted multi-pass seed, then resource-order local search.
// The shared clock includes parsing and uses the same budgets in -h and -e modes.
Result<Schedule> list_schedule(Netlist const &n, Resources const &r, Clock const &clock);
Result<Schedule> list_schedule(Netlist const &n, Resources const &r);
