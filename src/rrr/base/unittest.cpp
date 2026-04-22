module;

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <rusty/rusty.hpp>

#include <string.h>


#include <stdio.h>




module rrr:impl.base.unittest;

import std;

import rrr;

// @external: {
//   printf: [unsafe],
//   strlen: [unsafe],
//   delete: [unsafe],
//   new: [unsafe]
// }

// NOTE: This file is unit test infrastructure. It uses raw pointers, new/delete,
// and C-style I/O. All functions are marked @unsafe as they manage test case
// lifecycle with manual memory management.

namespace rrr {

// @safe - Simple counter increment
void TestCase::fail() {
    failures_++;
}

TestMgr* TestMgr::instance_s = nullptr;

// @unsafe - Uses raw pointer and new for singleton pattern
TestMgr* TestMgr::instance() {
    if (instance_s == nullptr) {
        instance_s = new TestMgr;  // @unsafe
    }
    return instance_s;
}

// @unsafe - Stores raw pointer in container
TestCase* TestMgr::reg(TestCase* t) {
    tests_.push(t);  // @unsafe
    return t;
}

// @unsafe - Uses raw pointers and C-string operations
void TestMgr::matched_tests(const char* match, rusty::Vec<TestCase*>* matched) {
    rusty::Vec<std::string>&& split = strsplit(match, ',');  // @unsafe
    matched->clear();  // @unsafe
    for (auto& t: tests_) {
        for (auto& s: split) {
            if (s.find('/') != std::string::npos) {
                if (t->group() + std::string("/") + t->name() == s) {
                    matched->push(t);  // @unsafe
                }
            } else {
                if (t->group() == s) {
                    matched->push(t);  // @unsafe
                }
            }
        }
    }
}

// @unsafe - Uses raw pointers, C-strings, strlen
int TestMgr::parse_args(int argc, char* argv[], bool* show_help, bool* list_tests, rusty::Vec<TestCase*>* selected) {
    *show_help = false;
    *list_tests = false;
    char* select = nullptr;
    char* skip = nullptr;
    rusty::Vec<TestCase*> match;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {  // @unsafe
            *show_help = true;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) {  // @unsafe
            *list_tests = true;
        } else if (startswith(argv[i], "--select=")) {  // @unsafe
            select = argv[i] + strlen("--select=");  // @unsafe
            matched_tests(select, &match);  // @unsafe
        } else if (startswith(argv[i], "--skip=")) {  // @unsafe
            skip = argv[i] + strlen("--skip=");  // @unsafe
            matched_tests(skip, &match);  // @unsafe
        } else {
            return 1;
        }
    }
    if (select == nullptr && skip == nullptr) {
        selected->clear();  // @unsafe
        selected->reserve(tests_.len());  // @unsafe
        for (auto& t : tests_) {
            selected->push(t);  // @unsafe
        }
    } else if (select != nullptr && skip == nullptr) {
        selected->clear();  // @unsafe
        selected->reserve(match.len());  // @unsafe
        for (auto& t : match) {
            selected->push(t);  // @unsafe
        }
    } else if (select == nullptr && skip != nullptr) {
        selected->clear();  // @unsafe
        for (auto& t: tests_) {
            bool select_me = true;
            for (auto& m: match) {
                if (t == m) {
                    select_me = false;
                }
            }
            if (select_me) {
                selected->push(t);  // @unsafe
            }
        }
    } else { // select != nullptr && skip != nullptr
        printf("please provide either --select or --skip, not both\n");  // @unsafe
        return 1;
    }
    return 0;
}

// @unsafe - Uses printf, raw pointers, delete
int TestMgr::run(int argc, char* argv[]) {
    bool show_help;
    bool list_tests;
    rusty::Vec<TestCase*> selected;
    int r = parse_args(argc, argv, &show_help, &list_tests, &selected);  // @unsafe
    if (r != 0 || show_help) {
        printf("usage: %s [-h|--help] [-l|--list] [--select,skip=group_x/test_y,group_z]\n", argv[0]);  // @unsafe
        return r;
    }
    if (list_tests) {
        for (auto& t : selected) {
            printf("%s/%s\n", t->group(), t->name());  // @unsafe
        }
        return r;
    }

    int failures = 0;
    int passed = 0;
    if (selected.size() > 0) {
        Log::info("The following %d test cases will be checked:", selected.size());  // @unsafe
        for (auto& t : selected) {
            Log::info("    %s/%s", t->group(), t->name());  // @unsafe
        }
    }
    for (auto& t : selected) {
        Log::info("--> starting test: %s/%s", t->group(), t->name());  // @unsafe
        t->run();  // @unsafe
        failures += t->failures();
        if (t->failures() == 0) {
            Log::info("<-- passed test: %s/%s", t->group(), t->name());  // @unsafe
            passed++;
        } else {
            Log::error("X-- failed test: %s/%s", t->group(), t->name());  // @unsafe
        }
    }
    Log::info("%d/%lu passed, %d failures\n", passed, selected.size(), failures);  // @unsafe
    // cleanup testcases
    for (auto& t : tests_) {
        delete t;  // @unsafe
    }
    delete this;  // @unsafe
    return failures;
}

} // namespace rrr
