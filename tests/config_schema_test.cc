/**
 * @file config_schema_test.cc
 * @brief Unit tests for configuration schema serialization.
 */

#include <gtest/gtest.h>
#include "../src/deptran/config_schema.h"

namespace janus {
namespace test {

// Test PersistentSiteInfo serialization roundtrip
// @safe - archive serde roundtrip
TEST(ConfigSchemaTest, SiteInfoSerializationRoundtrip) {
    PersistentSiteInfo original;
    original.id = 1;
    original.locale_id = 10;
    original.name = "server1";
    original.proc_name = "proc1";
    original.role = 0;  // leader
    original.host = "192.168.1.1";
    original.port = 8080;
    original.n_thread = 4;
    original.type = 1;  // SERVER
    original.partition_id = 0;

    // Serialize
    rrr::BufferSink __sink__;
    {
        rrr::BinaryWriteArchive __war__(rrr::make_sink_proxy(&__sink__));
        rrr::Serialize_::serialize(original, __war__);
    }
    rrr::BufferSource __src__(__sink__.bytes.data(), __sink__.bytes.len());
    rrr::BinaryReadArchive __rar__(rrr::make_source_proxy(&__src__));

    // Deserialize
    PersistentSiteInfo restored;
    rrr::Deserialize_::deserialize(restored, __rar__);

    // Verify
    EXPECT_EQ(original.id, restored.id);
    EXPECT_EQ(original.locale_id, restored.locale_id);
    EXPECT_EQ(original.name, restored.name);
    EXPECT_EQ(original.proc_name, restored.proc_name);
    EXPECT_EQ(original.role, restored.role);
    EXPECT_EQ(original.host, restored.host);
    EXPECT_EQ(original.port, restored.port);
    EXPECT_EQ(original.n_thread, restored.n_thread);
    EXPECT_EQ(original.type, restored.type);
    EXPECT_EQ(original.partition_id, restored.partition_id);
}

// Test PersistentReplicaGroup serialization roundtrip
// @safe - archive serde roundtrip
TEST(ConfigSchemaTest, ReplicaGroupSerializationRoundtrip) {
    PersistentReplicaGroup original;
    original.partition_id = 5;
    original.replica_ids = {1, 2, 3};

    // Serialize
    rrr::BufferSink __sink__;
    {
        rrr::BinaryWriteArchive __war__(rrr::make_sink_proxy(&__sink__));
        rrr::Serialize_::serialize(original, __war__);
    }
    rrr::BufferSource __src__(__sink__.bytes.data(), __sink__.bytes.len());
    rrr::BinaryReadArchive __rar__(rrr::make_source_proxy(&__src__));

    // Deserialize
    PersistentReplicaGroup restored;
    rrr::Deserialize_::deserialize(restored, __rar__);

    // Verify
    EXPECT_EQ(original.partition_id, restored.partition_id);
    ASSERT_EQ(original.replica_ids.size(), restored.replica_ids.size());
    for (size_t i = 0; i < original.replica_ids.size(); ++i) {
        EXPECT_EQ(original.replica_ids[i], restored.replica_ids[i]);
    }
}

// Test PersistentProtocolSettings serialization roundtrip
// @safe - archive serde roundtrip
TEST(ConfigSchemaTest, ProtocolSettingsSerializationRoundtrip) {
    PersistentProtocolSettings original;
    original.tx_proto = 3;
    original.replica_proto = 1;
    original.benchmark = 2;
    original.txn_timeout_us = 60000000;
    original.scale_factor = 4;

    // Serialize
    rrr::BufferSink __sink__;
    {
        rrr::BinaryWriteArchive __war__(rrr::make_sink_proxy(&__sink__));
        rrr::Serialize_::serialize(original, __war__);
    }
    rrr::BufferSource __src__(__sink__.bytes.data(), __sink__.bytes.len());
    rrr::BinaryReadArchive __rar__(rrr::make_source_proxy(&__src__));

    // Deserialize
    PersistentProtocolSettings restored;
    rrr::Deserialize_::deserialize(restored, __rar__);

    // Verify
    EXPECT_EQ(original.tx_proto, restored.tx_proto);
    EXPECT_EQ(original.replica_proto, restored.replica_proto);
    EXPECT_EQ(original.benchmark, restored.benchmark);
    EXPECT_EQ(original.txn_timeout_us, restored.txn_timeout_us);
    EXPECT_EQ(original.scale_factor, restored.scale_factor);
}

// Test PersistentConfig serialization roundtrip
// @safe - archive serde roundtrip
TEST(ConfigSchemaTest, FullConfigSerializationRoundtrip) {
    PersistentConfig original;
    original.version = 42;

    // Add sites
    PersistentSiteInfo site1;
    site1.id = 1;
    site1.name = "server1";
    site1.host = "host1";
    site1.port = 8080;
    site1.type = 1;  // SERVER
    site1.partition_id = 0;

    PersistentSiteInfo site2;
    site2.id = 2;
    site2.name = "server2";
    site2.host = "host2";
    site2.port = 8081;
    site2.type = 1;  // SERVER
    site2.partition_id = 0;

    PersistentSiteInfo site3;
    site3.id = 3;
    site3.name = "server3";
    site3.host = "host3";
    site3.port = 8082;
    site3.type = 1;  // SERVER
    site3.partition_id = 1;

    original.sites = {site1, site2, site3};

    // Add replica groups
    PersistentReplicaGroup group1;
    group1.partition_id = 0;
    group1.replica_ids = {1, 2};

    PersistentReplicaGroup group2;
    group2.partition_id = 1;
    group2.replica_ids = {3};

    original.replica_groups = {group1, group2};

    // Set protocol settings
    original.settings.tx_proto = 5;
    original.settings.replica_proto = 2;
    original.settings.benchmark = 1;
    original.settings.txn_timeout_us = 45000000;
    original.settings.scale_factor = 2;

    // Serialize
    rrr::BufferSink __sink__;
    {
        rrr::BinaryWriteArchive __war__(rrr::make_sink_proxy(&__sink__));
        rrr::Serialize_::serialize(original, __war__);
    }
    rrr::BufferSource __src__(__sink__.bytes.data(), __sink__.bytes.len());
    rrr::BinaryReadArchive __rar__(rrr::make_source_proxy(&__src__));

    // Deserialize
    PersistentConfig restored;
    rrr::Deserialize_::deserialize(restored, __rar__);

    // Verify version
    EXPECT_EQ(original.version, restored.version);

    // Verify sites
    ASSERT_EQ(original.sites.size(), restored.sites.size());
    for (size_t i = 0; i < original.sites.size(); ++i) {
        EXPECT_EQ(original.sites[i].id, restored.sites[i].id);
        EXPECT_EQ(original.sites[i].name, restored.sites[i].name);
        EXPECT_EQ(original.sites[i].host, restored.sites[i].host);
        EXPECT_EQ(original.sites[i].port, restored.sites[i].port);
        EXPECT_EQ(original.sites[i].type, restored.sites[i].type);
        EXPECT_EQ(original.sites[i].partition_id, restored.sites[i].partition_id);
    }

    // Verify replica groups
    ASSERT_EQ(original.replica_groups.size(), restored.replica_groups.size());
    for (size_t i = 0; i < original.replica_groups.size(); ++i) {
        EXPECT_EQ(original.replica_groups[i].partition_id, restored.replica_groups[i].partition_id);
        ASSERT_EQ(original.replica_groups[i].replica_ids.size(),
                  restored.replica_groups[i].replica_ids.size());
        for (size_t j = 0; j < original.replica_groups[i].replica_ids.size(); ++j) {
            EXPECT_EQ(original.replica_groups[i].replica_ids[j],
                      restored.replica_groups[i].replica_ids[j]);
        }
    }

    // Verify settings
    EXPECT_EQ(original.settings.tx_proto, restored.settings.tx_proto);
    EXPECT_EQ(original.settings.replica_proto, restored.settings.replica_proto);
    EXPECT_EQ(original.settings.benchmark, restored.settings.benchmark);
    EXPECT_EQ(original.settings.txn_timeout_us, restored.settings.txn_timeout_us);
    EXPECT_EQ(original.settings.scale_factor, restored.settings.scale_factor);
}

// Test empty config serialization
// @safe - archive serde roundtrip
TEST(ConfigSchemaTest, EmptyConfigSerialization) {
    PersistentConfig original;
    original.version = 1;
    // Leave sites and replica_groups empty

    // Serialize
    rrr::BufferSink __sink__;
    {
        rrr::BinaryWriteArchive __war__(rrr::make_sink_proxy(&__sink__));
        rrr::Serialize_::serialize(original, __war__);
    }
    rrr::BufferSource __src__(__sink__.bytes.data(), __sink__.bytes.len());
    rrr::BinaryReadArchive __rar__(rrr::make_source_proxy(&__src__));

    // Deserialize
    PersistentConfig restored;
    rrr::Deserialize_::deserialize(restored, __rar__);

    // Verify
    EXPECT_EQ(original.version, restored.version);
    EXPECT_TRUE(restored.sites.empty());
    EXPECT_TRUE(restored.replica_groups.empty());
}

// Test config keys are defined correctly
// @safe
TEST(ConfigSchemaTest, KeysAreDefined) {
    EXPECT_STREQ(config_keys::VERSION, "config/version");
    EXPECT_STREQ(config_keys::SITES, "config/topology/sites");
    EXPECT_STREQ(config_keys::REPLICAS, "config/topology/replicas");
    EXPECT_STREQ(config_keys::SETTINGS, "config/settings");
}

// Test default values
// @safe
TEST(ConfigSchemaTest, DefaultValues) {
    PersistentSiteInfo site;
    EXPECT_EQ(site.id, 0u);
    EXPECT_EQ(site.locale_id, 0u);
    EXPECT_EQ(site.role, 0);
    EXPECT_EQ(site.port, 0u);
    EXPECT_EQ(site.n_thread, 1u);
    EXPECT_EQ(site.type, 1);  // SERVER
    EXPECT_EQ(site.partition_id, 0u);

    PersistentProtocolSettings settings;
    EXPECT_EQ(settings.tx_proto, 0);
    EXPECT_EQ(settings.replica_proto, 0);
    EXPECT_EQ(settings.benchmark, 0);
    EXPECT_EQ(settings.txn_timeout_us, 30000000u);
    EXPECT_EQ(settings.scale_factor, 1u);
}

}  // namespace test
}  // namespace janus
