#pragma once

// Module shim: classes come from rrr.unittest. Macros stay textual
// here since macros can't be exported via modules.
#include <iostream>
import rrr.unittest;

#define TEST_CLASS_NAME(group, name) \
    TestCase_ ## group ## _ ## name

#define TEST(group, name) \
    class TEST_CLASS_NAME(group, name) : public ::base::TestCase { \
        static TestCase* me_s; \
    public: \
        TEST_CLASS_NAME(group, name)(); \
        void run(); \
    }; \
    TEST_CLASS_NAME(group, name)::TEST_CLASS_NAME(group, name)(): \
        TestCase(#group, #name) { } \
    ::base::TestCase* TEST_CLASS_NAME(group, name)::me_s = \
        ::base::TestMgr::instance()->reg(new TEST_CLASS_NAME(group, name)); \
    void TEST_CLASS_NAME(group, name)::run()

#define RUN_TESTS(argc, argv) ::base::TestMgr::instance()->run((argc), (argv));

#define EXPECT_TRUE(a) \
    { \
        auto va = (a); \
        if (!va) { \
            fail(); \
            std::cout << "    *** expected true: '" << #a << "', got false (" \
                << __FILE__ << ':' << __LINE__  << ')' << std::endl; \
        } \
    }

#define EXPECT_FALSE(a) \
    { \
        auto va = (a); \
        if (va) { \
            fail(); \
            std::cout << "    *** expected false: '" << #a << "', got true (" \
                << __FILE__ << ':' << __LINE__  << ')' << std::endl; \
        } \
    }

#define EXPECT_BINARY_OP_GENERATOR(op, a, b) \
    { \
        auto va = (a); \
        auto vb = (b); \
        if (!(va op vb)) { \
            fail(); \
            std::cout << "    *** expected: '" \
                << #a << ' ' << #op << ' ' << #b \
                << "', got " << va << " and " << vb \
                << " (" << __FILE__ << ':' << __LINE__ << ')' << std::endl; \
        } \
    }

#define EXPECT_LT(a, b) EXPECT_BINARY_OP_GENERATOR(<, (a), (b))
#define EXPECT_LE(a, b) EXPECT_BINARY_OP_GENERATOR(<=, (a), (b))
#define EXPECT_GT(a, b) EXPECT_BINARY_OP_GENERATOR(>, (a), (b))
#define EXPECT_GE(a, b) EXPECT_BINARY_OP_GENERATOR(>=, (a), (b))
#define EXPECT_EQ(a, b) EXPECT_BINARY_OP_GENERATOR(==, (a), (b))
#define EXPECT_NEQ(a, b) EXPECT_BINARY_OP_GENERATOR(!=, (a), (b))
