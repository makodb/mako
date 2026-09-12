// Process-isolated executors for the mako-local deterministic script corpus.
//
// Each invocation selects either the direct C++ MassTrans surface or the raw
// C ABI.  The safe Rust executor lives in its own integration test and compares
// its byte-stable transcript with both modes of this program.

#include "lib/common.h"
#include "sto/MassTrans.hh"
#include "sto/StringWrapper.hh"
#include "sto/thread_registration.hh"
#include "storage/mako_local_abi.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef STO_OPACITY
#define STO_OPACITY 0
#endif

namespace {

using Bytes = std::string;
using Rows = std::vector<std::pair<Bytes, Bytes>>;

#if STO_OPACITY
using DirectTable =
    MassTrans<std::string, versioned_str_struct, true /* opacity */>;
#else
using DirectTable =
    MassTrans<std::string, versioned_str_struct, false /* opacity */>;
#endif

class ParseError : public std::runtime_error {
public:
  ParseError(size_t line, const std::string &message)
      : std::runtime_error("line " + std::to_string(line) + ": " + message) {}
};

struct TableSpec {
  std::string slot;
  uint64_t id;
  Bytes name;
  size_t line;
};

enum class OperationKind { get, put, insert, remove, scan, rscan };

struct Operation {
  OperationKind kind;
  size_t table;
  Bytes key;
  Bytes value;
  std::optional<Bytes> upper;
  size_t line;
};

struct TransactionSpec {
  std::vector<Operation> operations;
  bool commit;
  size_t begin_line;
  size_t terminal_line;
};

struct ScriptSpec {
  std::string name;
  std::vector<TableSpec> tables;
  std::vector<TransactionSpec> transactions;
  size_t line;
};

struct InputLine {
  size_t number;
  std::vector<std::string> words;
};

std::vector<std::string> split_words(const std::string &line) {
  std::istringstream input(line);
  std::vector<std::string> words;
  std::string word;
  while (input >> word)
    words.push_back(std::move(word));
  return words;
}

int hex_nibble(char value) {
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  if (value >= 'A' && value <= 'F')
    return value - 'A' + 10;
  return -1;
}

Bytes parse_hex(const std::string &token, size_t line, std::string_view field) {
  if (token == "-")
    return {};
  if (token.empty() || token == "*" || token.size() % 2 != 0) {
    throw ParseError(line,
                     std::string(field) +
                         " must be '-' or an even-length hexadecimal token");
  }
  Bytes result;
  result.reserve(token.size() / 2);
  for (size_t index = 0; index != token.size(); index += 2) {
    const int high = hex_nibble(token[index]);
    const int low = hex_nibble(token[index + 1]);
    if (high < 0 || low < 0) {
      throw ParseError(line,
                       std::string(field) + " contains a non-hexadecimal byte");
    }
    result.push_back(static_cast<char>((high << 4) | low));
  }
  return result;
}

uint64_t parse_u64(const std::string &token, size_t line,
                   std::string_view field) {
  uint64_t value = 0;
  const char *begin = token.data();
  const char *end = begin + token.size();
  const auto parsed = std::from_chars(begin, end, value, 10);
  if (token.empty() || parsed.ec != std::errc() || parsed.ptr != end ||
      std::to_string(value) != token) {
    throw ParseError(line, std::string(field) +
                               " must be a canonical unsigned decimal integer");
  }
  return value;
}

std::vector<InputLine> read_lines(std::istream &input) {
  std::vector<InputLine> lines;
  std::string text;
  size_t number = 0;
  while (std::getline(input, text)) {
    ++number;
    if (!text.empty() && text.back() == '\r') {
      throw ParseError(number, "carriage returns are not canonical");
    }
    if (text.empty())
      throw ParseError(number, "blank lines are not permitted");
    auto words = split_words(text);
    std::string canonical;
    for (const std::string &word : words) {
      if (!canonical.empty())
        canonical.push_back(' ');
      canonical.append(word);
    }
    if (words.empty() || canonical != text)
      throw ParseError(number, "whitespace is not canonical");
    lines.push_back(InputLine{number, std::move(words)});
  }
  if (!input.eof())
    throw std::runtime_error("failed while reading script corpus");
  return lines;
}

void require_word_count(const InputLine &line, size_t expected,
                        std::string_view syntax) {
  if (line.words.size() != expected) {
    throw ParseError(line.number, "expected " + std::string(syntax));
  }
}

std::vector<ScriptSpec> parse_corpus(std::istream &input) {
  const auto lines = read_lines(input);
  if (lines.empty())
    throw ParseError(1, "missing corpus header");
  require_word_count(lines.front(), 1, "mako-local-differential-v1");
  if (lines.front().words[0] != "mako-local-differential-v1") {
    throw ParseError(lines.front().number,
                     "expected mako-local-differential-v1 header");
  }

  std::vector<ScriptSpec> scripts;
  std::unordered_set<std::string> script_names;
  size_t cursor = 1;
  while (cursor != lines.size()) {
    const InputLine &start = lines[cursor++];
    require_word_count(start, 2, "script NAME");
    if (start.words[0] != "script") {
      throw ParseError(start.number, "expected script NAME");
    }
    if (!std::all_of(start.words[1].begin(), start.words[1].end(),
                     [](unsigned char byte) {
                       return (byte >= 'a' && byte <= 'z') ||
                              (byte >= 'A' && byte <= 'Z') ||
                              (byte >= '0' && byte <= '9') || byte == '_' ||
                              byte == '-';
                     })) {
      throw ParseError(start.number, "invalid script name " + start.words[1]);
    }
    if (!script_names.insert(start.words[1]).second) {
      throw ParseError(start.number, "duplicate script name " + start.words[1]);
    }

    ScriptSpec script{start.words[1], {}, {}, start.number};
    std::unordered_map<std::string, size_t> slots;
    std::unordered_set<uint64_t> table_ids;
    std::unordered_set<Bytes> table_names;

    while (cursor != lines.size() && lines[cursor].words[0] == "table") {
      const InputLine &line = lines[cursor++];
      require_word_count(line, 4, "table SLOT ID NAME_HEX");
      const std::string &slot = line.words[1];
      const uint64_t slot_number = parse_u64(slot, line.number, "table slot");
      if (slot_number != script.tables.size()) {
        throw ParseError(line.number,
                         "table slots must be contiguous from zero");
      }
      if (slots.contains(slot)) {
        throw ParseError(line.number, "duplicate table slot " + slot);
      }
      const uint64_t id = parse_u64(line.words[2], line.number, "table ID");
      Bytes name = parse_hex(line.words[3], line.number, "table name");
      if (!table_ids.insert(id).second) {
        throw ParseError(line.number, "duplicate table ID " + line.words[2]);
      }
      if (!table_names.insert(name).second) {
        throw ParseError(line.number, "duplicate binary table name");
      }
      slots.emplace(slot, script.tables.size());
      script.tables.push_back(
          TableSpec{slot, id, std::move(name), line.number});
    }
    if (script.tables.empty()) {
      throw ParseError(start.number, "script must declare at least one table");
    }

    while (cursor != lines.size() && lines[cursor].words[0] != "end") {
      const InputLine &begin = lines[cursor++];
      require_word_count(begin, 1, "begin");
      if (begin.words[0] != "begin") {
        throw ParseError(begin.number,
                         "expected begin or end after table declarations");
      }

      TransactionSpec transaction{{}, false, begin.number, 0};
      bool terminal = false;
      while (cursor != lines.size()) {
        const InputLine &line = lines[cursor++];
        const std::string &verb = line.words[0];
        if (verb == "commit" || verb == "abort") {
          require_word_count(line, 1, "commit or abort");
          transaction.commit = verb == "commit";
          transaction.terminal_line = line.number;
          terminal = true;
          break;
        }

        if (verb == "get" || verb == "remove") {
          require_word_count(line, 3,
                             verb == "get" ? "get SLOT KEYHEX"
                                           : "remove SLOT KEYHEX");
        } else if (verb == "put" || verb == "insert") {
          require_word_count(line, 4,
                             verb == "put" ? "put SLOT KEYHEX VALUEHEX"
                                           : "insert SLOT KEYHEX VALUEHEX");
        } else if (verb == "scan" || verb == "rscan") {
          require_word_count(line, 4,
                             verb == "scan"
                                 ? "scan SLOT LOWERHEX UPPERHEX_OR_*"
                                 : "rscan SLOT LOWERHEX UPPERHEX_OR_*");
        } else {
          throw ParseError(line.number,
                           "unknown transaction operation " + verb);
        }

        const auto slot = slots.find(line.words[1]);
        if (slot == slots.end()) {
          throw ParseError(line.number, "unknown table slot " + line.words[1]);
        }

        Operation operation{OperationKind::get, slot->second, {}, {},
                            std::nullopt,       line.number};
        if (verb == "get") {
          operation.kind = OperationKind::get;
          operation.key = parse_hex(line.words[2], line.number, "key");
        } else if (verb == "put") {
          operation.kind = OperationKind::put;
          operation.key = parse_hex(line.words[2], line.number, "key");
          operation.value = parse_hex(line.words[3], line.number, "value");
        } else if (verb == "insert") {
          operation.kind = OperationKind::insert;
          operation.key = parse_hex(line.words[2], line.number, "key");
          operation.value = parse_hex(line.words[3], line.number, "value");
        } else if (verb == "remove") {
          operation.kind = OperationKind::remove;
          operation.key = parse_hex(line.words[2], line.number, "key");
        } else {
          operation.kind =
              verb == "scan" ? OperationKind::scan : OperationKind::rscan;
          operation.key = parse_hex(line.words[2], line.number, "lower bound");
          if (line.words[3] != "*") {
            operation.upper =
                parse_hex(line.words[3], line.number, "upper bound");
          }
        }
        transaction.operations.push_back(std::move(operation));
      }
      if (!terminal) {
        throw ParseError(begin.number,
                         "transaction is missing commit or abort");
      }
      script.transactions.push_back(std::move(transaction));
    }

    if (script.transactions.empty()) {
      throw ParseError(start.number,
                       "script must contain at least one transaction");
    }
    if (cursor == lines.size()) {
      throw ParseError(start.number, "script is missing end");
    }
    const InputLine &end = lines[cursor++];
    require_word_count(end, 1, "end");
    if (end.words[0] != "end") {
      throw ParseError(end.number, "expected end");
    }
    scripts.push_back(std::move(script));
  }

  if (scripts.empty()) {
    throw ParseError(lines.front().number,
                     "corpus must contain at least one script");
  }
  return scripts;
}

std::string encode_hex(std::string_view bytes) {
  if (bytes.empty())
    return "-";
  static constexpr char digits[] = "0123456789abcdef";
  std::string result(bytes.size() * 2, '0');
  for (size_t index = 0; index != bytes.size(); ++index) {
    const auto byte = static_cast<unsigned char>(bytes[index]);
    result[index * 2] = digits[byte >> 4];
    result[index * 2 + 1] = digits[byte & 0x0f];
  }
  return result;
}

std::string encode_features(uint64_t features) {
  std::ostringstream output;
  output << std::hex << std::nouppercase << std::setfill('0') << std::setw(16)
         << features;
  return output.str();
}

std::string status_description(int status) {
  const char *message = mako_local_status_string(status);
  return std::to_string(status) + " (" +
         (message == nullptr ? std::string("null status message")
                             : std::string(message)) +
         ")";
}

void require_boundary_features(uint64_t features) {
  constexpr uint64_t required = MAKO_LOCAL_FEATURE_POINT_TRANSACTIONS |
                                MAKO_LOCAL_FEATURE_READ_MY_WRITES |
                                MAKO_LOCAL_FEATURE_TRANSACTIONAL_SCANS |
                                MAKO_LOCAL_FEATURE_SCAN_READ_MY_WRITES;
  if ((features & required) != required) {
    throw std::runtime_error(
        "linked engine does not advertise the required point/RYW/scan boundary "
        "profile (features=" +
        encode_features(features) + ")");
  }
}

class Runner {
public:
  virtual ~Runner() = default;
  virtual uint64_t features() const = 0;
  virtual void begin() = 0;
  virtual std::optional<Bytes> get(size_t table, const Bytes &key) = 0;
  virtual bool put(size_t table, const Bytes &key, const Bytes &value) = 0;
  virtual bool insert(size_t table, const Bytes &key, const Bytes &value) = 0;
  virtual bool remove(size_t table, const Bytes &key) = 0;
  virtual Rows scan(size_t table, const Bytes &lower,
                    const std::optional<Bytes> &upper, bool reverse) = 0;
  virtual void commit() = 0;
  virtual void abort() = 0;
};

lcdf::Str direct_key(const Bytes &bytes) {
  return lcdf::Str(bytes.data(), static_cast<int>(bytes.size()));
}

Bytes decode_direct_value(const std::string &encoded) {
  if (encoded.size() < static_cast<size_t>(mako::EXTRA_BITS_FOR_VALUE)) {
    throw std::runtime_error("direct MassTrans returned a malformed value");
  }
  return encoded.substr(0, encoded.size() - mako::EXTRA_BITS_FOR_VALUE);
}

void initialize_direct_runtime() {
  if (!mako::silo::claim_thread_runtime(
          mako::silo::thread_runtime::native_mako)) {
    throw std::runtime_error("direct worker runtime was already claimed");
  }
  const int id = mako::silo::try_allocate_thread_id();
  if (id < 0)
    throw std::runtime_error("direct worker exhausted STO thread IDs");

  TThread::set_id(id);
  Sto::update_threadid();
  TThread::set_mode(0);
  TThread::set_shard_index(0);
  TThread::set_nshards(1);
  TThread::set_warehouses(1);
  TThread::set_pid(0);
  TThread::set_is_micro(0);
  TThread::disable_multiversion();
  TThread::readset_shard_bits = 0;
  TThread::writeset_shard_bits = 0;
  TThread::transget_without_throw = false;
  TThread::transget_without_stable = false;
  TThread::trans_nosend_abort = 0;
  TThread::increment_id = 0;
  TThread::sclient = nullptr;

  if (!mako::silo::ensure_epoch_runtime()) {
    throw std::runtime_error("could not initialize the shared MassTrans epoch");
  }
  DirectTable::thread_init();
}

class DirectRunner final : public Runner {
public:
  explicit DirectRunner(const ScriptSpec &script)
      : features_(mako_local_feature_bits()) {
    require_boundary_features(features_);
    tables_.reserve(script.tables.size());
    for (const TableSpec &spec : script.tables) {
      if (spec.name.size() > MAKO_LOCAL_MAX_TABLE_NAME_BYTES) {
        throw std::runtime_error("direct table name exceeds the ABI limit");
      }
      // MassTrans teardown requires process-wide RCU quiescence. Match native
      // Mako and the C facade by intentionally retaining each table until exit.
      auto *table = new DirectTable();
      table->set_table_id(spec.id);
      table->set_is_remote(false);
      table->set_table_name(spec.name);
      tables_.push_back(table);
    }
  }

  ~DirectRunner() override { abort_if_active(); }

  uint64_t features() const override { return features_; }

  void begin() override {
    if (active_ || Sto::in_progress()) {
      throw std::runtime_error("direct transaction is already active");
    }
    staged_values_.clear();
    item_budget_used_ = 0;
    mutated_keys_.clear();
    Sto::start_transaction();
    active_ = true;
  }

  std::optional<Bytes> get(size_t table, const Bytes &key) override {
    validate_key(key);
    charge(1);
    try {
      std::string value;
      const bool found = table_at(table)->transGet(direct_key(key), value);
      if (TThread::transget_without_throw) {
        TThread::transget_without_throw = false;
        fail_active("unexpected direct get conflict");
      }
      if (!found)
        return std::nullopt;
      return decode_direct_value(value);
    } catch (const std::exception &) {
      abort_if_active();
      throw;
    } catch (...) {
      abort_if_active();
      throw std::runtime_error("unknown exception from direct get");
    }
  }

  bool put(size_t table, const Bytes &key, const Bytes &value) override {
    validate_write(key, value);
    check_legacy_duplicate(table, key);
    charge(write_charge(key.size()));
    try {
      staged_values_.push_back(mako::Encode(value));
      const bool existed = table_at(table)->transPut(
          direct_key(key), StringWrapper(staged_values_.back()));
      record_mutation(table, key);
      return !existed;
    } catch (const std::exception &) {
      abort_if_active();
      throw;
    } catch (...) {
      abort_if_active();
      throw std::runtime_error("unknown exception from direct put");
    }
  }

  bool insert(size_t table, const Bytes &key, const Bytes &value) override {
    validate_write(key, value);
    check_legacy_duplicate(table, key);
    charge(write_charge(key.size()));
    try {
      staged_values_.push_back(mako::Encode(value));
      const bool existed = table_at(table)->transInsert(
          direct_key(key), StringWrapper(staged_values_.back()));
      if (!existed)
        record_mutation(table, key);
      return !existed;
    } catch (const std::exception &) {
      abort_if_active();
      throw;
    } catch (...) {
      abort_if_active();
      throw std::runtime_error("unknown exception from direct insert");
    }
  }

  bool remove(size_t table, const Bytes &key) override {
    validate_key(key);
    check_legacy_duplicate(table, key);
    charge(1);
    try {
      const bool existed = table_at(table)->transDelete(direct_key(key));
      if (existed)
        record_mutation(table, key);
      return existed;
    } catch (const std::exception &) {
      abort_if_active();
      throw;
    } catch (...) {
      abort_if_active();
      throw std::runtime_error("unknown exception from direct remove");
    }
  }

  Rows scan(size_t table, const Bytes &lower, const std::optional<Bytes> &upper,
            bool reverse) override {
    validate_key(lower);
    if (upper)
      validate_key(*upper);
    if (upper && lower >= *upper)
      return {};

    Rows rows;
    size_t remaining = MAKO_LOCAL_TXN_ITEM_BUDGET - item_budget_used_;
    auto collect = [&](lcdf::Str key, std::string &encoded) {
      Bytes copied_key(key.data(), static_cast<size_t>(key.length()));
      rows.emplace_back(std::move(copied_key), decode_direct_value(encoded));
      return true;
    };

    try {
      bool within_budget = false;
      if (!reverse) {
        const Bytes empty_end;
        const Bytes &end = upper ? *upper : empty_end;
        within_budget = table_at(table)->transQueryBounded(
            direct_key(lower), direct_key(end), collect, remaining,
            true /* lower inclusive */, false /* upper exclusive */);
      } else {
        const Bytes maximum(MAKO_LOCAL_MAX_KEY_BYTES, static_cast<char>(0xff));
        const Bytes &begin = upper ? *upper : maximum;
        within_budget = table_at(table)->transRQueryBounded(
            direct_key(begin), direct_key(lower), collect, remaining,
            !upper.has_value() /* finite upper is exclusive */,
            true /* lower inclusive */);
      }
      item_budget_used_ = MAKO_LOCAL_TXN_ITEM_BUDGET - remaining;
      if (!within_budget)
        fail_active("unexpected direct scan item-budget failure");
      if (TThread::transget_without_throw) {
        TThread::transget_without_throw = false;
        fail_active("unexpected direct scan conflict");
      }
      return rows;
    } catch (const std::exception &) {
      abort_if_active();
      throw;
    } catch (...) {
      abort_if_active();
      throw std::runtime_error("unknown exception from direct scan");
    }
  }

  void commit() override {
    require_active();
    try {
      const bool committed = Sto::try_commit_no_paxos();
      active_ = false;
      staged_values_.clear();
      mutated_keys_.clear();
      if (!committed) {
        throw std::runtime_error("unexpected direct transaction conflict");
      }
    } catch (const std::exception &) {
      abort_if_active();
      throw;
    } catch (...) {
      abort_if_active();
      throw std::runtime_error("unknown exception from direct commit");
    }
  }

  void abort() override {
    require_active();
    try {
      Sto::silent_abort();
      active_ = false;
      staged_values_.clear();
      mutated_keys_.clear();
    } catch (...) {
      active_ = false;
      throw std::runtime_error("unknown exception from direct abort");
    }
  }

private:
  DirectTable *table_at(size_t table) {
    if (table >= tables_.size())
      throw std::runtime_error("invalid table index");
    return tables_[table];
  }

  void require_active() const {
    if (!active_)
      throw std::runtime_error("direct transaction is not active");
  }

  void validate_key(const Bytes &key) {
    require_active();
    if (key.size() > MAKO_LOCAL_MAX_KEY_BYTES) {
      fail_active("script key exceeds the ABI limit");
    }
  }

  void validate_write(const Bytes &key, const Bytes &value) {
    validate_key(key);
    if (value.size() > MAKO_LOCAL_MAX_VALUE_BYTES) {
      fail_active("script value exceeds the ABI limit");
    }
  }

  static size_t write_charge(size_t key_size) { return 4 + (key_size + 7) / 8; }

  void charge(size_t amount) {
    require_active();
    if (amount > MAKO_LOCAL_TXN_ITEM_BUDGET - item_budget_used_) {
      fail_active("script transaction exceeds the ABI item budget");
    }
    item_budget_used_ += amount;
  }

  std::string mutation_identity(size_t table, const Bytes &key) const {
    std::string identity(sizeof(table), '\0');
    std::memcpy(identity.data(), &table, sizeof(table));
    identity.append(key);
    return identity;
  }

  void check_legacy_duplicate(size_t table, const Bytes &key) {
    if ((features_ & MAKO_LOCAL_FEATURE_READ_MY_WRITES) == 0 &&
        mutated_keys_.contains(mutation_identity(table, key))) {
      fail_active("script requires repeated mutation in a no-RYW profile");
    }
  }

  void record_mutation(size_t table, const Bytes &key) {
    mutated_keys_.insert(mutation_identity(table, key));
  }

  [[noreturn]] void fail_active(const std::string &message) {
    abort_if_active();
    throw std::runtime_error(message);
  }

  void abort_if_active() noexcept {
    if (!active_)
      return;
    try {
      if (Sto::in_progress())
        Sto::silent_abort();
    } catch (...) {
    }
    active_ = false;
    staged_values_.clear();
    mutated_keys_.clear();
  }

  uint64_t features_;
  std::vector<DirectTable *> tables_;
  bool active_ = false;
  size_t item_budget_used_ = 0;
  std::deque<std::string> staged_values_;
  std::unordered_set<std::string> mutated_keys_;
};

const uint8_t *abi_bytes(const Bytes &bytes) {
  return reinterpret_cast<const uint8_t *>(bytes.data());
}

class AbiRunner final : public Runner {
public:
  explicit AbiRunner(const ScriptSpec &script)
      : features_(mako_local_feature_bits()) {
    require_boundary_features(features_);
    try {
      require_ok(mako_local_thread_attach(), "thread attach");
      require_ok(mako_local_db_open(&db_), "database open");
      tables_.reserve(script.tables.size());
      for (const TableSpec &spec : script.tables) {
        mako_local_table *table = nullptr;
        require_ok(mako_local_table_open(db_, abi_bytes(spec.name),
                                         spec.name.size(), spec.id, &table),
                   "table open");
        if (table == nullptr) {
          throw std::runtime_error("table open returned a null handle");
        }
        tables_.push_back(table);
      }
    } catch (...) {
      if (db_ != nullptr)
        (void)mako_local_db_close(db_);
      db_ = nullptr;
      throw;
    }
  }

  ~AbiRunner() override {
    if (txn_ != nullptr) {
      (void)mako_local_txn_destroy(txn_);
      txn_ = nullptr;
    }
    if (db_ != nullptr) {
      (void)mako_local_db_close(db_);
      db_ = nullptr;
    }
  }

  uint64_t features() const override { return features_; }

  void begin() override {
    if (txn_ != nullptr)
      throw std::runtime_error("ABI transaction is active");
    require_ok(mako_local_txn_begin(db_, &txn_), "transaction begin");
    if (txn_ == nullptr) {
      throw std::runtime_error("transaction begin returned a null handle");
    }
  }

  std::optional<Bytes> get(size_t table, const Bytes &key) override {
    require_active();
    uint8_t *value = nullptr;
    size_t value_size = 0;
    uint8_t found = 0;
    const int status =
        mako_local_txn_get(txn_, table_at(table), abi_bytes(key), key.size(),
                           &value, &value_size, &found);
    std::unique_ptr<uint8_t, decltype(&mako_local_bytes_free)> owned(
        value, &mako_local_bytes_free);
    operation_ok(status, "get");
    if (found > 1 || (found == 0 && (value != nullptr || value_size != 0)) ||
        (found == 1 && value == nullptr)) {
      fail_transaction("get returned malformed outputs");
    }
    if (found == 0)
      return std::nullopt;
    return Bytes(reinterpret_cast<const char *>(value), value_size);
  }

  bool put(size_t table, const Bytes &key, const Bytes &value) override {
    uint8_t created = 0;
    const int status = mako_local_txn_put(
        active_txn(), table_at(table), abi_bytes(key), key.size(),
        abi_bytes(value), value.size(), &created);
    operation_ok(status, "put");
    if (created > 1)
      fail_transaction("put returned a non-boolean result");
    return created != 0;
  }

  bool insert(size_t table, const Bytes &key, const Bytes &value) override {
    uint8_t inserted = 0;
    const int status = mako_local_txn_insert(
        active_txn(), table_at(table), abi_bytes(key), key.size(),
        abi_bytes(value), value.size(), &inserted);
    operation_ok(status, "insert");
    if (inserted > 1)
      fail_transaction("insert returned a non-boolean result");
    return inserted != 0;
  }

  bool remove(size_t table, const Bytes &key) override {
    uint8_t existed = 0;
    const int status = mako_local_txn_remove(
        active_txn(), table_at(table), abi_bytes(key), key.size(), &existed);
    operation_ok(status, "remove");
    if (existed > 1)
      fail_transaction("remove returned a non-boolean result");
    return existed != 0;
  }

  Rows scan(size_t table, const Bytes &lower, const std::optional<Bytes> &upper,
            bool reverse) override {
    require_active();
    constexpr size_t entry_capacity = 64;
    std::vector<mako_local_scan_entry> entries(entry_capacity);
    std::vector<uint8_t> arena(4096);
    std::optional<Bytes> resume;
    Rows rows;

    for (;;) {
      mako_local_scan_options options{};
      options.struct_size = MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE;
      options.lower = abi_bytes(lower);
      options.lower_len = lower.size();
      if (upper) {
        options.flags |= MAKO_LOCAL_SCAN_HAS_UPPER;
        options.upper = abi_bytes(*upper);
        options.upper_len = upper->size();
      }
      if (resume) {
        options.flags |= MAKO_LOCAL_SCAN_HAS_RESUME;
        options.resume = abi_bytes(*resume);
        options.resume_len = resume->size();
      }

      size_t entry_count = 0;
      size_t arena_used = 0;
      size_t arena_required = 0;
      uint8_t done = 0;
      const int status =
          reverse ? mako_local_txn_rscan_chunk(
                        txn_, table_at(table), &options, entries.data(),
                        entries.size(), arena.data(), arena.size(),
                        &entry_count, &arena_used, &arena_required, &done)
                  : mako_local_txn_scan_chunk(
                        txn_, table_at(table), &options, entries.data(),
                        entries.size(), arena.data(), arena.size(),
                        &entry_count, &arena_used, &arena_required, &done);

      if (status == MAKO_LOCAL_BUFFER_TOO_SMALL) {
        const size_t maximum_entry =
            MAKO_LOCAL_MAX_KEY_BYTES + MAKO_LOCAL_MAX_VALUE_BYTES;
        if (arena_required <= arena.size() || arena_required > maximum_entry) {
          fail_transaction("scan returned an invalid arena requirement");
        }
        arena.resize(arena_required);
        continue;
      }
      operation_ok(status, reverse ? "rscan" : "scan");
      if (entry_count > entries.size() || arena_used > arena.size() ||
          done > 1 || arena_required != 0) {
        fail_transaction("scan returned malformed scalar outputs");
      }

      for (size_t index = 0; index != entry_count; ++index) {
        const mako_local_scan_entry &entry = entries[index];
        const size_t key_offset = entry.key_offset;
        const size_t key_size = entry.key_length;
        const size_t value_offset = entry.value_offset;
        const size_t value_size = entry.value_length;
        if (key_offset > arena_used || key_size > arena_used - key_offset ||
            value_offset > arena_used ||
            value_size > arena_used - value_offset) {
          fail_transaction("scan returned an out-of-bounds entry");
        }
        Bytes key(reinterpret_cast<const char *>(arena.data() + key_offset),
                  key_size);
        Bytes value(reinterpret_cast<const char *>(arena.data() + value_offset),
                    value_size);
        if (key < lower || (upper && key >= *upper)) {
          fail_transaction("scan returned a key outside its requested range");
        }
        if (!rows.empty()) {
          const bool ordered =
              reverse ? rows.back().first > key : rows.back().first < key;
          if (!ordered)
            fail_transaction("scan keys are not strictly ordered");
        }
        rows.emplace_back(std::move(key), std::move(value));
      }

      if (done != 0)
        break;
      if (entry_count == 0) {
        fail_transaction("scan made no progress before an incomplete result");
      }
      resume = rows.back().first;
    }
    return rows;
  }

  void commit() override {
    require_active();
    mako_local_txn *txn = txn_;
    const int commit_status = mako_local_txn_commit(txn);
    const int destroy_status = mako_local_txn_destroy(txn);
    txn_ = nullptr;
    if (commit_status != MAKO_LOCAL_OK) {
      throw std::runtime_error("unexpected ABI commit result " +
                               status_description(commit_status));
    }
    require_ok(destroy_status, "committed transaction destroy");
  }

  void abort() override {
    require_active();
    mako_local_txn *txn = txn_;
    const int abort_status = mako_local_txn_abort(txn);
    const int destroy_status = mako_local_txn_destroy(txn);
    txn_ = nullptr;
    require_ok(abort_status, "transaction abort");
    require_ok(destroy_status, "aborted transaction destroy");
  }

private:
  static void require_ok(int status, std::string_view operation) {
    if (status != MAKO_LOCAL_OK) {
      throw std::runtime_error(std::string(operation) + " failed with " +
                               status_description(status));
    }
  }

  void require_active() const {
    if (txn_ == nullptr)
      throw std::runtime_error("ABI transaction is not active");
  }

  mako_local_txn *active_txn() {
    require_active();
    return txn_;
  }

  mako_local_table *table_at(size_t table) {
    if (table >= tables_.size())
      throw std::runtime_error("invalid table index");
    return tables_[table];
  }

  void operation_ok(int status, std::string_view operation) {
    if (status == MAKO_LOCAL_OK)
      return;
    const int cleanup =
        txn_ == nullptr ? MAKO_LOCAL_OK : mako_local_txn_destroy(txn_);
    txn_ = nullptr;
    std::string message =
        std::string(operation) + " failed with " + status_description(status);
    if (cleanup != MAKO_LOCAL_OK) {
      message += "; destroy also failed with " + status_description(cleanup);
    }
    throw std::runtime_error(message);
  }

  [[noreturn]] void fail_transaction(const std::string &message) {
    const int cleanup =
        txn_ == nullptr ? MAKO_LOCAL_OK : mako_local_txn_destroy(txn_);
    txn_ = nullptr;
    if (cleanup != MAKO_LOCAL_OK) {
      throw std::runtime_error(message + "; destroy also failed with " +
                               status_description(cleanup));
    }
    throw std::runtime_error(message);
  }

  uint64_t features_;
  mako_local_db *db_ = nullptr;
  std::vector<mako_local_table *> tables_;
  mako_local_txn *txn_ = nullptr;
};

void emit_rows(std::ostream &output, const Rows &rows) {
  output << " rows " << rows.size();
  for (const auto &[key, value] : rows) {
    output << ' ' << encode_hex(key) << ' ' << encode_hex(value);
  }
  output << '\n';
}

Rows collect_final_state(Runner &runner, size_t table) {
  runner.begin();
  Rows rows = runner.scan(table, Bytes{}, std::nullopt, false);
  runner.commit();
  std::sort(rows.begin(), rows.end(), [](const auto &left, const auto &right) {
    return left.first < right.first;
  });
  return rows;
}

void replay_script(const ScriptSpec &script, Runner &runner,
                   std::ostream &output,
                   const std::optional<size_t> injected_operation,
                   bool &injection_consumed) {
  output << "script " << script.name << '\n';
  output << "features " << encode_features(runner.features()) << '\n';

  size_t global_op_index = 0;
  for (size_t txn_index = 0; txn_index != script.transactions.size();
       ++txn_index) {
    const TransactionSpec &transaction = script.transactions[txn_index];
    try {
      runner.begin();
      output << "txn " << txn_index << " begin ok\n";
      for (const Operation &operation : transaction.operations) {
        const size_t operation_index = global_op_index++;
        const bool inject =
            !injection_consumed && injected_operation == operation_index;
        output << "op " << operation_index << ' ';
        switch (operation.kind) {
        case OperationKind::get: {
          const auto value = runner.get(operation.table, operation.key);
          output << "get ";
          if (inject) {
            // Test-only fault seam: make one observation deliberately wrong so
            // the orchestrating differential test proves that a real child
            // process divergence turns the gate red.
            injection_consumed = true;
            if (value) {
              output << "absent\n";
            } else {
              output << "value 00\n";
            }
          } else if (value) {
            output << "value " << encode_hex(*value) << '\n';
          } else {
            output << "absent\n";
          }
          break;
        }
        case OperationKind::put: {
          const bool created =
              runner.put(operation.table, operation.key, operation.value);
          output << "put created " << (created ? 1 : 0) << '\n';
          break;
        }
        case OperationKind::insert: {
          const bool inserted =
              runner.insert(operation.table, operation.key, operation.value);
          output << "insert inserted " << (inserted ? 1 : 0) << '\n';
          break;
        }
        case OperationKind::remove: {
          const bool removed = runner.remove(operation.table, operation.key);
          output << "remove existed " << (removed ? 1 : 0) << '\n';
          break;
        }
        case OperationKind::scan:
        case OperationKind::rscan: {
          const bool reverse = operation.kind == OperationKind::rscan;
          const Rows rows = runner.scan(operation.table, operation.key,
                                        operation.upper, reverse);
          output << (reverse ? "rscan" : "scan");
          emit_rows(output, rows);
          break;
        }
        }
      }

      if (transaction.commit) {
        runner.commit();
        output << "txn " << txn_index << " commit ok\n";
      } else {
        runner.abort();
        output << "txn " << txn_index << " abort ok\n";
      }
    } catch (const std::exception &error) {
      throw std::runtime_error("script " + script.name + ", transaction " +
                               std::to_string(txn_index) + ": " + error.what());
    }
  }

  for (size_t table = 0; table != script.tables.size(); ++table) {
    Rows rows;
    try {
      rows = collect_final_state(runner, table);
    } catch (const std::exception &error) {
      throw std::runtime_error("script " + script.name + ", final table " +
                               script.tables[table].slot + ": " + error.what());
    }
    output << "final " << script.tables[table].slot;
    emit_rows(output, rows);
  }
  output << "end " << script.name << '\n';
}

std::optional<size_t> parse_injected_operation() {
  const char *raw = std::getenv("MAKO_LOCAL_DIFF_INJECT_OP");
  if (raw == nullptr)
    return std::nullopt;

  const std::string_view text(raw);
  size_t operation = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), operation);
  if (text.empty() || error != std::errc{} ||
      end != text.data() + text.size()) {
    throw std::runtime_error(
        "MAKO_LOCAL_DIFF_INJECT_OP must be an unsigned decimal operation "
        "index");
  }
  return operation;
}

enum class Mode { direct, abi };

Mode parse_mode(int argc, char **argv) {
  if (argc != 2) {
    throw std::runtime_error(
        "usage: mako_local_differential_driver direct|abi");
  }
  const std::string_view mode(argv[1]);
  if (mode == "direct")
    return Mode::direct;
  if (mode == "abi")
    return Mode::abi;
  throw std::runtime_error("mode must be exactly 'direct' or 'abi'");
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Mode mode = parse_mode(argc, argv);
    const std::optional<size_t> injected_operation = parse_injected_operation();
    bool injection_consumed = false;
    const std::vector<ScriptSpec> scripts = parse_corpus(std::cin);
    if (mode == Mode::direct)
      initialize_direct_runtime();

    std::cout << "mako-local-transcript-v1\n";
    for (const ScriptSpec &script : scripts) {
      std::unique_ptr<Runner> runner =
          mode == Mode::direct
              ? std::unique_ptr<Runner>(new DirectRunner(script))
              : std::unique_ptr<Runner>(new AbiRunner(script));
      replay_script(script, *runner, std::cout, injected_operation,
                    injection_consumed);
    }
    if (injected_operation && !injection_consumed) {
      throw std::runtime_error(
          "MAKO_LOCAL_DIFF_INJECT_OP did not select a get operation");
    }
    std::cout.flush();
    if (!std::cout)
      throw std::runtime_error("failed to write transcript");
    return EXIT_SUCCESS;
  } catch (const ParseError &error) {
    std::cerr << "mako-local differential corpus error: " << error.what()
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << "mako-local differential driver error: " << error.what()
              << '\n';
  } catch (...) {
    std::cerr << "mako-local differential driver error: unknown exception\n";
  }
  return EXIT_FAILURE;
}
