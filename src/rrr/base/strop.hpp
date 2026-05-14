#pragma once

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <rusty/rusty.hpp>

#include <string.h>




namespace rrr {

#define streq(a, b) (strcmp((a), (b)) == 0)

bool startswith(const char* str, const char* head);
bool endswith(const char* str, const char* head);

// format as -#,###.##
std::string format_decimal(double val);
std::string format_decimal(int val);

rusty::Vec<std::string> strsplit(const std::string& str, const char sep = ' ');

} // namespace base
