#pragma once

#include <seaplane/error.hpp>
#include <seaplane/variant.hpp>

#include <charconv>
#include <format>
#include <print>
#include <string>
#include <unordered_map>

namespace seaplane
{

using OptionMap = std::unordered_map<std::string, Variant>;

inline OptionMap parse_args(const OptionMap& default_options, int argc, const char* argv[])
{
    std::println("{}: parsing arguments", argv[0]);
    std::size_t index = 1;

    auto options = default_options;
    while (index < argc)
    {
        auto current_option = std::string(argv[index]);

        while (!current_option.empty() && current_option.front() == ' ')
        {
             current_option = current_option.substr(1);
        }

        if (current_option.front() != '-' && current_option.front() != '+')
        {
            throw FlushingError{std::format("Argument must begin with '-' (or '+' to disable): {}", current_option)};
        }

        auto myIt = options.find(current_option.substr(1));
        if (myIt == options.end())
        {
            throw FlushingError{std::format("Unexpected option: {}", current_option)};
        }

        auto& option = myIt->second;
        const auto type = option.data_type();
        if (type == DataType::Bool)
        {
            option = current_option.front() == '+' ? false : true;
            index++;
            continue;
        }

        index++;
        auto value = std::string(argv[index]);
        switch (type)
        {
            case DataType::Integer:
            {
                std::int64_t result;
                auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
                if (ec != std::errc{} || ptr != value.data() + value.size())
                {
                    throw FlushingError{std::format("Failed to parse {} as integer", value)};
                }
                option = result;
                break;
            }
            case DataType::String:
            {
                option = value;
                break;
            }
            case DataType::Real:
            {
                char* end;
                auto result = std::strtod(value.data(), &end);
                if (end != value.data() + value.size())
                {
                    throw FlushingError{std::format("Failed to parse {} as real number", value)};
                }
                option = result;
                break;
            }
            case DataType::Bool:
            {
                throw std::logic_error{"bools should not reach here"};
            }
            case DataType::None:
            {
                throw FlushingError{"Cannot have arguments without data types"};
            }
        }

        index++;
    }

    return options;
}

}
