#include "benchmarks/benchmark_output.h"

#include <cstdlib>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition)
{
  if (!condition)
    std::abort();
}

ssize_t read_retry(int fd, char *data, std::size_t size)
{
  ssize_t result;
  do {
    result = ::read(fd, data, size);
  } while (result < 0 && errno == EINTR);
  return result;
}

void write_all(int fd, const char *data, std::size_t size)
{
  while (size != 0) {
    ssize_t written;
    do {
      written = ::write(fd, data, size);
    } while (written < 0 && errno == EINTR);
    require(written > 0);
    data += written;
    size -= static_cast<std::size_t>(written);
  }
}

// A separate logger writes after every ostream fragment, without acquiring
// the benchmark mutex. The acknowledgement makes the old chained-insertion
// failure deterministic instead of depending on a favorable scheduler.
class noisy_streambuf : public std::streambuf {
public:
  noisy_streambuf(int request_fd, int ack_fd)
    : request_fd_(request_fd), ack_fd_(ack_fd)
  {
  }

  void inject_noise()
  {
    write_all(request_fd_, "x", 1);
    char ack;
    require(read_retry(ack_fd_, &ack, 1) == 1 && ack == 'x');
  }

protected:
  std::streamsize xsputn(const char *data, std::streamsize size) override
  {
    write_all(STDERR_FILENO, data, static_cast<std::size_t>(size));
    inject_noise();
    return size;
  }

  int_type overflow(int_type ch) override
  {
    if (!traits_type::eq_int_type(ch, traits_type::eof())) {
      const char byte = traits_type::to_char_type(ch);
      write_all(STDERR_FILENO, &byte, 1);
      inject_noise();
    }
    return traits_type::not_eof(ch);
  }

  int sync() override { return 0; }

private:
  int request_fd_;
  int ack_fd_;
};

} // namespace

int main()
{
  int capture[2], request[2], ack[2];
  require(::pipe(capture) == 0);
  require(::pipe(request) == 0);
  require(::pipe(ack) == 0);
  std::cerr.flush();
  const int saved_stderr = ::dup(STDERR_FILENO);
  require(saved_stderr >= 0);
  require(::dup2(capture[1], STDERR_FILENO) == STDERR_FILENO);
  require(::close(capture[1]) == 0);

  std::string captured;
  std::thread reader([&] {
    char data[4096];
    for (;;) {
      const ssize_t size = read_retry(capture[0], data, sizeof(data));
      require(size >= 0);
      if (size == 0)
        break;
      captured.append(data, static_cast<std::size_t>(size));
    }
  });
  std::thread logger([&] {
    char signal;
    while (read_retry(request[0], &signal, 1) == 1) {
      if (signal == 'q')
        break;
      constexpr char noise[] = "<concurrent-shutdown-log>";
      write_all(STDERR_FILENO, noise, sizeof(noise) - 1);
      write_all(ack[1], "x", 1);
    }
  });

  noisy_streambuf noisy(request[1], ack[0]);
  std::streambuf *const original = std::cerr.rdbuf(&noisy);
  // Prove the fixture can fragment the previously used reporting pattern.
  mako::benchmark_cerr() << "CONTROL phase=" << "run" << '\n';
  std::vector<std::string> expected;
  const auto emit = [&](const char *phase, const char *detail,
                        std::string expected_line) {
    noisy.inject_noise();
    require(mako::emit_benchmark_resource_exhaustion(phase, detail));
    expected.push_back(std::move(expected_line));
  };
  for (const char *phase : {"startup", "load", "run"}) {
    for (int repeat = 0; repeat < 32; ++repeat) {
      emit(phase, "capacity reached", "TPCC_RESOURCE_EXHAUSTED phase="
           + std::string(phase) + " error=capacity reached");
    }
  }
  emit("run", "line\nbreak\r\ttab\x01\x7f",
       "TPCC_RESOURCE_EXHAUSTED phase=run error=line break  tab  ");
  emit(nullptr, nullptr, "TPCC_RESOURCE_EXHAUSTED phase=unknown error=");
  const std::string oversized(4096, 'z');
  const std::string prefix = "TPCC_RESOURCE_EXHAUSTED phase=run error=";
  const std::string exact_fit(512 - 2 - prefix.size(), 'z');
  emit("run", exact_fit.c_str(), prefix + exact_fit);
  emit("run", oversized.c_str(),
       prefix + std::string(512 - 2 - prefix.size() - 3, 'z') + "...");

  std::cerr.rdbuf(original);
  write_all(request[1], "q", 1);
  logger.join();
  require(::dup2(saved_stderr, STDERR_FILENO) == STDERR_FILENO);
  require(::close(saved_stderr) == 0);
  reader.join();
  for (int fd : {capture[0], request[0], request[1], ack[0], ack[1]})
    require(::close(fd) == 0);

  require(captured.find("CONTROL phase=<concurrent-shutdown-log>run")
          != std::string::npos);
  require(captured.find("CONTROL phase=run") == std::string::npos);
  std::istringstream lines(captured);
  std::string line;
  std::size_t count = 0;
  while (std::getline(lines, line)) {
    if (line.find("TPCC_RESOURCE_EXHAUSTED") == std::string::npos)
      continue;
    require(count < expected.size());
    require(line == expected[count++]);
    require(line.size() + 2 <= 512);
  }
  require(count == expected.size());
  std::cout << "Passed " << count
            << " bounded capacity diagnostics with concurrent shutdown logging\n";
}
