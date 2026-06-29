#pragma once
// GStreamerCaptureSource (was GstRtspClient) - RTSP via a GStreamer pipeline with
// an appsink. GStreamer/GLib headers MUST precede OpenCV (OpenCV pulls Windows.h).
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <glib.h>

#include <atomic>
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
};

} // namespace lpr
