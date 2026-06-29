#pragma once
// Compatibility shim (lpr_basler only): maps the facade's AppLogger:: static
// helpers onto the clean lpr logger. LOG* macros come from lpr/Log.h.
#include "lpr/Log.h"
#include <exception>
#include <string>
#include <opencv2/core.hpp>   // cv::Exception

class AppLogger {
public:
    static void Init(const char* = "app") {}
    static void LogException(const std::exception& e, const std::string& where = "") {
        lpr::logException(e, where);
    }
    static void LogUnknownException(const std::string& where = "") {
        if (where.empty()) LOGF() << "Unknown exception";
        else               LOGF() << "Unknown exception at [" << where << "]";
    }
    static void LogPylonException(const std::string& d, const std::string& where = "") {
        LOGE() << "Pylon exception" << (where.empty() ? std::string() : (" at [" + where + "]")) << ": " << d;
    }
    static void LogCvException(const cv::Exception& e, const std::string& where = "") {
        LOGE() << "OpenCV exception" << (where.empty() ? std::string() : (" at [" + where + "]")) << ": " << e.what();
    }
};
