#pragma once


#include "mongocxx/client.hpp"
#include "mongocxx/database.hpp"
#include "mongocxx/uri.hpp"

#include "bsoncxx/builder/stream/document.hpp"
#include "bsoncxx/oid.hpp"

namespace janus {

constexpr char kMongoDbUri[] = "mongodb://184.72.49.232:27017";
constexpr char kDatabaseName[] = "JetPack";
constexpr char kCollectionName[] = "KVTable";

class MongodbKVTableHandler {
 private:
  mongocxx::uri uri;
  mongocxx::client client;
  mongocxx::database db;
  mongocxx::collection collection;
 public:
  MongodbKVTableHandler()
    : uri(mongocxx::uri(kMongoDbUri)),
      client(mongocxx::client(uri)),
      db(client[kDatabaseName]),
      collection(db[kCollectionName]) {
    Setup();
  }

  ~MongodbKVTableHandler() {
    
  }

  bool Write(int key, int value) {
    // char kCollectionName[10];
    // std::string str = std::to_string(key);
    // str.copy(kCollectionName, str.length());
    // kCollectionName[str.length()] = '\0';

    // mongocxx::collection collection = db[kCollectionName];
    auto filter_builder = bsoncxx::builder::stream::document{};
    auto update_builder = bsoncxx::builder::stream::document{};

    bsoncxx::document::value filter =
      filter_builder << "key" << key << bsoncxx::builder::stream::finalize;

    bsoncxx::document::value update =
      update_builder << "$set" << bsoncxx::builder::stream::open_document
                    << "value" << value << bsoncxx::builder::stream::close_document
                    << bsoncxx::builder::stream::finalize;

    try {
      mongocxx::stdx::optional<mongocxx::result::update> result =
          collection.update_one(filter.view(), update.view(), mongocxx::options::update{}.upsert(true));

      if (result) {
        return true;
      }
    } catch (const std::exception& e) {
      std::cerr << "Error: " << e.what() << std::endl;
    }
    // If the operation failed, return false
    return false;
  }


  int Read(int key) {
    // char kCollectionName[10];
    // std::string str = std::to_string(key);
    // str.copy(kCollectionName, str.length());
    // kCollectionName[str.length()] = '\0';
    // mongocxx::collection collection = db[kCollectionName];

    auto filter_builder = bsoncxx::builder::stream::document{};
    
    bsoncxx::document::value filter =
      filter_builder << "key" << key << bsoncxx::builder::stream::finalize;

    try {
      bsoncxx::stdx::optional<bsoncxx::document::value> result =
        collection.find_one(filter.view());

      if (result) {
        bsoncxx::document::view view = result->view();
        auto value_element = view["value"];
        if (value_element && value_element.type() == bsoncxx::type::k_int32) {
          return value_element.get_int32().value;
        }
      }
    } catch (const std::exception& e) {
      std::cerr << "Error: " << e.what() << std::endl;
    }
    // If document does not exist or value is not found, return 0
    return 0;
  }

  void Clear() {
    try {
      db.drop();
        // mongocxx::stdx::optional<mongocxx::result::delete_result> result =
        //     collection.delete_many({});
        // assert(result);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
  }

  // document: {"_ids": 122323, "v": 2323}

  void Setup() {
    auto index_builder = bsoncxx::builder::stream::document{};
    bsoncxx::document::value index =
        index_builder << "key" << 1 << bsoncxx::builder::stream::finalize;
    try {
      bsoncxx::stdx::optional<bsoncxx::document::value> result =
        collection.create_index(index.view());
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    
  }

};

} // end of namespace
