#pragma once
//
// CameraDevice
// ------------
// A RAII wrapper around exactly one Pylon::CBaslerUniversalInstantCamera.
//
// Responsibility (single):
//   Own the lifetime of one physical camera handle and expose safe, narrow
//   operations on it (open, close, grab, release). It does NOT decide policy
//   (exposure values, sync roles, bandwidth) -- those are supplied by Strategy
//   objects and the BandwidthCoordinator. It only knows how to *do* things to
//   one device and how to clean itself up.
//
// Why a class:
//   In the old code the raw shared_ptr<...> was passed everywhere and every
//   call site re-implemented "is it open? is it grabbing? stop/close/detach".
//   Centralising that here means there is exactly one correct teardown path
//   (the destructor) and one place that knows the Pylon node quirks.
//
#include <memory>
#include <mutex>
#include <string>
#include <pylon/PylonIncludes.h>                 // CImageFormatConverter, CPylonImage, etc.
#include <pylon/BaslerUniversalInstantCamera.h>
#include <opencv2/core.hpp>

class CameraDevice {
public:
    using PylonCamera = Pylon::CBaslerUniversalInstantCamera;

    // Takes ownership of an already-created Pylon device pointer (Attach with
    // Cleanup_Delete). Does NOT open it -- call open() explicitly so callers
    // control when network I/O happens.
    explicit CameraDevice(Pylon::IPylonDevice* device, std::string serial);
    ~CameraDevice();                       // guarantees full release

    CameraDevice(const CameraDevice&)            = delete;
    CameraDevice& operator=(const CameraDevice&) = delete;
    CameraDevice(CameraDevice&&)                 = delete;
    CameraDevice& operator=(CameraDevice&&)      = delete;

    // ---- Lifecycle ----
    bool open();                           // open + apply heartbeat hardening
    void close() noexcept;                 // stop grab + close, keep device
    bool isOpen()     const;
    bool isGrabbing() const;

    bool startGrabbing();                  // LatestImageOnly strategy
    void stopGrabbing() noexcept;

    // Outcome of a single retrieve, so the caller can react proportionately:
    // a transient Timeout/BadFrame need not trigger a full reconnect, while
    // DeviceError (Pylon exception / connection loss) should.
    enum class GrabStatus { Ok, Timeout, BadFrame, DeviceError };

    // Retrieve one frame as BGR8. 'isColor' reports whether the source pixel
    // format was color (true) or Mono8 (false). Never throws -- all OpenCV and
    // Pylon exceptions are caught and mapped to GrabStatus::DeviceError.
    GrabStatus retrieveBGR(cv::Mat& out, bool& isColor, unsigned timeoutMs = 1000);

    // Live GigE rate control: read / set the streaming frame rate on the fly (used
    // by the capture loop to throttle down on packet loss and recover). Safe to call
    // while grabbing. Returns 0 / false when the model has no such node.
    double acquisitionFrameRate();
    bool   setAcquisitionFrameRate(double fps);

    // ---- Identity / introspection ----
    const std::string& serial() const { return serial_; }
    std::string        deviceId() const;     // GigE IP/MAC -- NOT an identity key
    std::string        subnet()   const;     // best-effort NIC group label
    bool               isColorModel();       // PixelFormat != Mono8

    // Raw access for Strategy objects / configurators that need node-level
    // control. Returns nullptr if the device was never constructed.
    PylonCamera* raw() { return cam_.get(); }

    // One-shot preset export: an "Export Preset" command does SaveToString and stashes the .pfs
    // text here; the manual-live fill lambda takes it (clearing it) and ships it to the operator's
    // browser for download — so a plate reader on a SEPARATE host from the server is still portable.
    void setPendingPreset(std::string s) { std::lock_guard<std::mutex> lk(presetMtx_); pendingPreset_ = std::move(s); }
    bool takePendingPreset(std::string& out) {
        std::lock_guard<std::mutex> lk(presetMtx_);
        if (pendingPreset_.empty()) return false;
        out.swap(pendingPreset_); pendingPreset_.clear(); return true;
    }

private:
    void hardRelease() noexcept;           // stop/close/detach/destroy, no throw

    std::string                  serial_;
    std::shared_ptr<PylonCamera> cam_;
    Pylon::CImageFormatConverter converter_;
    bool                         lastIsColor_ = false;   // cached PixelFormat colorness
    std::mutex                   presetMtx_;
    std::string                  pendingPreset_;
};
