#pragma once

// Allocations (global operator new) made by the calling thread so far, in the plugin test runner (tests/main.cpp).
// Per thread, so JUCE's own threads can't make a check of the audio path fail.
namespace pa::test
{
long threadAllocations();
}
