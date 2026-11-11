#pragma once

#include <chrono>

// Wall-clock budget measured from program start (the time limit includes file I/O and Gurobi).
struct Clock {
    using Time = std::chrono::steady_clock::time_point;
    Time start = std::chrono::steady_clock::now();
    double budget = 55; // seconds, whole program

    double elapsed() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
    double remaining() const { return budget - elapsed(); }
};
