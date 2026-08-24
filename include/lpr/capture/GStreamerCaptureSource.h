#pragma once
// GStreamerCaptureSource (was GstRtspClient) - RTSP via a GStreamer pipeline with
// an appsink. GStreamer/GLib headers MUST precede OpenCV (OpenCV pulls Windows.h).
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <glib.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

#include <opencv2/core.hpp>
#include "lpr/capture/CaptureSource.h"

namespace lpr {

class GStreamerCaptureSource : public CaptureSource {
public:
    GStreamerCaptureSource();
    ~GStreamerCaptureSource() override;

    GStreamerCaptureSource(const GStreamerCaptureSource&) = delete;
    GStreamerCaptureSource& operator=(const GStreamerCaptureSource&) = delete;

    void setAddress(const std::string& address, int delayMs) override;
    void run()  override;
    void stop() override;
    bool isLive() const override;

    // Report effective fps + read-error count so an RTSP camera surfaces in the
    // health dashboard's Grab test the same way a Basler camera does.
    bool readAppliedDiag(const std::string& serial, Diag& out) override;

private:
    bool buildPipeline();
    static void        onPadAdded(GstElement* src, GstPad* pad, gpointer user);
    static void        onDecodebinPad(GstElement* src, GstPad* pad, gpointer user);  // any-codec fallback
    static GstFlowReturn onNewSample(GstAppSink* sink, gpointer user);
    static gboolean    onBusMessage(GstBus* bus, GstMessage* msg, gpointer user);
    void handleSample(GstSample* sample);

    std::string       uri_;
    GstElement*       pipeline_ = nullptr;
    GstElement*       rtspsrc_  = nullptr;
    GstElement*       appsink_  = nullptr;
    GMainLoop*        mainLoop_ = nullptr;
    std::mutex        frameMtx_;
    std::atomic<bool> terminate_{false};
    std::atomic<bool> errorOccurred_{false};

    // Health telemetry (published to the manual-live diag + module_diag faults).
    std::atomic<double> measuredFps_{-1.0};   // rolling effective frame rate
    std::atomic<long>   readErrors_{0};       // pipeline errors/EOS this process
    std::atomic<bool>   sawFrame_{false};     // first-frame => emit an INFO "connected"
    int                 fpsFrames_ = 0;                     // gst thread only
    std::chrono::steady_clock::time_point fpsWinStart_;     // gst thread only
};

} // namespace lpr
