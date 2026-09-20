#ifndef MAKO_BENCHMARKS_BENCHMARK_OUTPUT_H
#define MAKO_BENCHMARKS_BENCHMARK_OUTPUT_H

#include <cerrno>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <ostream>
#include <utility>
#include <unistd.h>

namespace mako {

// C++ stream formatting state is mutable and is not safe to share between the
// benchmark threads. Keep both human-readable streams and raw machine-record
// writes behind the same process-wide lock.
inline std::mutex &benchmark_output_mutex()
{
  static std::mutex output_mutex;
  return output_mutex;
}

// Shutdown loggers do not share benchmark_output_mutex(). Emit this record in
// one write bounded by POSIX's minimum PIPE_BUF, including both newlines, so a
// partial concurrent log cannot split or attach to the machine-readable line.
// Use stack storage because this path reports exhausted storage resources.
inline bool emit_benchmark_resource_exhaustion(const char *phase,
                                              const char *detail)
{
  char line[512];
  std::size_t size = 0;
  bool truncated = false;
  line[size++] = '\n';
  const auto append = [&](const char *text) {
    while (*text != '\0' && size < sizeof(line) - 1) {
      const unsigned char ch = static_cast<unsigned char>(*text++);
      line[size++] = ch < 0x20 || ch == 0x7f ? ' ' : static_cast<char>(ch);
    }
    truncated = truncated || *text != '\0';
  };
  append("TPCC_RESOURCE_EXHAUSTED phase=");
  append(phase ? phase : "unknown");
  append(" error=");
  append(detail ? detail : "");
  if (truncated) {
    line[size - 3] = '.';
    line[size - 2] = '.';
    line[size - 1] = '.';
  }
  line[size++] = '\n';

  std::lock_guard<std::mutex> lock(benchmark_output_mutex());
  std::cerr.flush();
  ssize_t written;
  do {
    written = ::write(STDERR_FILENO, line, size);
  } while (written < 0 && errno == EINTR);
  return written == static_cast<ssize_t>(size);
}

class locked_benchmark_ostream {
public:
  explicit locked_benchmark_ostream(std::ostream &stream)
    : lock_(benchmark_output_mutex()), stream_(stream)
  {
  }

  locked_benchmark_ostream(const locked_benchmark_ostream &) = delete;
  locked_benchmark_ostream(locked_benchmark_ostream &&) = delete;
  locked_benchmark_ostream &operator=(const locked_benchmark_ostream &) = delete;
  locked_benchmark_ostream &operator=(locked_benchmark_ostream &&) = delete;

  template <typename T>
  locked_benchmark_ostream &operator<<(T &&value)
  {
    stream_ << std::forward<T>(value);
    return *this;
  }

  locked_benchmark_ostream &operator<<(
      std::ostream &(*manipulator)(std::ostream &))
  {
    manipulator(stream_);
    return *this;
  }

  locked_benchmark_ostream &operator<<(std::ios &(*manipulator)(std::ios &))
  {
    manipulator(stream_);
    return *this;
  }

  locked_benchmark_ostream &operator<<(
      std::ios_base &(*manipulator)(std::ios_base &))
  {
    manipulator(stream_);
    return *this;
  }

  void flush()
  {
    stream_.flush();
  }

private:
  std::lock_guard<std::mutex> lock_;
  std::ostream &stream_;
};

inline locked_benchmark_ostream benchmark_cerr()
{
  return locked_benchmark_ostream(std::cerr);
}

inline locked_benchmark_ostream benchmark_cout()
{
  return locked_benchmark_ostream(std::cout);
}

} // namespace mako

#endif // MAKO_BENCHMARKS_BENCHMARK_OUTPUT_H
