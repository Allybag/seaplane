#pragma once

#include <seaplane/error.hpp>

#include <concepts>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

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

inline auto format_as(DataType aType)
{
    switch (aType)
    {
    case seaplane::DataType::None:
        return "None";
    case seaplane::DataType::Integer:
        return "Integer";
    case seaplane::DataType::Bool:
        return "Bool";
    case seaplane::DataType::Real:
        return "Real";
    case seaplane::DataType::String:
        return "String";
    }

    throw FlushingError{"Invalid Variant type"};
}

template<typename T> concept Enum = std::is_enum_v<T>;

struct Variant
{
    Variant() {}

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
                emplace_text(aOther.text);
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
                emplace_text(std::move(aOther.text));
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

    Variant& operator=(const Variant& aOther)
    {
        if (this == &aOther) return *this;

        if (type != DataType::None && type != aOther.type)
        {
            throw FlushingError{std::format("Cannot assign Variant of type {} to {}", format_as(type), format_as(aOther.type))};
        }

        if (type == DataType::String)
        {
            text = aOther.text;
            return *this;
        }
        else if (type == DataType::None && aOther.type == DataType::String)
        {
            type = DataType::String;
            emplace_text(aOther.text);
            return *this;
        }

        type = aOther.type;
        switch (type)
        {
            case DataType::Integer:
            case DataType::Bool:
                integral = aOther.integral;
                return *this;
            case DataType::Real:
                real = aOther.real;
                return *this;
            case DataType::None:
            case DataType::String:
                return *this;
        }
    }

    Variant& operator=(Variant&& aOther)
    {
        if (this == &aOther) return *this;

        if (type != DataType::None && type != aOther.type)
        {
            throw FlushingError{std::format("Cannot assign Variant of type {} to {}", format_as(type), format_as(aOther.type))};
        }

        if (type == DataType::String)
        {
            text = std::move(aOther.text);
            return *this;
        }
        else if (type == DataType::None && aOther.type == DataType::String)
        {
            type = DataType::String;
            emplace_text(std::move(aOther.text));
            return *this;
        }

        type = aOther.type;
        switch (type)
        {
            case DataType::Integer:
            case DataType::Bool:
                integral = aOther.integral;
                return *this;
            case DataType::Real:
                real = aOther.real;
                return *this;
            case DataType::None:
            case DataType::String:
                return *this;
        }
    }

    DataType data_type() const
    {
        return type;
    }

    template <typename T>
    T as() const
    {
        if constexpr (std::is_same_v<T, bool>)
        {
            if (type == DataType::Bool)
            {
                return static_cast<bool>(integral);
            }
        }
        else if constexpr (std::integral<T>)
        {
            if (type == DataType::Integer)
            {
                return integral;
            }
        }
        else if constexpr (std::is_same_v<T, double> || std::is_same_v<T, float>)
        {
            if (type == DataType::Real)
            {
                return real;
            }
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
            if (type == DataType::String)
            {
                return text;
            }
        }

        throw FlushingError{std::format("Wrong type for seaplane::Variant containing {}", format())};
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

    std::string format() const
    {
        switch (type)
        {
        case seaplane::DataType::None:
            return std::format("");
        case seaplane::DataType::Integer:
            return std::format("{}", integral);
        case seaplane::DataType::Bool:
            return std::format("{}", static_cast<bool>(integral));
        case seaplane::DataType::Real:
            return std::format("{}", real);
        case seaplane::DataType::String:
            return std::format("{}", text);
        }

        throw FlushingError{"Invalid Variant type"};
    }

private:
    void emplace_text(const std::string& aText)
    {
        new (&text) std::string(aText);
    }

    void emplace_text(std::string&& aText)
    {
        new (&text) std::string(std::move(aText));
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
        return std::format_to(aContext.out(), "{}", aVariant.format());
    }
};
