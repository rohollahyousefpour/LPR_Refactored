#pragma once
// Log.h - clean, dependency-free logger (replaces the Boost.Log "AppLogger").
// Keeps the same LOGT/LOGD/LOGI/LOGW/LOGE/LOGF stream macros and the
// "[timestamp] [LEVEL] message" format, with level filtering, console + daily
// rotating file output, and thread safety. No Boost required.
#include <sstream>
#include <string>
#include <fstream>
#include <mutex>
#include <cstddef>
#include <exception>

namespace lpr {

enum class Level { Trace, Debug, Info, Warning, Error, Fatal };
const char* toString(Level lvl);

class Logger {
public:
    static Logger& instance();

    // Configure file logging. If dir is empty a platform default is used
    // (PROGRAMDATA\Hoshyar\ALPR\logs on Windows, /var/log/Hoshyar/ALPR/logs else).
    // Safe to call once; falls back to console if the directory can't be opened.
    void init(const std::string& appName = "app", const std::string& dir = "");

    void setLevel(Level minLevel);
    void setConsole(bool on);
    void setRetentionDays(int days);            // delete log files older than this (0 = keep all)
    bool isEnabled(Level lvl) const;

    void write(Level lvl, const std::string& message);

private:
    Logger() = default;
    void openFile(const std::string& day, int index);   // caller holds mutex_
    void purgeOldLogs();                                 // caller holds mutex_

    mutable std::mutex mutex_;
    std::ofstream file_;
    std::string  appName_ = "app";
    std::string  dir_;
    std::string  currentDate_;
    int          index_    = 0;
    std::size_t  bytes_    = 0;
    std::size_t  maxBytes_ = 10u * 1024u * 1024u;   // 10 MB then roll
    int          retentionDays_ = 30;               // purge logs older than this many days
    Level        minLevel_ = Level::Trace;
    bool         console_  = false;
    bool         fileOk_   = false;
};

// RAII stream: accumulates a line, writes it to the Logger on destruction.
class LogStream {
public:
    explicit LogStream(Level lvl) : level_(lvl) {}
    ~LogStream();
    template <typename T>
    LogStream& operator<<(const T& v) { os_ << v; return *this; }
private:
    Level level_;
    std::ostringstream os_;
};

// Generic helper; SDK-specific ones (OpenCV/Pylon/OpenVINO) arrive with those modules.
void logException(const std::exception& ex, const std::string& where = "");

} // namespace lpr

// The for-loop guard skips building the string when the level is disabled, and
// is statement-safe (no dangling-else when used inside an if/else).
#define LOG_AT_(lvl) \
    for (bool _lpr_on = ::lpr::Logger::instance().isEnabled(lvl); _lpr_on; _lpr_on = false) \
        ::lpr::LogStream(lvl)

#define LOGT() LOG_AT_(::lpr::Level::Trace)
#define LOGD() LOG_AT_(::lpr::Level::Debug)
#define LOGI() LOG_AT_(::lpr::Level::Info)
#define LOGW() LOG_AT_(::lpr::Level::Warning)
#define LOGE() LOG_AT_(::lpr::Level::Error)
#define LOGF() LOG_AT_(::lpr::Level::Fatal)

#ifndef PROBE_FLUSH
#define PROBE_FLUSH() ((void)0)
#endif
