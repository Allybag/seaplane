#pragma once

#include <cstdio>
#include <print>
#include <stdexcept>

class FlushingError : public std::runtime_error
{
public:
    FlushingError(const std::string& what) : std::runtime_error(what)
    {
        std::println("{}", what);
        std::fflush(nullptr); // Flush all output buffers
    }
};
