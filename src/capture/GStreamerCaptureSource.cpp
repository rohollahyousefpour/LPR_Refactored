#include "lpr/capture/GStreamerCaptureSource.h"
#include "lpr/Log.h"
#include "lpr/util/Time.h"
#include "lpr/net/ModuleDiag.h"

#include <opencv2/opencv.hpp>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace lpr {

namespace {
constexpr int kLatencyMs        = 100;
constexpr int kReconnectDelaySec = 1;

// When GStreamer is shipped next to the executable (the CMake post-build copies the vcpkg
// plugins into <exe_dir>/gstreamer-1.0 and the scanner into <exe_dir>), point GStreamer at
// them BEFORE gst_init so plate cameras work without the user setting any environment vars.
// A user-provided GST_PLUGIN_PATH is always respected. No-op on non-Windows (system paths).
void setupBundledPluginPath() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    std::wstring exe(buf, n);
    const auto slash = exe.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    const std::wstring dir = exe.substr(0, slash);

    auto toUtf8 = [](const std::wstring& w) {
        if (w.empty()) return std::string();
        const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                             nullptr, 0, nullptr, nullptr);
        std::string s((size_t)(len > 0 ? len : 0), '\0');
        if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                         s.data(), len, nullptr, nullptr);
        return s;
    };

    const std::string pluginDir = toUtf8(dir + L"\\gstreamer-1.0");
    const std::string rtspPlug  = toUtf8(dir + L"\\gstreamer-1.0\\gstrtsp.dll");
    const std::string scanner   = toUtf8(dir + L"\\gst-plugin-scanner.exe");

    // Only adopt a bundled plugin dir that is actually COMPLETE (contains the rtsp plugin).
    // Otherwise leave the environment alone so a working system GStreamer on PATH is used as-is.
    // A user-provided GST_PLUGIN_PATH always wins.
    if (!g_getenv("GST_PLUGIN_PATH")
            && g_file_test(pluginDir.c_str(), G_FILE_TEST_IS_DIR)
            && g_file_test(rtspPlug.c_str(),  G_FILE_TEST_EXISTS)) {
        g_setenv("GST_PLUGIN_PATH", pluginDir.c_str(), TRUE);
        if (!g_getenv("GST_PLUGIN_SCANNER") && g_file_test(scanner.c_str(), G_FILE_TEST_EXISTS))
            g_setenv("GST_PLUGIN_SCANNER", scanner.c_str(), TRUE);
    }
#endif
}

// Try a hardware decoder first (Windows d3d11), then portable software decoders.
GstElement* makeDecoder(const char* hw, const char* sw1, const char* sw2) {
    GstElement* d = gst_element_factory_make(hw, nullptr);
    if (!d) d = gst_element_factory_make(sw1, nullptr);
    if (!d) d = gst_element_factory_make(sw2, nullptr);   // avdec_* (libav) - cross-platform
    return d;
}
} // namespace

GStreamerCaptureSource::GStreamerCaptureSource() {
    static std::once_flag pluginPathOnce;
    std::call_once(pluginPathOnce, setupBundledPluginPath);   // must precede gst_init
    gst_init(nullptr, nullptr);   // safe to call repeatedly
}

GStreamerCaptureSource::~GStreamerCaptureSource() {
    stop();
}

void GStreamerCaptureSource::setAddress(const std::string& address, int /*delayMs*/) {
    uri_ = address;
}

bool GStreamerCaptureSource::buildPipeline() {
    pipeline_ = gst_pipeline_new("rtsp-pipeline");
    if (!pipeline_) { LOGE() << "GStreamer: failed to create pipeline"; return false; }

    rtspsrc_ = gst_element_factory_make("rtspsrc", "source");
    if (!rtspsrc_) { LOGE() << "GStreamer: failed to create rtspsrc"; return false; }

    g_object_set(rtspsrc_, "location", uri_.c_str(), "latency", kLatencyMs, "protocols", 4 /*TCP*/, NULL);
    g_signal_connect(rtspsrc_, "pad-added", G_CALLBACK(onPadAdded), this);
    gst_bin_add(GST_BIN(pipeline_), rtspsrc_);
    return true;
}

void GStreamerCaptureSource::run() {
    // Single-shot: build + run once. On error/EOS we return and let CameraWorker
    // own reconnect (uniform with the VLC/video sources).
    terminate_     = false;
    errorOccurred_ = false;
    sawFrame_      = false;
    measuredFps_   = -1.0;
    fpsFrames_     = 0;
    fpsWinStart_   = std::chrono::steady_clock::now();

    GMainContext* ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);

    if (!buildPipeline()) {
        g_main_context_pop_thread_default(ctx);
        g_main_context_unref(ctx);
        lpr::diag::cameraFault(cameraId_, uri_, "ERROR", "stream_open_failed",
            "ساختِ پایپ‌لاینِ GStreamer برای دوربینِ RTSP ناموفق شد — آدرس/کدک را بررسی کنید");
        emitError();
        return;
    }

    mainLoop_ = g_main_loop_new(ctx, FALSE);
    GstBus* bus = gst_element_get_bus(pipeline_);
    gst_bus_add_watch(bus, onBusMessage, this);
    gst_object_unref(bus);

    gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    LOGI() << "GStreamer: pipeline running for " << uri_;
    g_main_loop_run(mainLoop_);          // blocks until error/EOS/stop quits it

    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
    pipeline_ = nullptr; rtspsrc_ = nullptr; appsink_ = nullptr;
    g_main_loop_unref(mainLoop_); mainLoop_ = nullptr;
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);

    if (errorOccurred_ && !terminate_)
        emitError();                     // signal the worker to reconnect
}

void GStreamerCaptureSource::stop() {
    terminate_ = true;
    if (mainLoop_) g_main_loop_quit(mainLoop_);
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
}

bool GStreamerCaptureSource::isLive() const {
    return !terminate_;
}

void GStreamerCaptureSource::onPadAdded(GstElement* /*src*/, GstPad* newPad, gpointer user) {
    auto* self = static_cast<GStreamerCaptureSource*>(user);

    GstCaps* caps = gst_pad_get_current_caps(newPad);
    if (!caps) caps = gst_pad_query_caps(newPad, NULL);
    if (!caps) { LOGW() << "GStreamer: no caps on new pad"; return; }

    GstStructure* st = gst_caps_get_structure(caps, 0);
    if (g_strcmp0(gst_structure_get_name(st), "application/x-rtp") != 0) { gst_caps_unref(caps); return; }

    const gchar* enc = gst_structure_get_string(st, "encoding-name");
    if (!enc) { LOGW() << "GStreamer: unknown encoding on pad"; gst_caps_unref(caps); return; }

    GstElement* depay = nullptr; GstElement* parse = nullptr; GstElement* decoder = nullptr;
    if (g_ascii_strcasecmp(enc, "H264") == 0) {
        depay   = gst_element_factory_make("rtph264depay", nullptr);
        parse   = gst_element_factory_make("h264parse", nullptr);
        decoder = makeDecoder("d3d11h264dec", "openh264dec", "avdec_h264");
    } else if (g_ascii_strcasecmp(enc, "H265") == 0) {
        depay   = gst_element_factory_make("rtph265depay", nullptr);
        parse   = gst_element_factory_make("h265parse", nullptr);
        decoder = makeDecoder("d3d11h265dec", "openh265dec", "avdec_h265");
    } else {
        // Any-codec fallback: let decodebin autoplug the depayloader + (HW/SW) decoder for whatever
        // this stream is (MJPEG, VP8/VP9, MPEG-4, ...). decodebin accepts the application/x-rtp pad
        // directly and emits a raw-video pad, which onDecodebinPad links to videoconvert -> appsink.
        gst_caps_unref(caps);
        GstElement* dbin = gst_element_factory_make("decodebin", nullptr);
        if (!dbin) { LOGE() << "GStreamer: decodebin unavailable for " << enc; return; }
        LOGI() << "GStreamer: using decodebin fallback for encoding " << enc;
        g_signal_connect(dbin, "pad-added", G_CALLBACK(onDecodebinPad), self);
        gst_bin_add(GST_BIN(self->pipeline_), dbin);
        gst_element_sync_state_with_parent(dbin);
        GstPad* dbinSink = gst_element_get_static_pad(dbin, "sink");
        if (gst_pad_link(newPad, dbinSink) != GST_PAD_LINK_OK)
            LOGE() << "GStreamer: failed to link rtsp pad -> decodebin";
        gst_object_unref(dbinSink);
        return;
    }
    gst_caps_unref(caps);

    if (!depay || !parse || !decoder) { LOGE() << "GStreamer: failed to create decode elements for " << enc; return; }

    GstElement* queue = gst_element_factory_make("queue", nullptr);
    GstElement* conv  = gst_element_factory_make("videoconvert", nullptr);
    GstElement* cf    = gst_element_factory_make("capsfilter", nullptr);
    GstCaps*    bgr   = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGR", nullptr);
    g_object_set(cf, "caps", bgr, nullptr);
    gst_caps_unref(bgr);

    if (!self->appsink_) {
        self->appsink_ = gst_element_factory_make("appsink", "sink");
        gst_app_sink_set_emit_signals(GST_APP_SINK(self->appsink_), true);
        gst_app_sink_set_drop(GST_APP_SINK(self->appsink_), true);
        gst_app_sink_set_max_buffers(GST_APP_SINK(self->appsink_), 1);
        g_object_set(self->appsink_, "sync", FALSE, "max-lateness", 0, nullptr);
        g_signal_connect(self->appsink_, "new-sample", G_CALLBACK(onNewSample), self);
    }

    gst_bin_add_many(GST_BIN(self->pipeline_), depay, parse, decoder, queue, conv, cf, self->appsink_, nullptr);

    gst_element_sync_state_with_parent(depay);
    gst_element_sync_state_with_parent(parse);
    gst_element_sync_state_with_parent(decoder);
    gst_element_sync_state_with_parent(queue);
    gst_element_sync_state_with_parent(conv);
    gst_element_sync_state_with_parent(cf);
    gst_element_sync_state_with_parent(self->appsink_);

    GstPad* sinkPad = gst_element_get_static_pad(depay, "sink");
    if (gst_pad_link(newPad, sinkPad) != GST_PAD_LINK_OK) {
        LOGE() << "GStreamer: failed to link pad -> depay";
        gst_object_unref(sinkPad);
        return;
    }
    gst_object_unref(sinkPad);

    if (!gst_element_link_many(depay, parse, decoder, queue, conv, cf, self->appsink_, nullptr))
        LOGE() << "GStreamer: failed to link decode chain for " << enc;
    else
        LOGI() << "GStreamer: linked decode chain for " << enc;
}

void GStreamerCaptureSource::onDecodebinPad(GstElement* /*src*/, GstPad* newPad, gpointer user) {
    auto* self = static_cast<GStreamerCaptureSource*>(user);
    // Only link raw video pads.
    GstCaps* caps = gst_pad_get_current_caps(newPad);
    if (!caps) caps = gst_pad_query_caps(newPad, NULL);
    if (!caps) return;
    const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    bool isVideo = name && g_str_has_prefix(name, "video/");
    gst_caps_unref(caps);
    if (!isVideo) return;

    GstElement* conv = gst_element_factory_make("videoconvert", nullptr);
    GstElement* cf   = gst_element_factory_make("capsfilter", nullptr);
    GstCaps*    bgr  = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGR", nullptr);
    g_object_set(cf, "caps", bgr, nullptr);
    gst_caps_unref(bgr);

    if (!self->appsink_) {
        self->appsink_ = gst_element_factory_make("appsink", "sink");
        gst_app_sink_set_emit_signals(GST_APP_SINK(self->appsink_), true);
        gst_app_sink_set_drop(GST_APP_SINK(self->appsink_), true);
        gst_app_sink_set_max_buffers(GST_APP_SINK(self->appsink_), 1);
        g_object_set(self->appsink_, "sync", FALSE, "max-lateness", 0, nullptr);
        g_signal_connect(self->appsink_, "new-sample", G_CALLBACK(onNewSample), self);
    }

    gst_bin_add_many(GST_BIN(self->pipeline_), conv, cf, self->appsink_, nullptr);
    gst_element_sync_state_with_parent(conv);
    gst_element_sync_state_with_parent(cf);
    gst_element_sync_state_with_parent(self->appsink_);

    GstPad* sinkPad = gst_element_get_static_pad(conv, "sink");
    if (gst_pad_link(newPad, sinkPad) != GST_PAD_LINK_OK)
        LOGE() << "GStreamer: failed to link decodebin pad -> videoconvert";
    gst_object_unref(sinkPad);

    if (!gst_element_link_many(conv, cf, self->appsink_, nullptr))
        LOGE() << "GStreamer: failed to link decodebin tail";
    else
        LOGI() << "GStreamer: decodebin raw-video linked to appsink";
}

GstFlowReturn GStreamerCaptureSource::onNewSample(GstAppSink* sink, gpointer user) {
    auto* self = static_cast<GStreamerCaptureSource*>(user);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_ERROR;
    self->handleSample(sample);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

void GStreamerCaptureSource::handleSample(GstSample* sample) {
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstCaps*   caps   = gst_sample_get_caps(sample);
    if (!caps) return;

    GstStructure* s = gst_caps_get_structure(caps, 0);
    int w = 0, h = 0;
    if (!gst_structure_get_int(s, "width", &w) || !gst_structure_get_int(s, "height", &h)) return;

    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        // The mapped buffer is only valid until unmap, so emit an owning copy.
        cv::Mat frame(h, w, CV_8UC3, reinterpret_cast<unsigned char*>(map.data));
        {
            std::lock_guard<std::mutex> lock(frameMtx_);
            emitFrame(frame.clone(), cv::Mat(), nowEpochSeconds());
        }
        gst_buffer_unmap(buffer, &map);

        // First frame of the session => the stream is genuinely delivering. Surface
        // a positive notice so a recovery is visible, not just the preceding errors.
        if (!sawFrame_.exchange(true)) {
            lpr::diag::cameraFault(cameraId_, uri_, "INFO", "stream_connected",
                "استریمِ دوربینِ RTSP برقرار شد و فریم می‌دهد");
        }
        // Rolling fps over a ~15-frame window (this callback is single-threaded).
        if (++fpsFrames_ >= 15) {
            const auto now = std::chrono::steady_clock::now();
            const double secs = std::chrono::duration<double>(now - fpsWinStart_).count();
            if (secs > 0) measuredFps_ = fpsFrames_ / secs;
            fpsWinStart_ = now; fpsFrames_ = 0;
        }
    }
}

gboolean GStreamerCaptureSource::onBusMessage(GstBus* /*bus*/, GstMessage* msg, gpointer user) {
    auto* self = static_cast<GStreamerCaptureSource*>(user);
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr; gchar* dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            const std::string reason = err && err->message ? err->message : "unknown";
            LOGE() << "GStreamer error (" << self->uri_ << "): " << reason;
            ++self->readErrors_;
            self->measuredFps_ = -1.0;
            lpr::diag::cameraFault(self->cameraId_, self->uri_, "ERROR", "stream_lost",
                "خطای استریمِ دوربینِ RTSP: " + reason + " — تلاش برای اتصالِ مجدد");
            if (err) g_error_free(err);
            g_free(dbg);
            self->errorOccurred_ = true;
            if (self->mainLoop_) g_main_loop_quit(self->mainLoop_);
            break;
        }
        case GST_MESSAGE_EOS:
            LOGI() << "GStreamer: EOS for " << self->uri_;
            ++self->readErrors_;
            self->measuredFps_ = -1.0;
            lpr::diag::cameraFault(self->cameraId_, self->uri_, "WARNING", "stream_lost",
                "پایانِ جریانِ دوربینِ RTSP (EOS) — تلاش برای اتصالِ مجدد");
            self->errorOccurred_ = true;
            if (self->mainLoop_) g_main_loop_quit(self->mainLoop_);
            break;
        default: break;
    }
    return TRUE;
}

bool GStreamerCaptureSource::readAppliedDiag(const std::string& /*serial*/, Diag& out) {
    if (!sawFrame_.load()) return false;   // nothing meaningful until frames flow
    out.fps = measuredFps_.load();                 // effective frame rate
    out.incompleteFrames = readErrors_.load();     // stream errors/EOS this process
    out.model = "IP / RTSP (GStreamer)";           // shown as the device "model" tile
    return true;
}

} // namespace lpr
