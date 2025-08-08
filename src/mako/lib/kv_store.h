#ifndef _KV_STORE_H_
#define _KV_STORE_H_

#include <string>
#include <map>
#include <utility>
#include <vector>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <regex>

class KVStore {
public:
    KVStore();
    ~KVStore();
    
    struct Result {
        std::string value;
        bool success;
        
        Result(const std::string& val, bool succ) : value(val), success(succ) {}
        Result(bool succ) : value(""), success(succ) {}
    };
    
    Result get(const std::string& key) const;
    Result set(const std::string& key, const std::string& value);
    Result execute_operation(const std::string& operation, const std::string& key, const std::string& value);
    
    // Numeric operations
    Result incr(const std::string& key);
    Result decr(const std::string& key);
    Result incrby(const std::string& key, int increment);
    Result decrby(const std::string& key, int decrement);
    
    // List operations
    Result lpush(const std::string& key, const std::string& value);
    Result rpush(const std::string& key, const std::string& value);
    Result lpop(const std::string& key);
    Result rpop(const std::string& key);
    Result llen(const std::string& key);
    Result lrange(const std::string& key, int start, int stop);
    
    // Hash operations
    Result hset(const std::string& key, const std::string& field, const std::string& value);
    Result hget(const std::string& key, const std::string& field);
    Result hgetall(const std::string& key);
    Result hmget(const std::string& key, const std::string& fields);
    Result hdel(const std::string& key, const std::string& field);
    Result hexists(const std::string& key, const std::string& field);
    
    // Set operations
    Result sadd(const std::string& key, const std::string& members);
    Result smembers(const std::string& key);
    Result sismember(const std::string& key, const std::string& member);
    Result sinter(const std::string& key1, const std::string& key2);
    Result sdiff(const std::string& key1, const std::string& key2);
    Result scard(const std::string& key);
    
    // Key management operations
    Result exists(const std::string& key) const;
    Result expire(const std::string& key, int seconds);
    Result ttl(const std::string& key) const;
    Result keys(const std::string& pattern) const;
    Result del(const std::string& key);
    
    size_t size() const;
    bool empty() const;
    void clear();
    
private:
    std::map<std::string, std::string> store_;
    std::map<std::string, std::list<std::string>> lists_;
    std::map<std::string, std::unordered_map<std::string, std::string>> hashes_;
    std::map<std::string, std::unordered_set<std::string>> sets_;
    std::map<std::string, std::chrono::steady_clock::time_point> expiry_times_;
    
    // Helper method to check if a key has expired
    bool is_expired(const std::string& key) const;
};

#endif