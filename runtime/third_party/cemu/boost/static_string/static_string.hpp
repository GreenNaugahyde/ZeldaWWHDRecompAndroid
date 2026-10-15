// shim: boost::static_string<N> as std::string with a fixed capacity (Cemu's PPC assembler)
#pragma once
#include <string>
namespace boost {
template <std::size_t N>
class static_string : public std::string {
public:
    std::size_t capacity() const { return N; }
};
}  // namespace boost
