#pragma once

#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace JrCache {

enum EvictionPolicy{
    LRU,
    LFU,
    MRU,
    MFU,
    FIFO,
    LIFO,
};

enum CompressionPolicy{
    NONE,
    GZIP,
    ZLIB
};

// A type that can be written to a std::ostream with operator<<.
template <typename T>
concept Streamable = requires(std::ostream& os, const T& value) { os << value; };

namespace detail {

// Writes `value` as "<length>:<text>". The length prefix keeps fields apart, so ("sum", 1, 23)
// and ("sum", 12, 3) produce different text. Floating-point values are written with enough
// digits to tell any two of them apart.
template <Streamable T>
void writeField(std::ostream& out, const T& value) {
    std::ostringstream field;
    field.precision(std::numeric_limits<long double>::max_digits10);
    field << value;
    const std::string text = field.str();
    out << text.size() << ':' << text;
}

} // namespace detail

// Generates a hash value based on the function name and additional arguments.
// It writes every argument to a stringstream and hashes the resulting text.
template <Streamable... Args>
inline std::size_t HashString(const std::string& funcName, const Args&... args) {
    try {
        std::ostringstream ss;
        detail::writeField(ss, funcName);
        (detail::writeField(ss, args), ...);
        return std::hash<std::string>{}(ss.str());
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Hashing failed: ") + e.what());
    }
}

// Combines the hash of the function name and any additional arguments into a single hash value.
// It first hashes the function name and then combines it with the hash of the additional arguments.
template <Streamable... Args>
inline std::size_t Hash(const std::string& funcName, const Args&... args) {
    std::size_t seed = std::hash<std::string>{}(funcName);
    seed ^= HashString(funcName, args...) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    return seed;
}


} // namespace JrCache

