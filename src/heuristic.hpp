#pragma once

#include "netlist.hpp"
#include "result.hpp"
#include "schedule.hpp"

// Baseline list scheduling. Each step fills every kind's units with the ready operations of the
// largest height (longest path to a sink), ties broken by node id.
Result<Schedule> list_schedule(Netlist const &n, Resources const &r);
