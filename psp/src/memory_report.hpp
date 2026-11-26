#pragma once
// Boot-time memory report (out/boot-memory.txt) and the allocation-failure text shared by the
// fatal screen and out/error.txt. Diagnostics only: no allocation behaviour changes.
#include <cstddef>
#include <exception>
#include <new>
#include <string>

namespace hkpsp::memory_report {

// Thrown where an allocation category is known (and by the test-only forced failure).
class AllocationFailure:public std::bad_alloc{
public:
    AllocationFailure(std::string category,std::size_t requested);
    const char* what() const noexcept override;
    const std::string& category()const{return category_;}
    std::size_t requested()const{return requested_;}
private:
    std::string category_;
    std::size_t requested_;
};

// Writes <out>/boot-memory.txt. Call at boot, before any large allocation.
void writeBootReport(const char* out);

// Test-only switch: <HKPSP/data>/force-alloc-failure.cfg makes the game throw an
// AllocationFailure right after the boot report. Returns true when the switch file exists.
bool forcedFailureRequested();

// "hkpsp: <what>" plus, for std::bad_alloc, category, requested bytes, heap used and max
// free block. The same text goes to the fatal screen and to <out>/error.txt.
std::string failureText(const std::exception& error);

}
