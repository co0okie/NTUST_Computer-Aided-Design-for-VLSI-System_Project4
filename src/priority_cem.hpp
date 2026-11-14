#pragma once
#include "resource_order.hpp"

// Research-only priority population with serial earliest-insertion decoding.
OrderSearchStats improve_priority_cem(OpGraph const &g, HeuristicResult &best,
                                      Clock const &clock, double deadline_elapsed, bool local_noise = false);
