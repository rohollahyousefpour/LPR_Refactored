#include "lpr/manager/CameraManager.h"
#include "lpr/Log.h"
#include "lpr/config/SettingsManager.h"
#include <string>

namespace lpr {

void CameraManager::addCamera(const CameraSourceParams& params, MotionConfig cfg) {
    CameraSourceParams p = params;
    auto factory = [p]() -> std::unique_ptr<CaptureSource> { return CameraSourceFactory::create(p); };
    addCamera(p.gate, std::move(factory), cfg);
}

void CameraManager::addCamera(const std::string& gate, CameraWorker::SourceFactoryFn factory, MotionConfig cfg) {
    auto worker = std::make_unique<CameraWorker>(gate, std::move(factory), cfg);

    if (out_) {
        std::shared_ptr<FrameQueue> q = out_;
        worker->setFrameSink([q](FrameItem&& item) { q->push(std::move(item)); });
    }
    if (status_)
        worker->setStatusCallback(status_);
    if (rawObserver_)
        worker->setRawFrameObserver(rawObserver_);
    if (firstFrame_)
        worker->setFirstFrameCallback(firstFrame_);

    workers_.push_back(std::move(worker));
    LOGI() << "CameraManager: added camera gate=" << gate << " (total " << workers_.size() << ")";
}

void CameraManager::start() {
    for (auto& w : workers_) w->start();
    LOGI() << "CameraManager: started " << workers_.size() << " camera(s)";
}

void CameraManager::stop() {
    for (auto& w : workers_) w->stop();
    LOGI() << "CameraManager: stopped";
}

void CameraManager::handleCommand(const std::string& cameraId,
                                  const std::string& key, const std::string& value) {
    for (auto& w : workers_) {
        if (w && w->gate() == cameraId) {
            LOGI() << "CameraManager: command '" << key << "' -> camera " << cameraId;
            w->handleCommand(key, value);
            return;
        }
    }
    LOGW() << "CameraManager: command '" << key << "' for unknown camera id '" << cameraId << "'";
}

bool CameraManager::readExposureGain(const std::string& cameraId, const std::string& serial,
                                     double& exposureUs, double& gain) {
    for (auto& w : workers_) {
        if (w && w->gate() == cameraId)
            return w->readExposureGain(serial, exposureUs, gain);
    }
    return false;
}

} // namespace lpr

// --- settings-driven construction (bridge to the ported SettingsManager) ---
namespace lpr {
void CameraManager::buildFromSettings(MotionConfig defaults) {
    auto& s = SettingsManager::instance();
    for (int id : s.getCameraIds()) {
        CameraSourceParams p;
        p.gate        = std::to_string(id);
        p.typeOfLink  = s.getCameraSettingByIdAndKey<std::string>(id, "type_of_link").value_or("");
        p.address     = s.getCameraSettingByIdAndKey<std::string>(id, "CameraAddress").value_or("");
        p.monoAddress = s.getCameraSettingByIdAndKey<std::string>(id, "MonoCameraAddress").value_or("");
        p.delayMs     = s.getCameraSettingByIdAndKey<int>(id, "CameraDelayTime").value_or(defaults.delayMs);

        MotionConfig cfg = defaults;
        cfg.delayMs      = p.delayMs;
        cfg.minDeviation = s.getCameraSettingByIdAndKey<float>(id, "MinDeviation").value_or((float)defaults.minDeviation);
        cfg.maxDeviation = s.getCameraSettingByIdAndKey<float>(id, "MaxDeviation").value_or((float)defaults.maxDeviation);
        // ObjectSize = minimum number of changed pixels to treat as real motion (original: num_obj).
        cfg.minChangedPixels = s.getCameraSettingByIdAndKey<int>(id, "ObjectSize").value_or(defaults.minChangedPixels);
        // Mono+RGB pair: detect on the mono (MonoCameraAddress) and keep the RGB (CameraAddress)
        // as the evidence image. For Basler the facade already delivers the pair mono-first, so
        // detection stays on the primary; for IP/video/rtsp the mono is the SECONDARY stream.
        {
            const bool hasMono  = !p.monoAddress.empty() && p.monoAddress != "-1";
            const bool isBasler = (p.typeOfLink == "pylon");
            cfg.detectOnSecondary = hasMono && !isBasler;
        }
        // Note: detection-stage ROI/mask (getGeneralRoi/cropAndMask) is applied by
        // RoiCropRecognizer in the detection pipeline, not by the motion gate here.

        addCamera(p, cfg);
    }
    LOGI() << "CameraManager: built " << workers_.size() << " camera(s) from settings";
}

void CameraManager::reapplyCameraSettings() {
    auto& s = SettingsManager::instance();
    const MotionConfig def{};   // fallbacks match buildFromSettings' defaults
    int n = 0;
    for (auto& w : workers_) {
        if (!w) continue;
        int id = 0;
        try { id = std::stoi(w->gate()); } catch (...) { continue; }
        const double minDev = s.getCameraSettingByIdAndKey<float>(id, "MinDeviation").value_or((float)def.minDeviation);
        const double maxDev = s.getCameraSettingByIdAndKey<float>(id, "MaxDeviation").value_or((float)def.maxDeviation);
        const int    objSz  = s.getCameraSettingByIdAndKey<int>(id, "ObjectSize").value_or(def.minChangedPixels);
        const int    delay  = s.getCameraSettingByIdAndKey<int>(id, "CameraDelayTime").value_or(def.delayMs);
        w->setMotionTuning(minDev, maxDev, objSz, delay);
        // Structural/hardware camera settings (Basler exposure/GigE/AOI/trigger): the source
        // reconnects only the cameras whose connect-time settings actually changed. No-op for
        // video/rtsp sources and when nothing hardware-relevant changed.
        w->reapplySourceSettings();
        ++n;
    }
    LOGI() << "CameraManager: re-applied motion-gate settings to " << n << " camera(s) live"
           << " (+ hardware reconnect where changed)";
}
} // namespace lpr
