#pragma once
// CameraManager - owns N CameraWorkers (one per camera/gate) and fans their
// accepted frames into a single shared FrameQueue that the detection stage
// consumes. Clean replacement for the camera-owning half of Managment_Cameras
// (the plate-detection / tracking / recording pipeline is a separate module).
#include "lpr/manager/CameraWorker.h"
#include "lpr/capture/FrameQueue.h"
#include "lpr/capture/CameraSourceFactory.h"

#include <memory>
#include <string>
#include <vector>

namespace lpr {

class CameraManager {
public:
    // Shared output queue read by the detection pipeline.
    void setOutputQueue(std::shared_ptr<FrameQueue> queue) { out_ = std::move(queue); }
    void setStatusCallback(CameraWorker::StatusCallback cb) { status_ = std::move(cb); }
    // Observer for EVERY captured frame (pre motion gate), applied to each worker — for live view.
    void setRawFrameObserver(CameraWorker::FrameSink obs) { rawObserver_ = std::move(obs); }

    // Observer for the FIRST frame of each camera (full crude frame) — used to send the
    // crud_image the operator draws the ROI polygon on. Applied to each worker.
    void setFirstFrameObserver(CameraWorker::FirstFrameCallback cb) { firstFrame_ = std::move(cb); }

    // Production: build a worker that creates its source via the factory (and
    // re-creates it on every reconnect).
    void addCamera(const CameraSourceParams& params, MotionConfig cfg = {});

    // Lower-level / testable: supply your own source factory.
    // backpressure=true makes the frame sink BLOCK when the queue is full (never drop)
    // — used for OFFLINE video so every frame is processed (max direction sightings).
    // Live cameras pass false (drop-oldest: never stall the sensor).
    void addCamera(const std::string& gate, CameraWorker::SourceFactoryFn factory, MotionConfig cfg = {}, bool backpressure = false);

    // Build all cameras from SettingsManager (camera ids + per-camera keys).
    // Frame-size-dependent ROI is applied later via the worker's first-frame hook.
    void buildFromSettings(MotionConfig defaults = {});

    void start();
    void stop();
    std::size_t cameraCount() const { return workers_.size(); }

    // Re-read each camera's settings and push them to the running worker so a camera
    // settings save takes effect immediately. Motion-gate thresholds update in place (no
    // reconnect); Basler hardware settings (exposure/GigE/AOI/trigger) apply via a targeted
    // per-camera reconnect inside the source. Only camera address/link-type still need a full
    // restart (they select which device/source is built).
    void reapplyCameraSettings();

    // Route a manual command (NATS) to the camera whose gate == cameraId.
    // value is an opaque JSON-object string forwarded to the source. No-op (warns)
    // if no camera matches.
    void handleCommand(const std::string& cameraId,
                       const std::string& key, const std::string& value);

    // Read the true applied exposure (us) + gain for a sub-camera (by serial) under
    // the given camera id/gate. False if the camera/serial is not available.
    bool readExposureGain(const std::string& cameraId, const std::string& serial,
                          double& exposureUs, double& gain);

    // Fetch the latest frame a sub-camera (by serial) grabbed under the given camera id/gate,
    // for the manual-control live view. False if the camera/serial is not available.
    bool latestFrame(const std::string& cameraId, const std::string& serial, cv::Mat& out);

    // Read a sub-camera's current hardware AOI + sensor ranges (manual-control AOI sliders).
    bool readAoi(const std::string& cameraId, const std::string& serial, CaptureSource::Aoi& out);

    // Read a sub-camera's read-only device health (manual-control diagnostics tiles).
    bool readDiag(const std::string& cameraId, const std::string& serial, CaptureSource::Diag& out);

    // Take (return + clear) a one-shot exported preset (.pfs text) stashed after "Export Preset".
    bool readPreset(const std::string& cameraId, const std::string& serial, std::string& out);

private:
    std::vector<std::unique_ptr<CameraWorker>> workers_;
    CameraWorker::FrameSink rawObserver_;
    CameraWorker::FirstFrameCallback firstFrame_;
    std::shared_ptr<FrameQueue>                out_;
    CameraWorker::StatusCallback               status_;
};

} // namespace lpr
