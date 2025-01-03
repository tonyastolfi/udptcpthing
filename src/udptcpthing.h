#pragma once

#include <vector>
#include <string>


#ifdef _WIN32
  #define UDPTCPTHING_EXPORT __declspec(dllexport)
#else
  #define UDPTCPTHING_EXPORT
#endif

UDPTCPTHING_EXPORT void udptcpthing();
UDPTCPTHING_EXPORT void udptcpthing_print_vector(const std::vector<std::string> &strings);
