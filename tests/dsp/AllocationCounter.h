#pragma once

#include <atomic>

// Counts calls to global operator new in the DSP test runner (tests/dsp/main.cpp).
namespace pa::test
{
extern std::atomic<long> allocationCount;

// Allocations made between construction and allocations().
class AllocationProbe
{
  public:
    long allocations() const { return allocationCount.load() - start; }

  private:
    long start = allocationCount.load();
};
} // namespace pa::test
