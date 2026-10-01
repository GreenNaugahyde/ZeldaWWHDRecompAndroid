// shim: boost::container::static_vector as a reserved std::vector
#pragma once
#include <vector>
namespace boost { namespace container {
template <typename T, std::size_t N>
class static_vector : public std::vector<T> {
public:
    static_vector() { this->reserve(N); }
    using std::vector<T>::vector;
};
}}
