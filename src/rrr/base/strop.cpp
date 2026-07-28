module;

#include <rusty/rusty.hpp>
#include <string.h>

export module rrr.strop;

import std;
import rusty;

// @safe - string ops. format_decimal and strsplit are pure std::string
// + ostringstream + rusty::Vec; startswith/endswith carry per-method
// `// @unsafe` because they take raw `const char*` and call strlen /
// strncmp with pointer arithmetic.
namespace rrr {

// @unsafe - raw `const char*` plus strlen/strncmp libc calls.
export bool startswith(const char* str, const char* head) {
    size_t len_str = strlen(str);
    size_t len_head = strlen(head);
    if (len_head > len_str) {
        return false;
    }
    return strncmp(str, head, len_head) == 0;
}

// @unsafe - raw `const char*` plus strlen/strncmp + pointer arithmetic
// (`str + (len_str - len_tail)`).
export bool endswith(const char* str, const char* tail) {
    size_t len_str = strlen(str);
    size_t len_tail = strlen(tail);
    if (len_tail > len_str) {
        return false;
    }
    return strncmp(str + (len_str - len_tail), tail, len_tail) == 0;
}

export std::string format_decimal(double val) {
    std::ostringstream o;
    o.precision(2);
    o << std::fixed << val;
    std::string s(o.str());
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

export std::string format_decimal(int val) {
    std::ostringstream o;
    o << val;
    std::string s(o.str());
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

export rusty::Vec<std::string> strsplit(const std::string& str, const char sep = ' ') {
    rusty::Vec<std::string> split;
    size_t begin, end;
    begin = str.find_first_not_of(sep);
    while ((end = str.find(sep, begin)) != std::string::npos) {
        split.push(str.substr(begin, end - begin));
        begin = str.find_first_not_of(sep, end);
    }
    if (begin != std::string::npos && begin < str.size()) {
        split.push(str.substr(begin));
    }
    return split;
}

} // namespace rrr
