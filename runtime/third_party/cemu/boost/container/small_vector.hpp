// shim: boost::container::small_vector as std::vector
#pragma once
#include <vector>
namespace boost { namespace container {
template <typename T, std::size_t N>
class small_vector : public std::vector<T> {
public:
    small_vector() { this->reserve(N); }
    using std::vector<T>::vector;
};
}}
