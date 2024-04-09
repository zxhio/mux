//===- logrus.h - logrus for cpp --------------------------------*- C++ -*-===//
//
/// \file
/// A structured logging interface similar to logrus based on spdlog,
/// implemented using C++17.
//
// Author:  zxh
// Date:    2024/02/06 14:49:29
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace logrus {

#define SPDLOG_LEVEL_TRACE 0
#define SPDLOG_LEVEL_DEBUG 1
#define SPDLOG_LEVEL_INFO 2
#define SPDLOG_LEVEL_WARN 3
#define SPDLOG_LEVEL_ERROR 4
#define SPDLOG_LEVEL_CRITICAL 5

enum Level : int {
  kTrace = SPDLOG_LEVEL_TRACE,
  kDebug = SPDLOG_LEVEL_DEBUG,
  kInfo = SPDLOG_LEVEL_INFO,
  kWarn = SPDLOG_LEVEL_WARN,
  kError = SPDLOG_LEVEL_ERROR,
  kFatal = SPDLOG_LEVEL_CRITICAL,
};

const char kFieldMsgKey[] = "msg";
const char kFieldErrKey[] = "error";
const char kFieldDelim[] = "=";
const char kFieldValueQuoted[] = "'";

template <size_t N>
constexpr size_t strlen_const(const char (&str)[N]) {
  return N - 1;
}

using ValueType =
    std::variant<char, unsigned char, short, unsigned short, int, unsigned int,
                 long, unsigned long, long long, unsigned long long, float,
                 double, bool, std::string, const char *, void *>;

using FieldType = std::pair<std::string, ValueType>;

class Logger;

void log_to(Logger *logger, const char *file, int line, const char *func,
            Level level, std::string_view msg,
            std::vector<std::pair<std::string, ValueType>> &&fields);

class Entry {
public:
  Entry(Logger *logger) : logger_(logger) {}

  Entry(Logger *logger, const std::initializer_list<FieldType> &fields)
      : logger_(logger), fields_(fields) {}

  template <typename T>
  Entry(Logger *logger, const std::string &k, const T &v)
      : Entry(logger, {{k, v}}) {}

  template <typename T>
  Entry(Logger *logger, const std::string &k, T &&v)
      : Entry(logger, {{k, std::forward<T>(v)}}) {}

  Entry with_fields(const std::initializer_list<FieldType> &fields) const {
    Entry entry{logger_};
    entry.fields_ = fields_;
    std::copy(fields.begin(), fields.end(), std::back_inserter(entry.fields_));
    return entry;
  }

  template <typename T1>
  Entry with_field(const std::string &k, const T1 &v) const {
    return with_fields({{k, v}});
  }

  template <typename T1>
  Entry with_field(const std::string &k, T1 &&v) const {
    return with_fields({{k, std::forward<T1>(v)}});
  }

#define LOGRUS_DECLARE_ENTRY_LOG(logfunc, level)                               \
  template <size_t N>                                                          \
  void logfunc(const char(&msg)[N]) {                                          \
    return log("", 0, "", level, msg);                                         \
  }

  LOGRUS_DECLARE_ENTRY_LOG(trace, Level::kTrace);
  LOGRUS_DECLARE_ENTRY_LOG(debug, Level::kDebug);
  LOGRUS_DECLARE_ENTRY_LOG(info, Level::kInfo);
  LOGRUS_DECLARE_ENTRY_LOG(warn, Level::kWarn);
  LOGRUS_DECLARE_ENTRY_LOG(error, Level::kError);
  LOGRUS_DECLARE_ENTRY_LOG(fatal, Level::kFatal);
#undef LOGRUS_DECLARE_ENTRY_LOG

#define LOGRUS_DECLARE_ENTRY_LOG_WITH_LOC(logfunc, level)                      \
  template <size_t N>                                                          \
  void logfunc(const char *file, int line, const char *func,                   \
               const char(&msg)[N]) {                                          \
    return log(file, line, func, level, msg);                                  \
  }

  LOGRUS_DECLARE_ENTRY_LOG_WITH_LOC(trace, Level::kTrace);
  LOGRUS_DECLARE_ENTRY_LOG_WITH_LOC(debug, Level::kDebug);
  LOGRUS_DECLARE_ENTRY_LOG_WITH_LOC(info, Level::kInfo);
  LOGRUS_DECLARE_ENTRY_LOG_WITH_LOC(warn, Level::kWarn);
  LOGRUS_DECLARE_ENTRY_LOG_WITH_LOC(error, Level::kError);
  LOGRUS_DECLARE_ENTRY_LOG_WITH_LOC(fatal, Level::kFatal);
#undef LOGRUS_DECLARE_ENTRY_LOG_WITH_LOC

private:
  // Message must be literal string.
  template <size_t N>
  void log(const char *file, int line, const char *func, Level level,
           const char (&msg)[N]) {
    log_to(logger_, file, line, func, level,
           std::string_view(msg, strlen_const(msg)), std::move(fields_));
  }

  template <size_t N>
  void log(Level level, const char (&msg)[N]) {
    log("", 0, "", level, msg);
  }

private:
  Logger *logger_;
  std::vector<FieldType> fields_;
};

class LoggerImpl;
class Logger {
public:
  Logger();
  Logger(const Logger &) = delete;
  Logger &operator=(const Logger &) = delete;

  void set_pattern(const std::string &pattern);
  void set_level(Level level);
  void set_rotating(const std::string &lname, const std::string &fname,
                    size_t max_file_size, size_t max_files);

  template <typename T>
  Entry with_field(const std::string &k, const T &v) {
    return Entry(this, k, v);
  }

  template <typename T>
  Entry with_field(const std::string &k, T &&v) {
    return Entry(this, k, std::forward<T>(v));
  }

  Entry with_fields(const std::initializer_list<FieldType> &fields) {
    return Entry(this, fields);
  }

  Entry with_error(int errnum) {
    return with_field(kFieldErrKey, strerror(errnum));
  }

#define LOGRUS_DECLARE_LOGGER_LOG(logfunc)                                     \
  template <size_t N>                                                          \
  void logfunc(const char(&msg)[N]) {                                          \
    Entry(this).logfunc(msg);                                                  \
  }

  LOGRUS_DECLARE_LOGGER_LOG(trace);
  LOGRUS_DECLARE_LOGGER_LOG(debug);
  LOGRUS_DECLARE_LOGGER_LOG(info);
  LOGRUS_DECLARE_LOGGER_LOG(warn);
  LOGRUS_DECLARE_LOGGER_LOG(error);
  LOGRUS_DECLARE_LOGGER_LOG(fatal);
#undef LOGRUS_DECLARE_LOGGER_LOG

#define LOGRUS_DECLARE_LOGGER_LOG_WITH_LOC(logfunc)                            \
  template <size_t N>                                                          \
  void logfunc(const char *file, int line, const char *func,                   \
               const char(&msg)[N]) {                                          \
    Entry(this).logfunc(file, line, func, msg);                                \
  }

  LOGRUS_DECLARE_LOGGER_LOG_WITH_LOC(trace);
  LOGRUS_DECLARE_LOGGER_LOG_WITH_LOC(debug);
  LOGRUS_DECLARE_LOGGER_LOG_WITH_LOC(info);
  LOGRUS_DECLARE_LOGGER_LOG_WITH_LOC(warn);
  LOGRUS_DECLARE_LOGGER_LOG_WITH_LOC(error);
  LOGRUS_DECLARE_LOGGER_LOG_WITH_LOC(fatal);
#undef LOGRUS_DECLARE_LOGGER_LOG_WITH_LOC

  void log(const char *file, int line, const char *func, Level level,
           std::string_view data);

private:
  Level level_;
  std::shared_ptr<LoggerImpl> impl_;
};

// Singleton logger.
Logger &sl();

void flush_every(std::chrono::seconds interval);

inline void set_pattern(const std::string &pattern) {
  sl().set_pattern(pattern);
}

inline void set_level(Level level) { sl().set_level(level); }

inline void set_rotating(const std::string &fname, size_t max_file_size,
                         size_t max_files) {
  sl().set_rotating("default", fname, max_file_size, max_files);
}

template <typename T>
inline Entry with_field(const std::string &k, const T &v) {
  return sl().with_field(k, v);
}

template <typename T>
inline Entry with_field(const std::string &k, T &&v) {
  return sl().with_field(k, std::forward<T>(v));
}

inline Entry with_fields(const std::initializer_list<FieldType> &fields) {
  return sl().with_fields(fields);
}

inline Entry with_error(int errnum) { return sl().with_error(errnum); }

#define LOGRUS_DECLARE_LOG_WITH_LOC(logfunc)                                   \
  template <size_t N>                                                          \
  inline void logfunc(const char *file, int line, const char *func,            \
                      const char(&msg)[N]) {                                   \
    return sl().logfunc(file, line, func, msg);                                \
  }

LOGRUS_DECLARE_LOG_WITH_LOC(trace);
LOGRUS_DECLARE_LOG_WITH_LOC(debug);
LOGRUS_DECLARE_LOG_WITH_LOC(info);
LOGRUS_DECLARE_LOG_WITH_LOC(warn);
LOGRUS_DECLARE_LOG_WITH_LOC(error);
LOGRUS_DECLARE_LOG_WITH_LOC(fatal);
#undef LOGRUS_DECLARE_LOG_WITH_LOC

#define LOGRUS_DECLARE_LOG(logfunc)                                            \
  template <size_t N>                                                          \
  inline void logfunc(const char(&msg)[N]) {                                   \
    return sl().logfunc(msg);                                                  \
  }

LOGRUS_DECLARE_LOG(trace);
LOGRUS_DECLARE_LOG(debug);
LOGRUS_DECLARE_LOG(info);
LOGRUS_DECLARE_LOG(warn);
LOGRUS_DECLARE_LOG(error);
LOGRUS_DECLARE_LOG(fatal);
#undef LOGRUS_DECLARE_LOG

} // namespace logrus

#define KV(k, v)                                                               \
  { k, v }

#define KERR(errnum) KV(logrus::kFieldErrKey, strerror(errnum))

#ifdef LOGRUS_WITH_LOC
#define LOG_(logger, logfunc, msg, ...)                                        \
  logger.with_fields({__VA_ARGS__})                                            \
      .logfunc(__FILE__, __LINE__, __FUNCTION__, msg)
#else
#define LOG_(logger, logfunc, msg, ...)                                        \
  logger.with_fields({__VA_ARGS__}).logfunc(msg)
#endif

#define LOG_TRACE(msg, ...) LOG_(logrus::sl(), trace, msg, __VA_ARGS__)
#define LOG_DEBUG(msg, ...) LOG_(logrus::sl(), debug, msg, __VA_ARGS__)
#define LOG_INFO(msg, ...) LOG_(logrus::sl(), info, msg, __VA_ARGS__)
#define LOG_WARN(msg, ...) LOG_(logrus::sl(), warn, msg, __VA_ARGS__)
#define LOG_ERROR(msg, ...) LOG_(logrus::sl(), error, msg, __VA_ARGS__)
#define LOG_FATAL(msg, ...) LOG_(logrus::sl(), fatal, msg, __VA_ARGS__)

#define LOG_TRACE_(l, msg, ...) LOG_(l, trace, msg, __VA_ARGS__)
#define LOG_DEBUG_(l, msg, ...) LOG_(l, debug, msg, __VA_ARGS__)
#define LOG_INFO_(l, msg, ...) LOG_(l, info, msg, __VA_ARGS__)
#define LOG_WARN_(l, msg, ...) LOG_(l, warn, msg, __VA_ARGS__)
#define LOG_ERROR_(l, msg, ...) LOG_(l, error, msg, __VA_ARGS__)
#define LOG_FATAL_(l, msg, ...) LOG_(l, fatal, msg, __VA_ARGS__)
