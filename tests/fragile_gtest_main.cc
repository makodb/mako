// Fragile gtest main: avoids including the full gtest/gtest.h header.
// RUN_ALL_TESTS() is an inline function in gtest.h and has no linkable symbol
// in libgtest.a. Instead, call the underlying UnitTest::GetInstance()->Run()
// which ARE regular member functions with linkable symbols.
namespace testing {
void InitGoogleTest(int* argc, char** argv);
class UnitTest {
public:
    static UnitTest* GetInstance();
    int Run();
};
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return testing::UnitTest::GetInstance()->Run();
}
