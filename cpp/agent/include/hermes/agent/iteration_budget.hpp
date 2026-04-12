// Iteration counter used by AIAgent's main loop to bound runaway
// tool-call sequences.
#pragma once

#include <stdexcept>

namespace hermes::agent {

class IterationBudget {
public:
    explicit IterationBudget(int total) : total_(total), used_(0) {
        if (total < 0) throw std::invalid_argument("budget must be non-negative");
    }

    int total() const { return total_; }
    int used() const { return used_; }
    int remaining() const { return total_ - used_; }
    bool exhausted() const { return used_ >= total_; }

    // Consume `n` iterations.  Allowed to overshoot — exhausted() then
    // reflects the saturation.
    void consume(int n = 1) {
        if (n < 0) throw std::invalid_argument("consume(n) needs n>=0");
        used_ += n;
    }

    void reset() { used_ = 0; }

private:
    int total_;
    int used_;
};

}  // namespace hermes::agent
