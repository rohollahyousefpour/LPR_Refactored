#include "lpr/Log.h"
#include "lpr/util/Time.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace lpr {

const char* toString(Level lvl) {
    switch (lvl) {
        case Level::Trace:   return "TRACE";
        case Level::Debug:   return "DEBUG";
        case Level::Info:    return "INFO";
        case Level::Warning: return "WARN";
        case Level::Error:   return "ERROR";
        case Level::Fatal:   return "FATAL";
    }
    return "UNKNOWN";
}

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

static std::string defaultLogDir() {
#if defined(_WIN32)
    const char* programData = std::getenv("PROGRAMDATA");
    fs::path base = programData ? fs::path(programData) : fs::path("C:\\ProgramData");
    return (base / "Hoshyar" / "ALPR" / "logs").string();
#else
    return "/var/log/Hoshyar/ALPR/logs";
#endif
}

void Logger::openFile(const std::string& day, int index) {
    if (file_.is_open()) file_.close();
    currentDate_ = day;
    index_       = index;
    std::string name = appName_ + "_" + day + (index > 0 ? "_" + std::to_string(index) : "") + ".log";
    fs::path path = fs::path(dir_) / name;
    file_.open(path, std::ios::app);
    fileOk_ = file_.is_open();
    bytes_  = 0;
    if (fileOk_) {
        std::error_code ec;
        auto sz = fs::file_size(path, ec);
        if (!ec) bytes_ = static_cast<std::size_t>(sz);
    }
}

void Logger::purgeOldLogs() {
    if (retentionDays_ <= 0 || dir_.empty()) return;
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    const auto maxAge = std::chrono::hours(24 * retentionDays_);
    const std::string prefix = appName_ + "_";
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = it->path().filename().string();
        if (name.rfind(prefix, 0) != 0) continue;                 // not our app's log
        if (it->path().extension() != ".log") continue;
        const auto mtime = fs::last_write_time(it->path(), ec);
        if (ec) { ec.clear(); continue; }
        if (now - mtime > maxAge) {
            std::error_code rmEc;
            fs::remove(it->path(), rmEc);                          // delete the old log
        }
    }
}

void Logger::init(const std::string& appName, const std::string& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    appName_ = appName;
    dir_     = dir.empty() ? defaultLogDir() : dir;

    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) {
        std::cerr << "Logging: cannot create log dir '" << dir_ << "': " << ec.message()
                  << " -- falling back to console.\n";
        fileOk_  = false;
        console_ = true;
        return;
    }
    openFile(formatLocalTime("%Y-%m-%d"), 0);
    if (!fileOk_) {
        std::cerr << "Logging: cannot open log file in '" << dir_ << "' -- console only.\n";
        console_ = true;
    }
    purgeOldLogs();                       // clear stale logs at startup
}

void Logger::setRetentionDays(int days) {
    std::lock_guard<std::mutex> lock(mutex_);
    retentionDays_ = days;
}

void Logger::setLevel(Level minLevel) {
    std::lock_guard<std::mutex> lock(mutex_);
    minLevel_ = minLevel;
}

void Logger::setConsole(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    console_ = on;
}

bool Logger::isEnabled(Level lvl) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(lvl) >= static_cast<int>(minLevel_);
}

void Logger::write(Level lvl, const std::string& message) {
    const std::string ts  = formatLocalTime("%Y-%m-%d %H:%M:%S");
    const std::string day = formatLocalTime("%Y-%m-%d");
    std::string line = "[" + ts + "] [" + toString(lvl) + "] " + message + "\n";

    std::lock_guard<std::mutex> lock(mutex_);
    if (console_)
        std::clog << line;

    if (fileOk_) {
        if (day != currentDate_) {                     // new day -> new file + purge old
            openFile(day, 0);
            purgeOldLogs();
        }
        else if (bytes_ + line.size() > maxBytes_)     // size cap -> next index
            openFile(currentDate_, index_ + 1);
        file_ << line;
        file_.flush();
        bytes_ += line.size();
    } else if (!console_) {
        std::clog << line;                             // never silently drop
    }
}

LogStream::~LogStream() {
    Logger::instance().write(level_, os_.str());
}

void logException(const std::exception& ex, const std::string& where) {
    if (!where.empty()) LOGE() << "Exception at [" << where << "]: " << ex.what();
    else                LOGE() << "Exception: " << ex.what();
}

} // namespace lpr
