#ifdef ENABLE_RDMA

#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <memory>
#include "transport/rdma_transport.h"
#include "transport/transport.h"
#include "base/all.hpp"

using namespace rrr;
using namespace std::chrono;

class RDMATransportTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Check if RDMA is available
        // In Docker with SoftRoCE, this may not be available
        // Skip tests if RDMA devices are not found
    }
    
    void TearDown() override {
    }
};

// Test RDMA context initialization
TEST_F(RDMATransportTest, RDMAContextInitialization) {
    RDMAContext ctx;
    
    // Try to initialize - may fail if no RDMA devices available
    bool initialized = ctx.initialize();
    
    if (!initialized) {
        GTEST_SKIP() << "No RDMA devices available (expected in Docker without SoftRoCE)";
    }
    
    EXPECT_TRUE(ctx.is_initialized());
    EXPECT_NE(ctx.get_context(), nullptr);
    EXPECT_NE(ctx.get_pd(), nullptr);
    EXPECT_NE(ctx.get_cq(), nullptr);
    EXPECT_GT(ctx.get_cq_fd(), 0);
    
    ctx.cleanup();
}

// Test RDMA transport creation
TEST_F(RDMATransportTest, RDMATransportCreation) {
    auto transport = create_rdma_transport("rdma://127.0.0.1:12345");
    
    if (!transport) {
        GTEST_SKIP() << "RDMA transport creation failed (expected if RDMA not enabled or no devices)";
    }
    
    EXPECT_NE(transport, nullptr);
    EXPECT_EQ(transport->type(), TransportType::RDMA);
}

// Test RDMA connection (requires actual RDMA setup)
// This test will be skipped if RDMA is not properly configured
TEST_F(RDMATransportTest, RDMATransportConnection) {
    // This test requires:
    // 1. RDMA devices available
    // 2. SoftRoCE or real RDMA hardware
    // 3. Proper network configuration
    
    auto transport = create_rdma_transport("rdma://127.0.0.1:12345");
    
    if (!transport) {
        GTEST_SKIP() << "RDMA transport not available";
    }
    
    // Try to bind (server side)
    int ret = transport->bind("rdma://0.0.0.0:12345");
    if (ret != 0) {
        GTEST_SKIP() << "RDMA bind failed (may need privileged Docker or SoftRoCE setup)";
    }
    
    // Try to listen
    ret = transport->listen();
    EXPECT_EQ(ret, 0);
    
    transport->close();
}

// Test RDMA transport with invalid address
TEST_F(RDMATransportTest, RDMATransportInvalidAddress) {
    auto transport = create_rdma_transport("rdma://invalid:address");
    
    if (!transport) {
        GTEST_SKIP() << "RDMA transport not available";
    }
    
    // Should fail with invalid address
    int ret = transport->connect("rdma://invalid:address");
    EXPECT_NE(ret, 0);
}

// Test RDMA transport type detection
TEST_F(RDMATransportTest, RDMATransportType) {
    auto transport = create_rdma_transport("rdma://127.0.0.1:12345");
    
    if (!transport) {
        GTEST_SKIP() << "RDMA transport not available";
    }
    
    EXPECT_EQ(transport->type(), TransportType::RDMA);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

#else  // ENABLE_RDMA not defined

#include <gtest/gtest.h>

// Dummy test when RDMA is not enabled
TEST(RDMATransportTest, RDMA_NOT_ENABLED) {
    GTEST_SKIP() << "RDMA support not enabled (compile with -DENABLE_RDMA=ON)";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

#endif  // ENABLE_RDMA

