#pragma once

#include <string>

namespace mako {

/**
 * Status - RocksDB-like status class for error handling
 *
 * Provides a clean, consistent way to report success/failure from Mako
 * operations, similar to rocksdb::Status.
 */
class Status {
public:
    // Factory methods for creating status objects
    static Status OK() { return Status(); }

    static Status NotFound(const std::string& msg = "") {
        return Status(kNotFound, msg);
    }

    static Status InvalidArgument(const std::string& msg = "") {
        return Status(kInvalidArgument, msg);
    }

    static Status IOError(const std::string& msg = "") {
        return Status(kIOError, msg);
    }

    static Status NotSupported(const std::string& msg = "") {
        return Status(kNotSupported, msg);
    }

    static Status Corruption(const std::string& msg = "") {
        return Status(kCorruption, msg);
    }

    static Status Busy(const std::string& msg = "") {
        return Status(kBusy, msg);
    }

    static Status TimedOut(const std::string& msg = "") {
        return Status(kTimedOut, msg);
    }

    // Default copy/move constructors and assignment operators
    Status(const Status&) = default;
    Status& operator=(const Status&) = default;
    Status(Status&&) = default;
    Status& operator=(Status&&) = default;

    // Status checks
    bool ok() const { return code_ == kOk; }
    bool IsNotFound() const { return code_ == kNotFound; }
    bool IsInvalidArgument() const { return code_ == kInvalidArgument; }
    bool IsIOError() const { return code_ == kIOError; }
    bool IsNotSupported() const { return code_ == kNotSupported; }
    bool IsCorruption() const { return code_ == kCorruption; }
    bool IsBusy() const { return code_ == kBusy; }
    bool IsTimedOut() const { return code_ == kTimedOut; }

    // Get string representation of the status
    std::string ToString() const {
        if (code_ == kOk) {
            return "OK";
        }

        std::string result;
        switch (code_) {
            case kNotFound:
                result = "NotFound";
                break;
            case kInvalidArgument:
                result = "Invalid argument";
                break;
            case kIOError:
                result = "IO error";
                break;
            case kNotSupported:
                result = "Not supported";
                break;
            case kCorruption:
                result = "Corruption";
                break;
            case kBusy:
                result = "Busy";
                break;
            case kTimedOut:
                result = "Timed out";
                break;
            default:
                result = "Unknown error";
                break;
        }

        if (!msg_.empty()) {
            result += ": ";
            result += msg_;
        }

        return result;
    }

private:
    enum Code {
        kOk = 0,
        kNotFound,
        kInvalidArgument,
        kIOError,
        kNotSupported,
        kCorruption,
        kBusy,
        kTimedOut
    };

    // Default constructor creates OK status
    Status() : code_(kOk) {}

    Status(Code code, const std::string& msg)
        : code_(code), msg_(msg) {}

    Code code_;
    std::string msg_;
};

}  // namespace mako
