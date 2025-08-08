#include "kv_store.h"
#include <sstream>
#include <stdexcept>

KVStore::KVStore() {
}

KVStore::~KVStore() {
}

KVStore::Result KVStore::get(const std::string& key) const {
    auto it = store_.find(key);
    if (it != store_.end()) {
        return Result(it->second, true);
    } else {
        return Result(false);
    }
}

KVStore::Result KVStore::set(const std::string& key, const std::string& value) {
    store_[key] = value;
    return Result("OK", true);
}

KVStore::Result KVStore::execute_operation(const std::string& operation, const std::string& key, const std::string& value) {
    if (operation == "get") {
        return get(key);
    } else if (operation == "set") {
        return set(key, value);
    } else if (operation == "incr") {
        return incr(key);
    } else if (operation == "decr") {
        return decr(key);
    } else if (operation == "incrby") {
        try {
            int increment = std::stoi(value);
            return incrby(key, increment);
        } catch (const std::exception&) {
            return Result("ERROR: Invalid increment value", false);
        }
    } else if (operation == "decrby") {
        try {
            int decrement = std::stoi(value);
            return decrby(key, decrement);
        } catch (const std::exception&) {
            return Result("ERROR: Invalid decrement value", false);
        }
    } else if (operation == "lpush") {
        // Handle multiple values separated by comma
        std::istringstream iss(value);
        std::string single_value;
        int count = 0;
        while (std::getline(iss, single_value, ',')) {
            lpush(key, single_value);
            count++;
        }
        return Result(std::to_string(lists_[key].size()), true);
    } else if (operation == "rpush") {
        // Handle multiple values separated by comma
        std::istringstream iss(value);
        std::string single_value;
        int count = 0;
        while (std::getline(iss, single_value, ',')) {
            rpush(key, single_value);
            count++;
        }
        return Result(std::to_string(lists_[key].size()), true);
    } else if (operation == "lpop") {
        return lpop(key);
    } else if (operation == "rpop") {
        return rpop(key);
    } else if (operation == "llen") {
        return llen(key);
    } else if (operation == "lrange") {
        // Parse start,stop from value
        size_t comma_pos = value.find(',');
        if (comma_pos == std::string::npos) {
            return Result("ERROR: Invalid range format", false);
        }
        try {
            int start = std::stoi(value.substr(0, comma_pos));
            int stop = std::stoi(value.substr(comma_pos + 1));
            return lrange(key, start, stop);
        } catch (const std::exception&) {
            return Result("ERROR: Invalid range values", false);
        }
    } else if (operation == "hset") {
        // Parse field:value from value
        size_t colon_pos = value.find(':');
        if (colon_pos == std::string::npos) {
            return Result("ERROR: Invalid hset format", false);
        }
        std::string field = value.substr(0, colon_pos);
        std::string val = value.substr(colon_pos + 1);
        return hset(key, field, val);
    } else if (operation == "hget") {
        return hget(key, value); // value is field
    } else if (operation == "hgetall") {
        return hgetall(key);
    } else if (operation == "hmget") {
        return hmget(key, value); // value contains comma-separated fields
    } else if (operation == "hdel") {
        return hdel(key, value); // value is field
    } else if (operation == "hexists") {
        return hexists(key, value); // value is field
    } else if (operation == "ping") {
        return Result("PONG", true);
    } else if (operation == "del") {
        return del(key);
    } else if (operation == "exists") {
        return exists(key);
    } else if (operation == "expire") {
        try {
            int seconds = std::stoi(value);
            return expire(key, seconds);
        } catch (const std::exception&) {
            return Result("ERROR: Invalid expire value", false);
        }
    } else if (operation == "ttl") {
        return ttl(key);
    } else if (operation == "keys") {
        return keys(key); // key parameter is the pattern
    } else if (operation == "sadd") {
        return sadd(key, value);
    } else if (operation == "smembers") {
        return smembers(key);
    } else if (operation == "sismember") {
        return sismember(key, value);
    } else if (operation == "sinter") {
        return sinter(key, value); // value is the second key
    } else if (operation == "sdiff") {
        return sdiff(key, value); // value is the second key
    } else if (operation == "scard") {
        return scard(key);
    } else if (operation == "multi") {
        return Result("OK", true); // Just acknowledge, no state change needed
    } else if (operation == "exec") {
        return Result("OK", true); // Just acknowledge, commands executed sequentially
    } else if (operation == "discard") {
        return Result("OK", true); // Just acknowledge, nothing to discard
    } else if (operation == "watch") {
        return Result("OK", true); // Just acknowledge, no actual watching
    } else if (operation == "unwatch") {
        return Result("OK", true); // Just acknowledge, no actual unwatching
    } else {
        return Result("ERROR: Invalid operation", false);
    }
}

size_t KVStore::size() const {
    return store_.size();
}

bool KVStore::empty() const {
    return store_.empty();
}

void KVStore::clear() {
    store_.clear();
    lists_.clear();
    hashes_.clear();
    sets_.clear();
}

// Numeric operations
KVStore::Result KVStore::incr(const std::string& key) {
    return incrby(key, 1);
}

KVStore::Result KVStore::decr(const std::string& key) {
    return decrby(key, 1);
}

KVStore::Result KVStore::incrby(const std::string& key, int increment) {
    auto it = store_.find(key);
    int current_value = 0;
    
    if (it != store_.end()) {
        try {
            current_value = std::stoi(it->second);
        } catch (const std::exception&) {
            return Result("ERROR: value is not an integer", false);
        }
    }
    
    int new_value = current_value + increment;
    store_[key] = std::to_string(new_value);
    return Result(std::to_string(new_value), true);
}

KVStore::Result KVStore::decrby(const std::string& key, int decrement) {
    return incrby(key, -decrement);
}

// List operations
KVStore::Result KVStore::lpush(const std::string& key, const std::string& value) {
    lists_[key].push_front(value);
    return Result(std::to_string(lists_[key].size()), true);
}

KVStore::Result KVStore::rpush(const std::string& key, const std::string& value) {
    lists_[key].push_back(value);
    return Result(std::to_string(lists_[key].size()), true);
}

KVStore::Result KVStore::lpop(const std::string& key) {
    auto it = lists_.find(key);
    if (it == lists_.end() || it->second.empty()) {
        return Result(false);
    }
    
    std::string value = it->second.front();
    it->second.pop_front();
    
    if (it->second.empty()) {
        lists_.erase(it);
    }
    
    return Result(value, true);
}

KVStore::Result KVStore::rpop(const std::string& key) {
    auto it = lists_.find(key);
    if (it == lists_.end() || it->second.empty()) {
        return Result(false);
    }
    
    std::string value = it->second.back();
    it->second.pop_back();
    
    if (it->second.empty()) {
        lists_.erase(it);
    }
    
    return Result(value, true);
}

KVStore::Result KVStore::llen(const std::string& key) {
    auto it = lists_.find(key);
    if (it == lists_.end()) {
        return Result("0", true);
    }
    return Result(std::to_string(it->second.size()), true);
}

KVStore::Result KVStore::lrange(const std::string& key, int start, int stop) {
    auto it = lists_.find(key);
    if (it == lists_.end()) {
        return Result("", true);
    }
    
    const auto& list = it->second;
    int size = static_cast<int>(list.size());
    
    // Handle negative indices
    if (start < 0) start += size;
    if (stop < 0) stop += size;
    
    // Clamp to valid range
    start = std::max(0, std::min(start, size - 1));
    stop = std::max(0, std::min(stop, size - 1));
    
    if (start > stop) {
        return Result("", true);
    }
    
    std::ostringstream result;
    auto list_it = list.begin();
    std::advance(list_it, start);
    
    for (int i = start; i <= stop && list_it != list.end(); ++i, ++list_it) {
        if (i > start) result << ",";
        result << *list_it;
    }
    
    return Result(result.str(), true);
}

// Hash operations
KVStore::Result KVStore::hset(const std::string& key, const std::string& field, const std::string& value) {
    bool is_new = hashes_[key].find(field) == hashes_[key].end();
    hashes_[key][field] = value;
    return Result(is_new ? "1" : "0", true);
}

KVStore::Result KVStore::hget(const std::string& key, const std::string& field) {
    auto hash_it = hashes_.find(key);
    if (hash_it == hashes_.end()) {
        return Result(false);
    }
    
    auto field_it = hash_it->second.find(field);
    if (field_it == hash_it->second.end()) {
        return Result(false);
    }
    
    return Result(field_it->second, true);
}

KVStore::Result KVStore::hgetall(const std::string& key) {
    auto hash_it = hashes_.find(key);
    if (hash_it == hashes_.end()) {
        return Result("", true);
    }
    
    std::ostringstream result;
    bool first = true;
    for (const auto& pair : hash_it->second) {
        if (!first) result << ",";
        result << pair.first << ":" << pair.second;
        first = false;
    }
    
    return Result(result.str(), true);
}

KVStore::Result KVStore::hmget(const std::string& key, const std::string& fields) {
    auto hash_it = hashes_.find(key);
    if (hash_it == hashes_.end()) {
        // Hash doesn't exist, return NULL for all fields
        std::istringstream iss(fields);
        std::string field;
        std::ostringstream result;
        bool first = true;
        while (std::getline(iss, field, ',')) {
            if (!first) result << ",";
            result << "NULL";
            first = false;
        }
        return Result(result.str(), true);
    }
    
    std::istringstream iss(fields);
    std::string field;
    std::ostringstream result;
    bool first = true;
    
    while (std::getline(iss, field, ',')) {
        if (!first) result << ",";
        
        auto field_it = hash_it->second.find(field);
        if (field_it != hash_it->second.end()) {
            result << field_it->second;
        } else {
            result << "NULL";
        }
        first = false;
    }
    
    return Result(result.str(), true);
}

KVStore::Result KVStore::hdel(const std::string& key, const std::string& field) {
    auto hash_it = hashes_.find(key);
    if (hash_it == hashes_.end()) {
        return Result("0", true);
    }
    
    int removed = hash_it->second.erase(field);
    if (hash_it->second.empty()) {
        hashes_.erase(hash_it);
    }
    
    return Result(std::to_string(removed), true);
}

KVStore::Result KVStore::hexists(const std::string& key, const std::string& field) {
    auto hash_it = hashes_.find(key);
    if (hash_it == hashes_.end()) {
        return Result("0", true);
    }
    
    bool exists = hash_it->second.find(field) != hash_it->second.end();
    return Result(exists ? "1" : "0", true);
}

// Key management operations
bool KVStore::is_expired(const std::string& key) const {
    auto it = expiry_times_.find(key);
    if (it == expiry_times_.end()) {
        return false; // No expiry set
    }
    return std::chrono::steady_clock::now() >= it->second;
}

KVStore::Result KVStore::exists(const std::string& key) const {
    if (is_expired(key)) {
        return Result("0", true);
    }
    
    int count = 0;
    if (store_.find(key) != store_.end()) count++;
    if (lists_.find(key) != lists_.end()) count++;
    if (hashes_.find(key) != hashes_.end()) count++;
    if (sets_.find(key) != sets_.end()) count++;
    
    return Result(std::to_string(count), true);
}

KVStore::Result KVStore::expire(const std::string& key, int seconds) {
    // Check if key exists in any store
    bool key_exists = (store_.find(key) != store_.end()) ||
                      (lists_.find(key) != lists_.end()) ||
                      (hashes_.find(key) != hashes_.end()) ||
                      (sets_.find(key) != sets_.end());
    
    if (!key_exists) {
        return Result("0", true); // Key doesn't exist
    }
    
    auto expiry_time = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    expiry_times_[key] = expiry_time;
    return Result("1", true); // Expiry set
}

KVStore::Result KVStore::ttl(const std::string& key) const {
    // Check if key exists
    bool key_exists = (store_.find(key) != store_.end()) ||
                      (lists_.find(key) != lists_.end()) ||
                      (hashes_.find(key) != hashes_.end()) ||
                      (sets_.find(key) != sets_.end());
    
    if (!key_exists) {
        return Result("-2", true); // Key doesn't exist
    }
    
    auto it = expiry_times_.find(key);
    if (it == expiry_times_.end()) {
        return Result("-1", true); // No expiry set
    }
    
    auto now = std::chrono::steady_clock::now();
    if (now >= it->second) {
        return Result("-2", true); // Key expired
    }
    
    auto remaining = std::chrono::duration_cast<std::chrono::seconds>(it->second - now);
    return Result(std::to_string(remaining.count()), true);
}

KVStore::Result KVStore::keys(const std::string& pattern) const {
    std::vector<std::string> matching_keys;
    
    // Convert Redis pattern to regex
    std::string regex_pattern = pattern;
    // Replace * with .*
    size_t pos = 0;
    while ((pos = regex_pattern.find('*', pos)) != std::string::npos) {
        regex_pattern.replace(pos, 1, ".*");
        pos += 2;
    }
    // Replace ? with .
    pos = 0;
    while ((pos = regex_pattern.find('?', pos)) != std::string::npos) {
        regex_pattern.replace(pos, 1, ".");
        pos += 1;
    }
    
    std::regex pattern_regex(regex_pattern);
    
    // Check all stores
    for (const auto& pair : store_) {
        if (!is_expired(pair.first) && std::regex_match(pair.first, pattern_regex)) {
            matching_keys.push_back(pair.first);
        }
    }
    for (const auto& pair : lists_) {
        if (!is_expired(pair.first) && std::regex_match(pair.first, pattern_regex)) {
            matching_keys.push_back(pair.first);
        }
    }
    for (const auto& pair : hashes_) {
        if (!is_expired(pair.first) && std::regex_match(pair.first, pattern_regex)) {
            matching_keys.push_back(pair.first);
        }
    }
    for (const auto& pair : sets_) {
        if (!is_expired(pair.first) && std::regex_match(pair.first, pattern_regex)) {
            matching_keys.push_back(pair.first);
        }
    }
    
    // Join keys with comma
    std::ostringstream result;
    for (size_t i = 0; i < matching_keys.size(); ++i) {
        if (i > 0) result << ",";
        result << matching_keys[i];
    }
    
    return Result(result.str(), true);
}

KVStore::Result KVStore::del(const std::string& key) {
    int deleted = 0;
    if (store_.erase(key)) deleted++;
    if (lists_.erase(key)) deleted++;
    if (hashes_.erase(key)) deleted++;
    if (sets_.erase(key)) deleted++;
    expiry_times_.erase(key); // Also remove expiry
    return Result(std::to_string(deleted), true);
}

// Set operations
KVStore::Result KVStore::sadd(const std::string& key, const std::string& members) {
    std::istringstream iss(members);
    std::string member;
    int added = 0;
    
    while (std::getline(iss, member, ',')) {
        if (sets_[key].insert(member).second) {
            added++;
        }
    }
    
    return Result(std::to_string(added), true);
}

KVStore::Result KVStore::smembers(const std::string& key) {
    auto it = sets_.find(key);
    if (it == sets_.end()) {
        return Result("", true); // Empty set
    }
    
    std::ostringstream result;
    bool first = true;
    for (const auto& member : it->second) {
        if (!first) result << ",";
        result << member;
        first = false;
    }
    
    return Result(result.str(), true);
}

KVStore::Result KVStore::sismember(const std::string& key, const std::string& member) {
    auto it = sets_.find(key);
    if (it == sets_.end()) {
        return Result("0", true);
    }
    
    bool is_member = it->second.find(member) != it->second.end();
    return Result(is_member ? "1" : "0", true);
}

KVStore::Result KVStore::sinter(const std::string& key1, const std::string& key2) {
    auto it1 = sets_.find(key1);
    auto it2 = sets_.find(key2);
    
    if (it1 == sets_.end() || it2 == sets_.end()) {
        return Result("", true); // Empty intersection
    }
    
    std::ostringstream result;
    bool first = true;
    
    for (const auto& member : it1->second) {
        if (it2->second.find(member) != it2->second.end()) {
            if (!first) result << ",";
            result << member;
            first = false;
        }
    }
    
    return Result(result.str(), true);
}

KVStore::Result KVStore::sdiff(const std::string& key1, const std::string& key2) {
    auto it1 = sets_.find(key1);
    auto it2 = sets_.find(key2);
    
    if (it1 == sets_.end()) {
        return Result("", true); // Empty diff
    }
    
    std::ostringstream result;
    bool first = true;
    
    for (const auto& member : it1->second) {
        if (it2 == sets_.end() || it2->second.find(member) == it2->second.end()) {
            if (!first) result << ",";
            result << member;
            first = false;
        }
    }
    
    return Result(result.str(), true);
}

KVStore::Result KVStore::scard(const std::string& key) {
    auto it = sets_.find(key);
    if (it == sets_.end()) {
        return Result("0", true);
    }
    
    return Result(std::to_string(it->second.size()), true);
}