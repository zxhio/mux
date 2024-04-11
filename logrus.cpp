//===- logrus.cpp - logrus log impl -----------------------------*- C++ -*-===//
//
/// \file
/// Reduce spdlog diffusion in header，Use spdlog::log interface to write log.
//
// Author:  zxh
// Date:    2024/03/30 22:07:29
//===----------------------------------------------------------------------===//

#include "logrus.h"

#define LOGRUS_LEVEL_NAME_CRITICAL spdlog::string_view_t("fatal", 5)

// Use "fatal" replace "critical"
#define SPDLOG_LEVEL_NAMES                                                     \
  {                                                                            \
    SPDLOG_LEVEL_NAME_TRACE, SPDLOG_LEVEL_NAME_DEBUG, SPDLOG_LEVEL_NAME_INFO,  \
        SPDLOG_LEVEL_NAME_WARNING, SPDLOG_LEVEL_NAME_ERROR,                    \
        LOGRUS_LEVEL_NAME_CRITICAL, SPDLOG_LEVEL_NAME_OFF                      \
  }

// Use "F"("fatal") replace "C" ("critical")
#define SPDLOG_SHORT_LEVEL_NAMES                                               \
  { "T", "D", "I", "W", "E", "F", "O" }

#define FMT_HEADER_ONLY
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

namespace logrus {

void log_to(Logger *logger, const char *file, int line, const char *func,
            Level level, std::string_view msg,
            std::vector<std::pair<std::string, ValueType>> &&fields) {
  fmt::basic_memory_buffer<char, 256> buf;

  // Handle msg field
  std::copy_n(kFieldMsgKey, strlen_const(kFieldMsgKey),
              std::back_inserter(buf));
  std::copy_n(kFieldDelim, strlen_const(kFieldDelim), std::back_inserter(buf));
  std::copy_n(kFieldValueQuoted, strlen_const(kFieldValueQuoted),
              std::back_inserter(buf));
  std::copy_n(msg.data(), msg.size(), std::back_inserter(buf));
  std::copy_n(kFieldValueQuoted, strlen_const(kFieldValueQuoted),
              std::back_inserter(buf));

  // Handle normal fields
  for (const auto &pair : fields) {
    std::visit(
        [&](const auto &value) {
          std::string s =
              fmt::format(" {}={}{}{}", pair.first, kFieldValueQuoted, value,
                          kFieldValueQuoted);
          std::copy_n(s.begin(), s.size(), std::back_inserter(buf));
        },
        pair.second);
  }

  logger->log(file, line, func, level, std::string(buf.data(), buf.size()));
}

class LoggerImpl {
public:
  LoggerImpl() : logger_(spdlog::default_logger()) {}

  std::shared_ptr<spdlog::logger> logger_;
};

Logger::Logger() : impl_(std::make_shared<LoggerImpl>()) {}

void Logger::set_pattern(const std::string &pattern) {
  impl_->logger_->set_pattern(pattern);
}

void Logger::set_level(Level level) {
  impl_->logger_->set_level((spdlog::level::level_enum)level);
}

void Logger::set_rotating(const std::string &lname, const std::string &fname,
                          size_t max_file_size, size_t max_files) {
  impl_->logger_ =
      spdlog::rotating_logger_mt(lname, fname, max_file_size, max_files);
}

void Logger::log(const char *file, int line, const char *func, Level level,
                 std::string_view data) {
  impl_->logger_->log(spdlog::source_loc(file, line, func),
                      (spdlog::level::level_enum)level, data);
  if (level == Level::kFatal)
    std::exit(1);
}

void flush_every(std::chrono::seconds interval) {
  spdlog::flush_every(interval);
}

Logger &sl() {
  static Logger logger;
  return logger;
}

} // namespace logrus
