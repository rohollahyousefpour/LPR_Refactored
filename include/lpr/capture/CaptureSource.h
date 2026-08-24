#pragma once
// CaptureSource - abstract interface for every camera/video source (was
// virtual_cap_url). OpenCV-free at the header level (cv::Mat is forward-declared
// and only ever passed by const-ref), and boost-free: it uses std::function
// callbacks instead of boost::signals2.
#include <functional>
#include <limits>
#include <string>

namespace cv { class Mat; }   // forward declaration -> no OpenCV include needed here

namespace lpr {

class CaptureSource {
public:
    virtual ~CaptureSource() = default;

    using FrameCallback = std::function<void(const cv::Mat& frame, const cv::Mat& mono, long timestamp)>;
    using ErrorCallback = std::function<void()>;

    void onFrame(FrameCallback cb) { frameCb_ = std::move(cb); }
    void onError(ErrorCallback cb) { errorCb_ = std::move(cb); }

    virtual void setAddress(const std::string& address, int delayMs) = 0;
    virtual void setMonoAddress(const std::string& /*address*/, int /*delayMs*/) {}

    virtual void run()  = 0;          // capture loop, blocking until stop()
    virtual void stop() = 0;
    virtual bool isLive() const = 0;

    virtual void handleCommand(const std::string& /*key*/, const std::string& /*value*/) {}

    // Re-read this source's settings and apply any that changed, WITHOUT tearing the
    // whole pipeline down. Default: no-op (video/rtsp sources have nothing hardware-bound
    // to re-apply). The Basler facade overrides it to reconnect only the cameras whose
    // hardware settings (exposure / GigE / AOI / trigger) actually changed, so those apply
    // cleanly through the tested connect path. Called on a settings save, off the capture
    // thread; implementations must be thread-safe w.r.t. their own capture loop.
    virtual void reapplySettings() {}

    // Read the exposure (microseconds) + gain a sub-camera actually holds right now, by serial.
    // Default: unsupported (returns false). Basler overrides it so the manual-control live view can
    // report the true applied values instead of the requested ones.
    virtual bool readAppliedExposureGain(const std::string& /*serial*/,
                                         double& /*exposureUs*/, double& /*gain*/) { return false; }

    // Fetch the latest grabbed frame for a sub-camera by serial. Default: unsupported (false).
    // Basler overrides it so the manual-control live view can show EACH sensor of a pair
    // independently (the paired frame bus loses per-serial identity). `out` is set only on true.
    virtual bool latestFrame(const std::string& /*serial*/, cv::Mat& /*out*/) { return false; }

    // Current hardware AOI + sensor ranges for a sub-camera, so the manual-control UI can draw
    // Pylon-Viewer-style Width/Height/OffsetX/OffsetY sliders with correct bounds and values.
    struct Aoi {
        long width = 0, height = 0, offsetX = 0, offsetY = 0;              // current, pixels
        long widthMax = 0, heightMax = 0;                                 // sensor extents (max)
        long widthInc = 1, heightInc = 1, offsetXInc = 1, offsetYInc = 1; // increments
    };
    // Read the AOI a sub-camera holds now, by serial. Default: unsupported (false); Basler overrides.
    virtual bool readAppliedAoi(const std::string& /*serial*/, Aoi& /*out*/) { return false; }

    // Read-only device health for the manual-control diagnostics tiles. Any field left at its
    // sentinel (temperatureC = NaN, empty string, link/frame < 0) is simply not shown.
    struct Diag {
        double      temperatureC = std::numeric_limits<double>::quiet_NaN(); // DeviceTemperature (°C)
        double      linkSpeedMbps = -1;                                      // GevLinkSpeed (Mb/s)
        double      fps           = -1;                                      // measured grab rate
        long        incompleteFrames = -1;                                   // failed/partial buffers
        std::string model;                                                   // DeviceModelName
        std::string firmware;                                                // DeviceFirmwareVersion
        std::string serial;                                                  // DeviceSerialNumber
    };
    // Read the current device health of a sub-camera, by serial. Default: unsupported (false).
    virtual bool readAppliedDiag(const std::string& /*serial*/, Diag& /*out*/) { return false; }

    // Take (return + clear) a one-shot exported preset (.pfs text) a sub-camera stashed after an
    // "Export Preset" command, so the manual-live stream can ship it to the browser for download.
    // Default: unsupported (false); Basler overrides. `out` is set only on true.
    virtual bool readAppliedPreset(const std::string& /*serial*/, std::string& /*out*/) { return false; }

    // The DB camera id (== module gate), tagged by the source factory. Lets a
    // source forward a fault to messages.module_diag (lpr::diag::cameraFault)
    // under the right camera — the Basler facade uses its own CameraContext, but
    // the generic OpenCV/IP sources have no other handle on their identity.
    void setCameraId(std::string id) { cameraId_ = std::move(id); }

protected:
    void emitFrame(const cv::Mat& f, const cv::Mat& m, long t) { if (frameCb_) frameCb_(f, m, t); }
    void emitError() { if (errorCb_) errorCb_(); }

    FrameCallback frameCb_;
    ErrorCallback errorCb_;
    std::string   cameraId_;   // set by the factory; empty for anonymous sources
};

} // namespace lpr
