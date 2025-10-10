#pragma once

#include <seaplane/error.hpp>

#include <concepts>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

template<typename T> concept Enum = std::is_enum_v<T>;

namespace seaplane
{

enum class DataType
{
    None,
    Bool,
    Integer,
    Real,
    String
};

struct Variant
{

    Variant(std::integral auto aIntegral) : integral{static_cast<std::int64_t>(aIntegral)}, type{DataType::Integer}
    {
        if constexpr (std::is_same_v<decltype(aIntegral), bool>)
        {
            type = DataType::Bool;
        }
    }
    Variant(Enum auto aEnum) : Variant{std::to_underlying(aEnum)} { }
    Variant(std::floating_point auto aReal) : real{aReal}, type{DataType::Real} { }
    Variant(std::string aText) : text{aText}, type{DataType::String} { }
    Variant(std::string_view aText) : text{aText}, type{DataType::String} { }
    Variant(const char* aText) : text{aText}, type{DataType::String} { }

    Variant(const Variant& aOther)
    {
        type = aOther.type;
        switch (aOther.type)
        {
            case DataType::String:
                text = aOther.text;
                return;
            case DataType::Integer:
            case DataType::Bool:
                integral = aOther.integral;
                return;
            case DataType::Real:
                real = aOther.real;
                return;
            case DataType::None:
                return;
        }
    }

    Variant(Variant&& aOther)
    {
        type = aOther.type;
        switch (aOther.type)
        {
            case DataType::String:
                text = std::move(aOther.text);
                return;
            case DataType::Integer:
            case DataType::Bool:
                integral = aOther.integral;
                return;
            case DataType::Real:
                real = aOther.real;
                return;
            case DataType::None:
                return;
        }
    }

    ~Variant()
    {
        switch (type)
        {
            case DataType::String:
                using TextT = decltype(text);
                text.~TextT();
                return;
            case DataType::None:
            case DataType::Integer:
            case DataType::Bool:
            case DataType::Real:
                return;
        }
    }

    union
    {
        std::int64_t integral;
        double real;
        std::string text;
    };

    DataType type{};
};

}

template<>
struct std::formatter<seaplane::Variant>
{
    constexpr auto parse(std::format_parse_context& aContext)
    {
        return aContext.begin();
    }

    auto format(const seaplane::Variant& aVariant, std::format_context& aContext) const
    {
        switch (aVariant.type)
        {
        case seaplane::DataType::None:
            return std::format_to(aContext.out(), "");
        case seaplane::DataType::Integer:
            return std::format_to(aContext.out(), "{}", aVariant.integral);
        case seaplane::DataType::Bool:
            return std::format_to(aContext.out(), "{}", static_cast<bool>(aVariant.integral));
        case seaplane::DataType::Real:
            return std::format_to(aContext.out(), "{}", aVariant.real);
        case seaplane::DataType::String:
            return std::format_to(aContext.out(), "{}", aVariant.text);
        }

        throw FlushingError{"Invalid Variant type"};
    }
};
