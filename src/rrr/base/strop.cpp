
// import std; replacement — see <std_compat.hpp> for rationale.
#include <stddef.h>


// @c-compat-added

#include <rusty/rusty.hpp>



#include <string.h>




#include "strop.hpp"


#include "../rrr.hpp"

import std;

// @external: {
//   strlen: [unsafe],
//   strncmp: [unsafe],
//   std::ostringstream: [unsafe],
//   std::ostringstream::str: [unsafe],
//   std::ostringstream::precision: [unsafe],
//   std::operator<<: [unsafe],
//   std::fixed: [unsafe]
// }


namespace rrr {

// @unsafe - Uses C-string functions (strlen, strncmp) which are not bounds-checked
bool startswith(const char* str, const char* head) {
    size_t len_str = strlen(str);  // @unsafe
    size_t len_head = strlen(head);  // @unsafe
    if (len_head > len_str) {
        return false;
    }
    return strncmp(str, head, len_head) == 0;  // @unsafe
}

// @unsafe - Uses C-string functions (strlen, strncmp) which are not bounds-checked
bool endswith(const char* str, const char* tail) {
    size_t len_str = strlen(str);  // @unsafe
    size_t len_tail = strlen(tail);  // @unsafe
    if (len_tail > len_str) {
        return false;
    }
    return strncmp(str + (len_str - len_tail), tail, len_tail) == 0;  // @unsafe
}

// @unsafe - Uses std::ostringstream which is not borrow-checked
std::string format_decimal(double val) {
    std::ostringstream o;  // @unsafe
    o.precision(2);  // @unsafe
    o << std::fixed << val;  // @unsafe
    std::string s(o.str());  // @unsafe
    std::string str;
    size_t idx = 0;
    while (idx < s.size()) {
        if (s[idx] == '.') {
            break;
        }
        idx++;
    }
    str.reserve(s.size() + 16);
    for (size_t i = 0; i < idx; i++) {
        if ((idx - i) % 3 == 0 && i != 0 && s[i - 1] != '-') {
            str += ',';
        }
        str += s[i];
    }
    str += s.substr(idx);
    if (str == "-0.00") {
        str = "0.00";
    }
    return str;
}

// @unsafe - Uses std::ostringstream which is not borrow-checked
std::string format_decimal(int val) {
    std::ostringstream o;  // @unsafe
    o << val;  // @unsafe
    std::string s(o.str());  // @unsafe
    std::string str;
    str.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); i++) {
        if ((s.size() - i) % 3 == 0 && i != 0 && s[i - 1] != '-') {
            str += ',';
        }
        str += s[i];
    }
    return str;
}

// @unsafe - Uses STL string parsing helpers not yet in rusty-cpp safe list
rusty::Vec<std::string> strsplit(const std::string& str, const char sep /* =? */) {
    rusty::Vec<std::string> split;
    size_t begin, end;
    begin = str.find_first_not_of(sep);  // @unsafe
    while ((end = str.find(sep, begin)) != std::string::npos) {
        split.push(str.substr(begin, end - begin));  // @unsafe
        begin = str.find_first_not_of(sep, end);  // @unsafe
    }
    if (begin != std::string::npos && begin < str.size()) {
        split.push(str.substr(begin));  // @unsafe
    }
    return split;
}

} // namespace rrr
