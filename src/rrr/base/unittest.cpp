module;

#include <rusty/once.hpp>
#include <rusty/rusty.hpp>
#include <string.h>
#include <stdio.h>

export module rrr.unittest;

import std;
import rusty;
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
    // No user-declared default constructor; the implicit default
    // calls `tests_`'s default ctor (an empty `rusty::Vec`) — same
    // effect as the previous `TestMgr() :tests_() { }`. The private
    // `static TestMgr new_()` factory below is the only construction
    // path; called exclusively from `instance()`.
    rusty::Vec<TestCase*> tests_;

    // @safe - Rust-style factory matching `fn new() -> Self`.
    // Returns a fresh `TestMgr` as a prvalue, so C++17 mandatory copy
    // elision installs it directly into the OnceCell storage.
    static TestMgr new_() { return TestMgr{}; }

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

// @safe - Rust-idiomatic singleton accessor.
//
// Equivalent in Rust:
//   static TEST_MGR: OnceLock<TestMgr> = OnceLock::new();
//   pub fn instance() -> &'static mut TestMgr { TEST_MGR.get_or_init(TestMgr::new_) }
//
// The previous form used a raw `TestMgr* instance_s` static + lazy
// `new TestMgr` on first call — not thread-safe, but in practice
// safe because test registration runs at static-init time (before
// `main`, single-threaded). `rusty::OnceCell<T>` gives us the same
// laziness with proper thread-safety, no leak, and a direct mapping
// to `std::sync::OnceLock<T>` in Rust.
//
// `instance()` still returns a raw `TestMgr*` to keep all the
// existing call sites (`TestMgr::instance()->...`) compiling
// unchanged. `OnceCell::get_mut()` is guaranteed non-null because
// `get_or_init` just initialized the cell.
TestMgr* TestMgr::instance() {
    static rusty::OnceCell<TestMgr> inst;
    inst.get_or_init([]() -> TestMgr { return TestMgr::new_(); });
    return inst.get_mut();
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
