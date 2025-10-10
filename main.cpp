#include <seaplane/log.hpp>
#include <seaplane/variant.hpp>

int main()
{
    using namespace seaplane;
    sea_log("Hello {}", "seaplane");

    Variant i = 6;
    Variant d = 0.5;
    Variant b = true;
    Variant s = "Yellow?";

    std::println("{},{},{},{}", i, d, b, s);

    auto i2 = i.as<int>();
    auto d2 = d.as<double>();
    auto b2 = b.as<bool>();
    auto s2 = s.as<std::string>();

    std::println("{},{},{},{}", i2, d2, b2, s2);
}
