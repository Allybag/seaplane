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
}
