module;

#include <rusty/rusty.hpp>
#include <string.h>
#include <stdio.h>

export module rrr.unittest;

import std;
import rrr.debugging;
import rrr.logging;
import rrr.strop;

// @safe - TestCase/TestMgr test-harness primitives. Most TestCase
// accessors return a stored raw `const char*` and most TestMgr
// methods take raw `TestCase*` / `char* argv[]`, so they carry
// per-method `// @unsafe` overrides below. Plain math/flag methods
// (`reset`, `failures`, `fail`) inherit namespace @safe.
export namespace rrr {

// @safe - see file header.
class TestCase {
    const char* group_;
    const char* name_;
    int failures_;
public:
    TestCase(const TestCase&) = delete;
    TestCase& operator=(const TestCase&) = delete;
    TestCase(const char* _group, const char* _name)
        : group_(_group), name_(_name), failures_(0) { }
    virtual ~TestCase() {}
    virtual void run() = 0;
    const char* group() { return group_; }
    const char* name() { return name_; }
    void reset() { failures_ = 0; }
    void fail();
    int failures() { return failures_; }
};

class TestMgr {
    TestMgr() :tests_() { }
    static TestMgr* instance_s;
    rusty::Vec<TestCase*> tests_;
public:
    static TestMgr* instance();
    TestCase* reg(TestCase*);
    int parse_args(int argc, char* argv[], bool* show_help, bool* list_tests, rusty::Vec<TestCase*>* selected);
    void matched_tests(const char* match, rusty::Vec<TestCase*>* matched);
    int run(int argc, char* argv[]);
};

} // export namespace rrr

// @safe - impl namespace. Most TestMgr methods take raw `TestCase*` /
// raw `char* argv[]` and carry per-method `// @unsafe`; the only
// inheritor here is `TestCase::fail` (a pure ++).
namespace rrr {

void TestCase::fail() {
    failures_++;
}

TestMgr* TestMgr::instance_s = nullptr;

// @unsafe - returns raw `TestMgr*`; `new TestMgr` raw allocation +
// static raw-pointer cache.
TestMgr* TestMgr::instance() {
    if (instance_s == nullptr) {
        instance_s = new TestMgr;
    }
    return instance_s;
}

// @unsafe - takes and returns raw `TestCase*` (lifetime not modeled).
TestCase* TestMgr::reg(TestCase* t) {
    tests_.push(t);
    return t;
}

// @unsafe - raw `const char* match`, raw `rusty::Vec<TestCase*>*`,
// dereferences stored `TestCase*` to read group/name.
void TestMgr::matched_tests(const char* match, rusty::Vec<TestCase*>* matched) {
    rusty::Vec<std::string>&& split = strsplit(match, ',');
    matched->clear();
    for (auto& t: tests_) {
        for (auto& s: split) {
            if (s.find('/') != std::string::npos) {
                if (t->group() + std::string("/") + t->name() == s) {
                    matched->push(t);
                }
            } else {
                if (t->group() == s) {
                    matched->push(t);
                }
            }
        }
    }
}

// @unsafe - raw `char* argv[]` argv, raw `char*` select/skip
// pointers from strncmp/strlen offsets, raw `bool*` out-params.
int TestMgr::parse_args(int argc, char* argv[], bool* show_help, bool* list_tests, rusty::Vec<TestCase*>* selected) {
    *show_help = false;
    *list_tests = false;
    char* select = nullptr;
    char* skip = nullptr;
    rusty::Vec<TestCase*> match;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            *show_help = true;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) {
            *list_tests = true;
        } else if (startswith(argv[i], "--select=")) {
            select = argv[i] + strlen("--select=");
            matched_tests(select, &match);
        } else if (startswith(argv[i], "--skip=")) {
            skip = argv[i] + strlen("--skip=");
            matched_tests(skip, &match);
        } else {
            return 1;
        }
    }
    if (select == nullptr && skip == nullptr) {
        selected->clear();
        selected->reserve(tests_.len());
        for (auto& t : tests_) {
            selected->push(t);
        }
    } else if (select != nullptr && skip == nullptr) {
        selected->clear();
        selected->reserve(match.len());
        for (auto& t : match) {
            selected->push(t);
        }
    } else if (select == nullptr && skip != nullptr) {
        selected->clear();
        for (auto& t: tests_) {
            bool select_me = true;
            for (auto& m: match) {
                if (t == m) {
                    select_me = false;
                }
            }
            if (select_me) {
                selected->push(t);
            }
        }
    } else {
        printf("please provide either --select or --skip, not both\n");
        return 1;
    }
    return 0;
}

// @unsafe - raw `char* argv[]` argv + `printf` + dereferences raw
// `TestCase*` plus `delete t` / `delete this` self-destruct.
int TestMgr::run(int argc, char* argv[]) {
    bool show_help;
    bool list_tests;
    rusty::Vec<TestCase*> selected;
    int r = parse_args(argc, argv, &show_help, &list_tests, &selected);
    if (r != 0 || show_help) {
        printf("usage: %s [-h|--help] [-l|--list] [--select,skip=group_x/test_y,group_z]\n", argv[0]);
        return r;
    }
    if (list_tests) {
        for (auto& t : selected) {
            printf("%s/%s\n", t->group(), t->name());
        }
        return r;
    }

    int failures = 0;
    int passed = 0;
    if (selected.size() > 0) {
        Log::info("The following %d test cases will be checked:", selected.size());
        for (auto& t : selected) {
            Log::info("    %s/%s", t->group(), t->name());
        }
    }
    for (auto& t : selected) {
        Log::info("--> starting test: %s/%s", t->group(), t->name());
        t->run();
        failures += t->failures();
        if (t->failures() == 0) {
            Log::info("<-- passed test: %s/%s", t->group(), t->name());
            passed++;
        } else {
            Log::error("X-- failed test: %s/%s", t->group(), t->name());
        }
    }
    Log::info("%d/%lu passed, %d failures\n", passed, selected.size(), failures);
    for (auto& t : tests_) {
        delete t;
    }
    delete this;
    return failures;
}

} // namespace rrr
