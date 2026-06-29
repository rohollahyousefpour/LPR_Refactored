#include "ISyncConfigurator.h"
#include "AppLogger.h"

namespace UCP = Basler_UniversalCameraParams;

namespace {
// Set an enum node, READ IT BACK, and report whether it actually holds the
// requested value. 'required' controls log severity. Returns true on success.
template <class Node, class Val>
bool setNode(Node& node, Val v, const char* nodeName, const std::string& cam, bool required) {
    try {
        if (!node.IsWritable()) {
            if (required)
                LOGE() << "[sync][" << cam << "] REQUIRED " << nodeName
                       << " NOT writable -- sync will not work";
            else
                LOGW() << "[sync][" << cam << "] " << nodeName << " not writable (skipped)";
            return false;
        }
        node.SetValue(v);
        // Verify by reading back the value the camera actually holds.
        const bool match = node.IsReadable() && (node.GetValue() == v);
        std::string got = node.IsReadable() ? std::string(node.ToString().c_str()) : "(unreadable)";
        if (match) {
            LOGI() << "[sync][" << cam << "] " << nodeName << " set -> " << got;
        } else if (required) {
            LOGE() << "[sync][" << cam << "] REQUIRED " << nodeName
                   << " did not take: holds '" << got << "' -- sync will not work";
        } else {
            LOGW() << "[sync][" << cam << "] " << nodeName << " coerced to '" << got << "'";
        }
        return match;
    } catch (const Pylon::GenericException& e) {
        if (required)
            LOGE() << "[sync][" << cam << "] REQUIRED " << nodeName
                   << " write FAILED: " << e.GetDescription();
        else
            LOGW() << "[sync][" << cam << "] " << nodeName << " write failed: " << e.GetDescription();
        return false;
    }
}

// Log the current input status of a line (e.g. Line1 on the slave) so you can
// see whether the wire is delivering a signal at setup time.
void logLineStatus(CameraDevice& dev, UCP::LineSelectorEnums line, const char* label) {
    auto* c = dev.raw(); if (!c) return;
    try {
        if (c->LineSelector.IsWritable()) c->LineSelector.SetValue(line);
        if (c->LineStatus.IsReadable())
            LOGI() << "[sync][" << dev.serial() << "] " << label
                   << " LineStatus=" << (c->LineStatus.GetValue() ? "HIGH" : "LOW");
        else
            LOGW() << "[sync][" << dev.serial() << "] " << label << " LineStatus not readable";
    } catch (const Pylon::GenericException& e) {
        LOGW() << "[sync][" << dev.serial() << "] LineStatus read failed: " << e.GetDescription();
    }
}

constexpr bool REQ = true;    // required node: failure means sync is broken
constexpr bool OPT = false;   // optional node
} // namespace

// config-2: free-running + Exposure Active output on Line 2.
// Applies to situation 1 (lone RGB) and situation 2 (pair mono master).
bool ExposureActiveOutputConfigurator::configure(CameraDevice& dev) {
    auto* c = dev.raw(); if (!c) return false;
    const std::string cam = dev.serial();
    bool ok = true;
    try {
        if (!dev.isOpen()) dev.open();
        LOGI() << "[sync][config-2][" << cam << "] BEGIN (master / free-run / ExposureActive out)";

        // REQUIRED for the master to emit the sync pulse:
        ok &= setNode(c->TriggerMode,  UCP::TriggerMode_Off,           "TriggerMode",  cam, REQ);
        ok &= setNode(c->LineSelector, UCP::LineSelector_Line2,        "LineSelector", cam, REQ);
        ok &= setNode(c->LineMode,     UCP::LineMode_Output,           "LineMode",     cam, REQ);
        ok &= setNode(c->LineSource,   UCP::LineSource_ExposureActive, "LineSource",   cam, REQ);
        // Optional (some models lack it / it doesn't affect the output pulse):
        setNode(c->TriggerSelector,    UCP::TriggerSelector_FrameStart,"TriggerSelector", cam, OPT);

        if (ok) LOGI()  << "[sync][config-2][" << cam << "] END ok";
        else    LOGE()  << "[sync][config-2][" << cam << "] END with FAILURES -- master will not emit sync pulse";
        return ok;
    }
    catch (const Pylon::GenericException& e) {
        AppLogger::LogPylonException(e.GetDescription(), "config-2 -- " + cam);
    }
    catch (const std::exception& e) { AppLogger::LogException(e, "config-2 -- " + cam); }
    catch (...) { AppLogger::LogUnknownException("config-2 -- " + cam); }
    return false;
}

// config-1: triggered slave. Applies to situation 3 (pair color slave).
bool TriggeredSlaveConfigurator::configure(CameraDevice& dev) {
    auto* c = dev.raw(); if (!c) return false;
    const std::string cam = dev.serial();
    bool ok = true;
    try {
        if (!dev.isOpen()) dev.open();
        LOGI() << "[sync][config-1][" << cam << "] BEGIN (slave / triggered on Line1)";

        // REQUIRED for the slave to be triggered:
        ok &= setNode(c->TriggerMode,       UCP::TriggerMode_On,               "TriggerMode",       cam, REQ);
        ok &= setNode(c->TriggerSource,     UCP::TriggerSource_Line1,          "TriggerSource",     cam, REQ);
        ok &= setNode(c->TriggerActivation, UCP::TriggerActivation_RisingEdge, "TriggerActivation", cam, REQ);
        // Optional:
        setNode(c->TriggerSelector,         UCP::TriggerSelector_FrameStart,   "TriggerSelector",   cam, OPT);

        // Show the input line state at setup (is the wire delivering anything?).
        logLineStatus(dev, UCP::LineSelector_Line1, "Line1(in)");

        if (ok) LOGI()  << "[sync][config-1][" << cam << "] END ok";
        else    LOGE()  << "[sync][config-1][" << cam << "] END with FAILURES -- slave will not trigger";
        return ok;
    }
    catch (const Pylon::GenericException& e) {
        AppLogger::LogPylonException(e.GetDescription(), "config-1 -- " + cam);
    }
    catch (const std::exception& e) { AppLogger::LogException(e, "config-1 -- " + cam); }
    catch (...) { AppLogger::LogUnknownException("config-1 -- " + cam); }
    return false;
}

bool SoftwareTriggerConfigurator::configure(CameraDevice& dev) {
    auto* c = dev.raw(); if (!c) return false;
    const std::string cam = dev.serial();
    bool ok = true;
    try {
        if (!dev.isOpen()) dev.open();
        ok &= setNode(c->TriggerMode,   UCP::TriggerMode_On,             "TriggerMode",   cam, REQ);
        ok &= setNode(c->TriggerSource, UCP::TriggerSource_Software,     "TriggerSource", cam, REQ);
        setNode(c->TriggerSelector,     UCP::TriggerSelector_FrameStart, "TriggerSelector", cam, OPT);
        LOGI() << "[sync][software][" << cam << "] " << (ok ? "armed" : "FAILED");
        return ok;
    }
    catch (const Pylon::GenericException& e) {
        AppLogger::LogPylonException(e.GetDescription(), "software -- " + cam);
    }
    catch (const std::exception& e) { AppLogger::LogException(e, "software -- " + cam); }
    catch (...) { AppLogger::LogUnknownException("software -- " + cam); }
    return false;
}

// Plain free-run: just turn the trigger OFF. No line/strobe output, no slave wiring.
bool FreeRunConfigurator::configure(CameraDevice& dev) {
    auto* c = dev.raw(); if (!c) return false;
    const std::string cam = dev.serial();
    bool ok = true;
    try {
        if (!dev.isOpen()) dev.open();
        LOGI() << "[sync][free-run][" << cam << "] BEGIN (trigger off, no strobe)";
        ok &= setNode(c->TriggerMode, UCP::TriggerMode_Off,            "TriggerMode",     cam, REQ);
        setNode(c->TriggerSelector,   UCP::TriggerSelector_FrameStart, "TriggerSelector", cam, OPT);
        LOGI() << "[sync][free-run][" << cam << "] END " << (ok ? "ok" : "FAILED");
        return ok;
    }
    catch (const Pylon::GenericException& e) {
        AppLogger::LogPylonException(e.GetDescription(), "free-run -- " + cam);
    }
    catch (const std::exception& e) { AppLogger::LogException(e, "free-run -- " + cam); }
    catch (...) { AppLogger::LogUnknownException("free-run -- " + cam); }
    return false;
}
