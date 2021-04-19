#pragma once

#include <string>
#include <type_traits>
#include <utility>

namespace opossum {

unsigned int murmur_hash2(const void* key, int len, unsigned int seed);

// murmur hash for builtin number types (int, double)
template <typename T>
typename std::enable_if<std::is_arithmetic<T>::value, unsigned int>::type murmur2(T key, unsigned int seed) {
  return murmur_hash2(&key, sizeof(T), seed);
}

// murmur hash for std::string
template <typename T>
typename std::enable_if<std::is_same<T, std::string>::value, unsigned int>::type murmur2(T key, unsigned int seed) {
  return murmur_hash2(key.c_str(), key.size(), seed);
}

std::pair<uint64_t, uint64_t> murmur_hash3(const void* key, const int len, const uint32_t seed);

// murmur hash for builtin number types (int, double)
template <typename T>
typename std::enable_if<std::is_arithmetic<T>::value, std::pair<uint64_t, uint64_t>>::type murmur3(T key, uint32_t seed) {
  return murmur_hash3(&key, sizeof(T), seed);
}

// murmur hash for std::string
template <typename T>
typename std::enable_if<std::is_same<T, std::string>::value, std::pair<uint64_t, uint64_t>>::type murmur3(T key, uint32_t seed) {
  return murmur_hash3(key.c_str(), key.size(), seed);
}

}  // namespace opossum
