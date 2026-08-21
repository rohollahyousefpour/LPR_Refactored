#include "lpr/process/PlateProcessor.h"
#include "lpr/app/Application.h"
#include "lpr/net/InMemoryTransport.h"
#include "lpr/config/SettingsManager.h"
#include "lpr/Log.h"
#include "lpr/JsonFind.h"

#include <chrono>
#include <thread>
#include <algorithm>
#include <filesystem>
#include <cstdlib>

#ifdef LPR_WITH_NATS
#include "lpr/net/NatsTransport.h"
#endif
#include "lpr/net/SystemResourceMonitor.h"

namespace lpr {

using json = nlohmann::json;

Application::Application(Options opts) : opts_(std::move(opts)) {
#ifdef LPR_WITH_PYLON
    // Wire the full Basler facade into the camera factory (trigger/exposure/gain/sync/bandwidth).
    CameraSourceFactory::setBaslerCreator(&makeBaslerFacade);
#endif
}
Application::~Application() { stop(); }

bool Application::start(std::atomic<bool>& running) {
    // 1) Create + connect the transport. NATS connect is retried until it succeeds or the
    //    user stops (matches the original's "retry until connected" loop).
    if (opts_.useNats) {
#ifdef LPR_WITH_NATS
        NatsTransport::Config nc;
        nc.url = opts_.natsUrl;
        nc.token = opts_.natsToken;
        nc.user = opts_.natsUser;
        nc.password = opts_.natsPassword;
        nc.certFile = opts_.natsCertFile;
        nc.keyFile  = opts_.natsKeyFile;
        nc.caFile   = opts_.natsCaFile;
        nc.tlsHandshakeFirst = opts_.natsTlsFirst;
        nc.expectedHostname  = opts_.natsExpectedHostname;
        auto nats = std::make_unique<NatsTransport>(nc);

        const int waitMs = 5000;
        bool ok = false;
        while (running.load()) {
            if (nats->connect()) { ok = true; break; }
            LOGE() << "Application: NATS connect failed; retrying in " << (waitMs / 1000) << "s";
            for (int slept = 0; slept < waitMs && running.load(); slept += 100)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!ok) return false;                       // stop requested before we connected
        transport_ = std::move(nats);
#else
        LOGE() << "Application: useNats set but built without WITH_NATS; using in-memory transport";
        transport_ = std::make_unique<InMemoryTransport>();
#endif
    } else {
        transport_ = std::make_unique<InMemoryTransport>();
    }

    // 2) Run the original protocol's handshake: authenticate -> get client_id ->
    //    subscribe to commands -> request settings. (Offline mode uses bootstrapFromJson.)
    if (opts_.useNats) {
        if (opts_.natsToken.empty())
            LOGW() << "Application: no token (AUTH_TOKEN); authenticate will likely fail";
        authenticate();
    }
    LOGI() << "Application: started; authenticating then requesting settings";
    return true;
}

void Application::authenticate() {
    const std::string token = opts_.natsToken;
    const std::string reply = "response.token." + token;
    transport_->subscribe(reply, [this](const std::string&, const std::string& p) { onAuthReply(p); });
    json req = {{"token", token}};
    transport_->publishWithReply(opts_.authenticateSubject, req.dump(), reply);
    LOGI() << "Application: authentication request sent on '" << opts_.authenticateSubject << "'";
}

void Application::onAuthReply(const std::string& payload) {
    try {
        json r = json::parse(payload);
        if (r.value("status", "") != "success") {
            LOGE() << "Application: authentication failed: " << payload;
            return;
        }
        const auto& cid = r.at("client_id");
        clientId_ = cid.is_number_integer() ? std::to_string(cid.get<int>())
                                            : cid.get<std::string>();
        LOGI() << "Application: authenticated, client_id=" << clientId_;
        subscribeToCommands();
        requestSettings();
    } catch (const std::exception& e) {
        LOGE() << "Application: bad auth reply: " << e.what();
    }
}

void Application::subscribeToCommands() {
    const std::string cmd = "command." + clientId_;
    transport_->subscribe(cmd, [this](const std::string&, const std::string& p) { onCommand(p); });
    LOGI() << "Application: subscribed to commands on '" << cmd << "'";
}

void Application::onCommand(const std::string& payload) {
    try {
        json cmd = json::parse(payload);
        // The backend's nesting/casing for these fields has varied; search the whole payload for
        // the first matching key rather than assuming a fixed path (command_type came back empty
        // when read from messageBody.data, and camera_id vs cameraId casing also differs).
        const json* ctNode = findAnyKeyDeep(cmd, {"commandType", "command_type"});
        std::string ct = (ctNode && ctNode->is_string()) ? ctNode->get<std::string>() : "";
        LOGI() << "Application: <<< command received command_type='" << ct << "'";
        if (ct.empty())
            LOGI() << "Application: raw command payload=" << payload;
        // A restart request from the backend (`reset_lpr`). We must NOT tear the process
        // down inside the callback: first fall through and publish the success ack below,
        // THEN fire the restart handler so the backend sees the acknowledgment before the
        // reader exits and re-launches.
        bool restartAfterAck = false;
        if (router_) {
            if      (ct == "recording")  { LOGI() << "Application: -> startRecording"; router_->startRecording(cmd); }
            else if (ct == "streaming")  { LOGI() << "Application: -> liveView";       router_->liveView(cmd); }
            else if (ct == "set_config") { LOGI() << "Application: -> setCameraConfig"; router_->setCameraConfig(cmd); }
            else if (ct == "lpr_settings") { LOGI() << "Application: -> requestSettings"; requestSettings(); }
            else if (ct == "reset_lpr" || ct == "restart" || ct == "reset") {
                // Backend-initiated restart of the whole reader (the "restart the LPR"
                // API). Defer the actual re-launch until after the ack is published.
                LOGW() << "Application: -> reset_lpr (restart requested by backend)";
                restartAfterAck = true;
            }
            else if (ct == "screenshot" || ct == "get_screenshot" || ct == "camera_screenshot") {
                const json* idN = findAnyKeyDeep(cmd, {"camera_id", "cameraId", "gate_id", "gate", "id"});
                if (!idN) { LOGW() << "Application: screenshot command needs camera_id"; }
                else {
                    const std::string cid = idN->is_string() ? idN->get<std::string>() : idN->dump();
                    LOGI() << "Application: -> screenshot id=" << cid;
                    sendScreenshot(cid);
                }
            }
            else if (ct == "camera_command" || ct == "camera_control" || ct == "set_camera") {
                // Manual per-camera control (exposure/gain/trigger/sync). Tolerant to the
                // backend's field naming, like the command_type lookup above.
                constexpr int kManualLiveSeconds = 60;   // live stays on this long after the last manual change
                const json* idN  = findAnyKeyDeep(cmd, {"camera_id", "cameraId", "gate_id", "gate", "id"});
                const json* keyN = findAnyKeyDeep(cmd, {"key", "parameter", "name", "setting"});
                const json* valN = findAnyKeyDeep(cmd, {"value", "val"});
                const json* serN = findAnyKeyDeep(cmd, {"camera_serial", "cameraSerial", "serial"});
                const json* nodeN = findAnyKeyDeep(cmd, {"node", "feature", "param_name"});
                const json* unitN = findAnyKeyDeep(cmd, {"unit"});
                if (!idN || !keyN || !keyN->is_string()) {
                    LOGW() << "Application: camera_command needs camera_id + key";
                } else if (!cameras_) {
                    LOGW() << "Application: camera_command before cameras ready; ignored";
                } else {
                    const std::string cid = idN->is_string() ? idN->get<std::string>() : idN->dump();
                    const std::string key = keyN->get<std::string>();
                    json down = json::object();
                    if (valN)  down["value"] = *valN;   // preserve value's JSON type (absent for revert)
                    if (serN)  down["camera_serial"] = serN->is_string() ? serN->get<std::string>() : serN->dump();
                    if (nodeN && nodeN->is_string()) down["node"] = nodeN->get<std::string>();
                    if (unitN && unitN->is_string()) down["unit"] = unitN->get<std::string>();  // explicit us/norm
                    LOGI() << "Application: -> camera_command id=" << cid << " key='" << key << "'";
                    cameras_->handleCommand(cid, key, down.dump());

                    // While the operator is manually adjusting a camera, stream its live
                    // view so they can see the effect; reverting ends the session. Live is
                    // keyed by camera id (gate); the serial is logged for correlation.
                    const bool isRevert = (key == "Use Settings" || key == "Reset" || key == "Exposure Auto");
                    const std::string ser = serN ? down.value("camera_serial", std::string{}) : std::string{};
                    {
                        std::lock_guard<std::mutex> lk(manualLiveMtx_);
                        if (isRevert) {
                            manualLive_.erase(cid);
                            if (media_) media_->clearLiveSerial(cid);
                            if (live_)  live_->disableLive(cid);
                            LOGI() << "Application: manual-control live stopped for camera " << cid;
                        } else {
                            auto& st = manualLive_[cid];
                            st.expireAt = std::chrono::steady_clock::now()
                                        + std::chrono::seconds(kManualLiveSeconds);  // refreshed each command
                            if (!ser.empty() && valN && valN->is_number()) {
                                const double v = valN->get<double>();
                                if      (key == "Exposure Time") st.vals[ser].exposureUs = (v > 1.0 ? v : -1.0);
                                else if (key == "Gain")          st.vals[ser].gain       = v;
                            }
                            LOGI() << "Application: manual-control live active for camera " << cid
                                   << (ser.empty() ? "" : (" serial " + ser)) << ", "
                                   << kManualLiveSeconds << "s";
                        }
                    }
                }
            }
            else LOGW() << "Application: unknown command_type '" << ct << "'";
        } else {
            LOGW() << "Application: command '" << ct << "' before pipeline ready; ignored";
        }
        json resp = {{"status", "success"}, {"command_type", ct}};
        LOGI() << "Application: >>> command response to 'response." << clientId_ << "'";
        transport_->publish("response." + clientId_, resp.dump());

        // The ack is out — now honour a restart request. onRestart_ only flips flags on
        // the main thread (which then stops the pipeline and re-execs the process).
        if (restartAfterAck) {
            LOGW() << "Application: restart handler firing after ack";
            if (onRestart_) onRestart_();
            else LOGW() << "Application: reset_lpr received but no restart handler set";
        }
    } catch (const std::exception& e) {
        LOGE() << "Application: bad command: " << e.what();
    }
}

void Application::sendScreenshot(const std::string& gate) {
    cv::Mat frame;
    {
        std::lock_guard<std::mutex> lk(lastFrameMtx_);
        auto it = lastFrame_.find(gate);
        if (it != lastFrame_.end()) frame = it->second;   // shared header; encoded below
    }
    if (frame.empty()) {
        LOGW() << "Application: no frame available yet for camera " << gate << "; screenshot skipped";
        return;
    }
    if (media_) {
        media_->sendScreenshot(gate, frame);
        LOGI() << "Application: screenshot sent for camera " << gate
               << " (" << frame.cols << "x" << frame.rows << ")";
    } else {
        LOGW() << "Application: no media sender; screenshot not sent for camera " << gate;
    }
}

void Application::requestSettings() {
    const std::string reply = "alpr.settings.response." + clientId_;
    transport_->subscribe(reply, [this](const std::string&, const std::string& p) { onSettings(p); });
    json req = {{"client_id", clientId_}, {"request_type", "alpr_settings"}};
    transport_->publish(opts_.settingsRequestSubject, req.dump());
    LOGI() << "Application: settings requested on '" << opts_.settingsRequestSubject
           << "', awaiting reply on '" << reply << "'";
}

void Application::onSettings(const std::string& payload) {
    try {
        json msg = json::parse(payload);
        // Settings live in messageBody (which itself may carry a "data" wrapper that
        // SettingsManager::loadAll unwraps).
        const json& body = msg.contains("messageBody") ? msg.at("messageBody") : msg;
        bootstrapFromJson(body);
    } catch (const std::exception& e) {
        LOGE() << "Application: bad settings payload: " << e.what();
    }
}

void Application::publishManualLive(const FrameItem& f) {
    if (!media_) return;

    // Serials for this gate from settings: CameraAddress = RGB/primary, MonoCameraAddress = mono.
    std::string rgbSerial, monoSerial;
    try {
        const int id = std::stoi(f.gate);
        auto& s = SettingsManager::instance();
        rgbSerial  = s.getCameraSettingByIdAndKey<std::string>(id, "CameraAddress").value_or("");
        monoSerial = s.getCameraSettingByIdAndKey<std::string>(id, "MonoCameraAddress").value_or("");
    } catch (...) {}
    if (monoSerial == "-1") monoSerial.clear();

    // Snapshot the operator's last-set exposure/gain per serial.
    std::map<std::string, ExpGain> vals;
    {
        std::lock_guard<std::mutex> lk(manualLiveMtx_);
        auto it = manualLive_.find(f.gate);
        if (it != manualLive_.end()) vals = it->second.vals;
    }
    auto fill = [&](MediaSender::ManualLiveCam& c) {
        // Prefer the exposure/gain the sensor ACTUALLY holds now (ground truth, post-clamp /
        // post-increment coercion) so the operator tunes against reality, not the request.
        double us = 0.0, g = 0.0;
        if (cameras_ && cameras_->readExposureGain(f.gate, c.serial, us, g)) {
            c.exposureUs = us; c.gain = g;
            return;
        }
        auto v = vals.find(c.serial);   // fallback: last value the operator requested
        if (v != vals.end()) { c.exposureUs = v->second.exposureUs; c.gain = v->second.gain; }
    };

    std::vector<MediaSender::ManualLiveCam> cams;
    if (!monoSerial.empty()) {
        // mono+RGB pair: fetch EACH sensor by its OWN serial so the two live views are matched
        // to the right camera. The paired frame bus (onFramePair) collapses the two Mats and
        // loses which serial is which -- for a same-kind (e.g. color+color) pair that would swap
        // them. latestFrame(serial) is unambiguous; fall back to the paired frame's two Mats
        // (evidenceImage=RGB, image=mono) when the source can't serve by serial.
        cv::Mat rgbImg, monoImg;
        const bool haveRgb  = cameras_ && cameras_->latestFrame(f.gate, rgbSerial,  rgbImg)  && !rgbImg.empty();
        const bool haveMono = cameras_ && cameras_->latestFrame(f.gate, monoSerial, monoImg) && !monoImg.empty();

        MediaSender::ManualLiveCam rgb;  rgb.serial = rgbSerial;  rgb.role = "rgb";
        rgb.image = haveRgb ? rgbImg : (f.evidenceImage.empty() ? f.image : f.evidenceImage); fill(rgb);
        MediaSender::ManualLiveCam mono; mono.serial = monoSerial; mono.role = "mono";
        mono.image = haveMono ? monoImg : f.image; fill(mono);
        cams.push_back(std::move(rgb));
        cams.push_back(std::move(mono));
    } else {
        // single camera: image is the camera (CameraAddress serial)
        MediaSender::ManualLiveCam one; one.serial = rgbSerial; one.role = "single"; one.image = f.image; fill(one);
        cams.push_back(std::move(one));
    }
    media_->sendManualLive(f.gate, cams);
}

std::unique_ptr<IPlateRecognizer> Application::buildRecognizerChain() {
    auto& s = SettingsManager::instance();
    const std::string modelType = s.getLpr<std::string>(opts_.keyBackend).value_or(opts_.defaultBackend);
    const Backend backend = parseBackend(modelType);
    const std::string device = s.getLpr<std::string>(opts_.keyDevice).value_or("CPU");
    LOGI() << "Application: model_type='" << modelType << "' -> backend " << toString(backend)
           << ", device " << device;
    const int carDetection = s.getLpr<int>("car_detection").value_or(0);
    const int trackPlates  = s.getLpr<int>("track_plates").value_or(0);

    // Load the local config file (model/training-data paths), as the original did.
    if (!config_ && !opts_.configPath.empty()) {
        config_ = std::make_unique<AppConfig>(opts_.configPath);
        if (config_->isLoaded())
            LOGI() << "Application: loaded config file '" << opts_.configPath << "'";
        else
            LOGW() << "Application: could not load config file '" << opts_.configPath << "'";
    }

    // Model paths: from the config file (config_file dir + file) when present, exactly like the
    // original (conf->config_file + "/" + conf->car_file); otherwise from the NATS settings.
    EastConfig east;
    // Detector input size + plate geometry come from the NATS settings (original reads deep_width/
    // deep_height for the detector and deep_plate_* for geometry), with the config file as fallback.
    {
        int dw = s.getLpr<int>("deep_width").value_or(config_ && config_->isLoaded() ? config_->deep_width : 0);
        int dh = s.getLpr<int>("deep_height").value_or(config_ && config_->isLoaded() ? config_->deep_height : 0);
        if (dw > 0) east.detectWidth  = dw;
        if (dh > 0) east.detectHeight = dh;
        if (auto v = s.getLpr<int>("deep_plate_height"))  east.heightScale = static_cast<float>(*v);
        if (auto v = s.getLpr<int>("deep_plate_width_1")) east.widthScale1 = static_cast<float>(*v);
        if (auto v = s.getLpr<int>("deep_plate_width_2")) east.widthScale2 = static_cast<float>(*v);
        if (auto v = s.getLpr<float>("deep_detect_prob")) east.scoreThresh = *v;
        LOGI() << "Application: detector input " << east.detectWidth << "x" << east.detectHeight
               << " scoreThresh=" << east.scoreThresh;
    }
    PlateOcr::Config ocr;
    ocr.backend = backend; ocr.device = device;
    // Charset by multi_language (original Ocr_Model): 1 -> mixed-case 60-char set; else 38-char set.
    if (s.getLpr<int>("multi_language").value_or(0) == 1)
        ocr.alphabet = " 0123456789abcdefhijlmnoqstvwypuzxABCDEFGHIJKLMNOPQRSTUVWXYZ";
    else
        ocr.alphabet = " 0123456789abcdefghijlkmnoqstvwypuzxDS";
    auto join = [](const std::string& dir, const std::string& file) -> std::string {
        if (file.empty()) return "";
        return dir.empty() ? file : dir + "/" + file;
    };
    std::string detModel, ocrModel, carModel;
    if (config_ && config_->isLoaded()) {
        const std::string dir = config_->config_file;
        detModel = join(dir, config_->plate_detection_file);
        ocrModel = join(dir, config_->ocr_file);
        carModel = join(dir, config_->car_file);
        if (config_->deep_detect_prob > 0.0f) east.scoreThresh = config_->deep_detect_prob;
        LOGI() << "Application: model paths from config file (detector='" << detModel
               << "', ocr='" << ocrModel << "', car='" << carModel << "')";
    } else {
        detModel = s.getLpr<std::string>(opts_.keyPlateDetector).value_or("");
        ocrModel = s.getLpr<std::string>(opts_.keyOcrModel).value_or("");
        carModel = s.getLpr<std::string>(opts_.keyCarModel).value_or("");
    }

    plateStage_ = std::make_unique<PlateRecognizer>();
    if (auto a = s.getLpr<std::string>(opts_.keyOcrAlphabet)) ocr.alphabet = *a;
    if (!plateStage_->load(detModel, ocrModel, backend, device, east, ocr))
        LOGW() << "Application: plate stage models not loaded (recognition will be empty until provided)";

    // Original-parity filters, all from settings.
    {
        float ocrProb = s.getLpr<float>("ocr_prob").value_or(0.0f);
        int   pw      = s.getLpr<int>("plate_width").value_or(0);
        int   ph      = s.getLpr<int>("plate_height").value_or(0);
        plateStage_->setOcrProb(ocrProb);
        plateStage_->setMinPlateSize(pw, ph);
        LOGI() << "Application: ALPR filters ocr_prob=" << ocrProb
               << " plate_min=" << pw << "x" << ph;
    }

    if (s.getLpr<int>("debug").value_or(0) == 1) {
        plateStage_->setDebugDiagnostics(true);
        LOGI() << "Application: ALPR per-frame diagnostics ON (box count + OCR text/conf)";
    }

    IPlateRecognizer* base = plateStage_.get();

    // Select the variant (mirrors run_plate_detection's car_detection / track_plates switch).
    if (carDetection == 1 || carDetection == 2) {
        vehicleDet_ = std::make_unique<VehicleDetector>();
        // Car-model input size from settings (original deep_car_width/deep_car_height), config fallback.
        VehicleConfig vcfg;
        int cw = s.getLpr<int>("deep_car_width").value_or(config_ && config_->isLoaded() ? config_->deep_car_width : 0);
        int ch = s.getLpr<int>("deep_car_height").value_or(config_ && config_->isLoaded() ? config_->deep_car_height : 0);
        if (cw > 0) vcfg.width  = cw;
        if (ch > 0) vcfg.height = ch;
        // The original decoded ONLY output head index 2 (coarse grid + large anchors {6,7,8});
        // decoding the fine heads on this model yields spurious boxes. Match the original.
        vcfg.headIndices    = { 2 };
        // Detection thresholds: original constants (num_classes=5, conf=0.45, NMS=0.5), each
        // overridable from settings if those keys are present (default-preserving).
        vcfg.numClasses     = s.getLpr<int>("car_num_classes").value_or(vcfg.numClasses);
        vcfg.scoreThreshold = s.getLpr<float>("deep_car_detect_prob").value_or(vcfg.scoreThreshold);
        LOGI() << "Application: vehicle model input " << vcfg.width << "x" << vcfg.height
               << " conf=" << vcfg.scoreThreshold << " classes=" << vcfg.numClasses
               << " head=2 (anchors {6,7,8})";
        if (!vehicleDet_->load(carModel, backend, device, vcfg))
            LOGW() << "Application: vehicle model not loaded";
        VehicleAwarePlateRecognizer::Config vc;
        vc.useTracking = (carDetection == 1);   // 1 = with_car (track), 2 = withouttrack_car
        vehicleRec_ = std::make_unique<VehicleAwarePlateRecognizer>(*vehicleDet_, *plateStage_, vc);
        base = vehicleRec_.get();
        LOGI() << "Application: pipeline = vehicle" << (vc.useTracking ? "+track" : "") << " -> plate";
    } else if (trackPlates == 1) {
        plateTrackRec_ = std::make_unique<PlateTrackingRecognizer>(*plateStage_);
        base = plateTrackRec_.get();
        LOGI() << "Application: pipeline = plate + tracking";
    } else {
        LOGI() << "Application: pipeline = plate only";
    }

    // Outermost: per-camera detection ROI + polygon mask (from settings).
    //
    // IMPORTANT — hardware-AOI interaction: when a camera is hardware-cropped, the frame that
    // reaches detection is ALREADY the plate region (the Basler sensor was cropped to the ROI
    // bbox / manual rect). Re-applying the software ROI/mask here would map the full-sensor-
    // normalized polygon onto the smaller cropped frame, shrinking it to a central sub-region
    // and cutting plates in half. So for a hardware-cropped camera we skip the software ROI/mask
    // entirely and detect on the whole cropped frame ("don't mask the cropped region").
    roiRec_ = std::make_unique<RoiCropRecognizer>(
        *base,
        [](const std::string& gate, int w, int h) {
            try {
                const int id = std::stoi(gate);
                if (SettingsManager::instance().detectionCropNorm(id)) return cv::Rect();  // hardware-cropped -> no software ROI
                return SettingsManager::instance().getGeneralRoi(id, w, h);
            }
            catch (...) { return cv::Rect(); }
        },
        [](const std::string& gate, int w, int h) {
            try {
                const int id = std::stoi(gate);
                if (SettingsManager::instance().detectionCropNorm(id)) return std::vector<cv::Point>{};  // hardware-cropped -> no mask
                return SettingsManager::instance().getCameraPoints(id, w, h);
            }
            catch (...) { return std::vector<cv::Point>{}; }
        });

    topRecognizer_ = roiRec_.get();
    return nullptr;   // chain owned by members; topRecognizer_ points at the head of it
}

PlateProcessorConfig Application::readPlateConfig() {
    auto& sm = SettingsManager::instance();
    PlateProcessorConfig pp;   // capture-all defaults
    pp.minVotes            = sm.getLpr<int>("min_votes").value_or(pp.minVotes);
    pp.similarityThreshold = static_cast<double>(sm.getLpr<float>("plate_similarity").value_or(static_cast<float>(pp.similarityThreshold)));
    pp.minConfidence       = sm.getLpr<float>("ocr_prob").value_or(pp.minConfidence);
    pp.passGapMs           = sm.getLpr<int>("plate_pass_gap_ms").value_or(static_cast<int>(pp.passGapMs));
    pp.maxPlateCharDiffs   = sm.getLpr<int>("plate_max_char_diffs").value_or(pp.maxPlateCharDiffs);
    pp.diag = [this](std::string j) { if (transport_) transport_->publish("messages.module_diag", j); };
    return pp;
}

DirectionEstimator::Config Application::readDirectionConfig() {
    auto& sm = SettingsManager::instance();
    DirectionEstimator::Config d;
    d.minSightings      = std::max(2, sm.getLpr<int>("direction_min_sightings").value_or(3));
    d.minGrowthRatio    = std::max(1.01, (double)sm.getLpr<float>("direction_min_growth").value_or(1.15f));
    d.cooldownMs        = (long)sm.getLpr<int>("direction_cooldown_sec").value_or(8) * 1000;
    d.trackGapMs        = (long)sm.getLpr<int>("direction_gap_sec").value_or(3) * 1000;
    d.requireYAgree     = (sm.getLpr<int>("direction_require_y").value_or(0) == 1);
    d.trendMinSightings = std::max(3, sm.getLpr<int>("direction_trend_min_sightings").value_or(6));
    d.minTrendDeltaPx   = (double)sm.getLpr<float>("direction_trend_delta_px").value_or(5.0f);
    d.peakMinSpread     = std::max(1.0, (double)sm.getLpr<float>("direction_peak_min_spread").value_or(1.08f));
    d.peakBias          = std::clamp((double)sm.getLpr<float>("direction_peak_bias").value_or(0.15f), 0.0, 0.49);
    d.confirmSightings  = std::max(sm.getLpr<int>("direction_min_sightings").value_or(3),
                                   sm.getLpr<int>("direction_confirm_sightings").value_or(8));
    d.confirmAgreeing   = std::max(2, sm.getLpr<int>("direction_confirm_agreeing").value_or(3));
    // Exit-strictness + announce-hold (reduce wrong «خروج» and «نامشخص»). Defaults are
    // tuned for a mostly one-directional (approaching) gate; 1.0 / 0 restore legacy symmetry.
    // Default SYMMETRIC (1.0 / 0): safe for a bidirectional gate (parking, two-way) where
    // real exits are common. A one-directional road raises these (via settings) to ~1.3 / ~5.
    d.recedingBias         = std::max(1.0, (double)sm.getLpr<float>("direction_receding_bias").value_or(1.0f));
    d.recedingMinSightings = std::max(0, sm.getLpr<int>("direction_receding_min_sightings").value_or(0));
    d.announceHoldSightings= std::max(0, sm.getLpr<int>("direction_announce_hold").value_or(3));
    return d;
}

// Apply the plate + direction + camera TUNING to the running pipeline without a rebuild, so a
// settings save takes effect immediately. Hardware camera settings (exposure/GigE/AOI/trigger)
// apply via a targeted per-camera reconnect; only models, camera address/link-type and queue
// geometry still need a full restart.
void Application::reapplyLiveSettings() {
    if (processor_) processor_->setConfig(readPlateConfig());
    if (worker_)    worker_->setDirectionConfig(readDirectionConfig());
    if (cameras_)   cameras_->reapplyCameraSettings();   // motion-gate tuning + hardware reconnect (exposure/GigE/AOI/trigger)
    auto& sm = SettingsManager::instance();
    LOGI() << "Application: settings applied LIVE — plate(minVotes=" << sm.getLpr<int>("min_votes").value_or(1)
           << " sim=" << sm.getLpr<float>("plate_similarity").value_or(0.9f)
           << " maxDiffs=" << sm.getLpr<int>("plate_max_char_diffs").value_or(2)
           << ") direction(minSightings=" << sm.getLpr<int>("direction_min_sightings").value_or(3)
           << " growth=" << sm.getLpr<float>("direction_min_growth").value_or(1.15f)
           << " recedingBias=" << sm.getLpr<float>("direction_receding_bias").value_or(1.0f)
           << " recedingMinSight=" << sm.getLpr<int>("direction_receding_min_sightings").value_or(0)
           << " announceHold=" << sm.getLpr<int>("direction_announce_hold").value_or(3)
           << ") + camera motion-gate + hardware exposure/GigE/AOI/trigger (reconnect where changed)."
           << " Only models, camera address/link-type and queue geometry still need a restart.";
}

void Application::bootstrapFromJson(const json& settingsBody) {
    std::lock_guard<std::mutex> lk(bootMtx_);
    if (booted_.load()) {
        // Already running: don't rebuild the whole pipeline — apply the tuning LIVE so
        // an LPR/camera settings save takes effect immediately on plate-reading + direction.
        LOGI() << "Application: settings update received -> applying live (no rebuild)";
        SettingsManager::instance().loadAll(settingsBody);
        reapplyLiveSettings();
        return;
    }
    booted_.store(true);
    LOGI() << "Application: bootstrapping from settings";
    if (!transport_) transport_ = std::make_unique<InMemoryTransport>();   // offline: no start()
    SettingsManager::instance().loadAll(settingsBody);

    // Build recognizer chain (loads models with the configured backend).
    buildRecognizerChain();

    // Queues + pipeline.
    // Queue capacity from BufferSize (original: per-camera drop-stale ring). The queue is shared
    // across cameras, so use the largest BufferSize x camera count, with a sensible floor.
    {
        auto& sm = SettingsManager::instance();
        int maxBuf = 0, ncam = 0;
        for (int id : sm.getCameraIds()) {
            maxBuf = std::max(maxBuf, sm.getCameraSettingByIdAndKey<int>(id, "BufferSize").value_or(10));
            ++ncam;
        }
        if (maxBuf <= 0) maxBuf = 10;
        if (ncam   <= 0) ncam = 1;
        std::size_t cap = static_cast<std::size_t>(std::max(32, maxBuf * ncam));
        frames_ = std::make_shared<FrameQueue>(cap);
        LOGI() << "Application: frame queue capacity=" << cap << " (BufferSize=" << maxBuf << " x " << ncam << " cameras)";
    }
    // capture-all defaults + live-tunable knobs (min_votes/plate_similarity/…); the
    // diag hook publishes notable OCR-consensus overrides as module-diag events.
    PlateProcessorConfig ppcfg = readPlateConfig();
    processor_ = std::make_unique<PlateProcessor>(ppcfg);
    LOGI() << "Application: plate processor minVotes=" << ppcfg.minVotes
           << " similarity=" << ppcfg.similarityThreshold << " minConf=" << ppcfg.minConfidence
           << " passGapMs=" << ppcfg.passGapMs << " (send-once-per-pass, best read)";
    // Recording size/quality knobs. Defaults favor small files (CRF 30); all overridable from
    // settings without recompiling: recording_crf, recording_preset, recording_fps,
    // recording_width, recording_height.
    RecordingService::Config recCfg;
    {
        auto& sm = SettingsManager::instance();
        recCfg.crf = sm.getLpr<int>("recording_crf").value_or(recCfg.crf);
        recCfg.fps = sm.getLpr<float>("recording_fps").value_or((float)recCfg.fps);
        int rw = sm.getLpr<int>("recording_width").value_or(recCfg.frameSize.width);
        int rh = sm.getLpr<int>("recording_height").value_or(recCfg.frameSize.height);
        if (rw > 0 && rh > 0) recCfg.frameSize = cv::Size(rw, rh);
        // Absolute, well-known recordings root (mirrors the log location) so files never depend on
        // the launch directory; overridable via `recording_dir`.
        {
#if defined(_WIN32)
            const char* pd = std::getenv("PROGRAMDATA");
            std::filesystem::path base = pd ? std::filesystem::path(pd) : std::filesystem::path("C:\\ProgramData");
            recCfg.baseDir = (base / "Hoshyar" / "ALPR" / "recordings").string();
#else
            recCfg.baseDir = "/var/lib/Hoshyar/ALPR/recordings";
#endif
            const std::string dir = sm.getLpr<std::string>("recording_dir").value_or("");
            if (!dir.empty()) recCfg.baseDir = dir;
        }
        // Disk retention (RecordingService prunes on a timer): age + total-size cap + free floor.
        recCfg.retentionDays = sm.getLpr<int>("recording_retention_days").value_or(recCfg.retentionDays);
        const int maxGb  = sm.getLpr<int>("recording_max_gb")
                               .value_or(int(recCfg.maxTotalBytes / (1024LL * 1024 * 1024)));
        const int freeGb = sm.getLpr<int>("recording_min_free_gb")
                               .value_or(int(recCfg.minFreeBytes / (1024LL * 1024 * 1024)));
        recCfg.maxTotalBytes = (long long)std::max(0, maxGb)  * 1024 * 1024 * 1024;
        recCfg.minFreeBytes  = (long long)std::max(0, freeGb) * 1024 * 1024 * 1024;
        LOGI() << "Application: recording " << recCfg.frameSize.width << "x" << recCfg.frameSize.height
               << " @" << recCfg.fps << "fps crf=" << recCfg.crf << " (" << recCfg.x264Preset << ")"
               << " dir=" << recCfg.baseDir << " retentionDays=" << recCfg.retentionDays
               << " maxGB=" << maxGb << " minFreeGB=" << freeGb;
    }
    recording_ = std::make_unique<RecordingService>(recCfg);
    live_      = std::make_unique<LiveViewService>();
    MediaSender::Config mcfg;
    {   // manual-control live image size is tunable from settings (no recompile):
        auto& sm = SettingsManager::instance();
        mcfg.manualLiveJpegQuality = sm.getLpr<int>("manual_live_quality").value_or(mcfg.manualLiveJpegQuality);
        mcfg.manualLiveScaleDiv    = std::max(1, sm.getLpr<int>("manual_live_scale").value_or(mcfg.manualLiveScaleDiv));
        // manual_live_base64: 1 = base64 (smaller, default), 0 = JSON byte-array (matches normal live)
        if (sm.getLpr<int>("manual_live_base64").value_or(1) == 0)
            mcfg.manualLiveEncoding = ImageEncoding::ByteArray;
    }
    media_     = std::make_unique<MediaSender>(*transport_, mcfg);
    sender_    = std::make_unique<PlateSender>(*transport_);
    notifier_  = std::make_unique<CameraStatusNotifier>(*transport_);
    router_    = std::make_unique<CommandRouter>(*recording_, *live_);

    // Heartbeat / resources (interval from settings; lpr_id from the token by default).
    HeartbeatMonitor::Config hb;
    hb.lprId = !clientId_.empty() ? clientId_ : (opts_.natsToken.empty() ? "lpr-client" : opts_.natsToken);
    // heartbeat_interval in the settings is in SECONDS (original); convert to milliseconds.
    {
        int hbSec = SettingsManager::instance().getLpr<int>("heartbeat_interval").value_or(20);
        if (hbSec <= 0) hbSec = 20;
        hb.intervalMs = std::max(1000, hbSec * 1000);
    }
    heartbeat_ = std::make_unique<HeartbeatMonitor>(*transport_, hb);
    // Real CPU/RAM/disk metrics (was a zeroed stub). One sampler kept across calls so CPU can
    // compute a delta between successive resource sends.
    {
        auto mon = std::make_shared<SystemResourceMonitor>();
        heartbeat_->setResourceProvider([mon]() { return mon->sample(); });
    }

    // Connect services to the broker.
    recording_->setSegmentCompleteCallback(media_->recordingCallback());
    live_->setLiveSink(media_->liveSink());

    // Command subjects are handled by the NATS handshake (command.<clientId>); the router
    // is invoked from onCommand. set_config handler also reloads settings.
    router_->setGetSettingHandler([](const json&) { /* re-bootstrap hook (not implemented) */ });
    router_->setSetCameraConfigHandler([](const json& m) {
        try { SettingsManager::instance().loadAll(m); } catch (...) {}
    });

    // Detection worker: frames -> recognizer -> processor -> sink, with frame observers.
    DetectionWorker::Config dwcfg;
    dwcfg.showLive  = (SettingsManager::instance().getLpr<int>("show_live").value_or(0) == 1);
    dwcfg.liveScale = std::max(1, SettingsManager::instance().getLpr<int>("live_scale").value_or(1));
    dwcfg.annotateEvidence = (SettingsManager::instance().getLpr<int>("evidence_overlay").value_or(1) == 1);
    {
        auto& sm = SettingsManager::instance();
        dwcfg.directionEnable = (sm.getLpr<int>("direction_enable").value_or(1) == 1);
        // All direction-estimator tuning (min sightings/growth, slow-vehicle trend,
        // peak-position fallback, confirm counts) — shared with the live re-apply path.
        dwcfg.direction = readDirectionConfig();
        // Same-pass plate similarity: OCR flicker within one pass keeps its passId
        // (so the backend merges early + correction); a clearly different plate starts
        // a new pass. Reuses the OCR consensus threshold, floored a little lower.
        dwcfg.passPlateSimilarity      = std::min(0.95, (double)sm.getLpr<float>("plate_similarity").value_or(0.90f) - 0.05);
        // Keep the DetectionWorker's pass-gap IN SYNC with the estimator's track gap, so a
        // quiet gap resets BOTH the passId state and the direction track together (otherwise
        // one restarts while the other keeps stale state -> merged/flipped passages).
        dwcfg.passGapMs                = dwcfg.direction.trackGapMs;
        dwcfg.approachingIsEnter = [](const std::string& gate) -> bool {
            try {
                return SettingsManager::instance()
                           .getCameraSettingByIdAndKey<int>(std::stoi(gate), "Entry_Exit")
                           .value_or(0) == 0;
            } catch (...) { return true; }
        };
        if (dwcfg.directionEnable)
            LOGI() << "Application: direction (enter/exit) ON minSightings=" << dwcfg.direction.minSightings
                   << " growth=" << dwcfg.direction.minGrowthRatio;
    }
    if (dwcfg.showLive) LOGI() << "Application: show_live enabled (preview window per camera, scale=" << dwcfg.liveScale << ")";
    worker_ = std::make_unique<DetectionWorker>(frames_, *topRecognizer_, dwcfg);
    // Shared overlay: the detection thread publishes the latest vehicle/plate/count annotations,
    // and the live path (below) stamps them onto every outgoing live frame.
    overlay_ = std::make_unique<LiveOverlay>();
    worker_->setLiveOverlay(overlay_.get());
    if (dwcfg.showLive)
        worker_->setRoiPolygonProvider([](const std::string& gate, int w, int h) {
            try { return SettingsManager::instance().getCameraPoints(std::stoi(gate), w, h); }
            catch (...) { return std::vector<cv::Point>{}; }
        });
    worker_->setPlateProcessor(processor_->asProcessor());
    // Publish notable direction FLIPS as module-diag events (best-effort).
    worker_->setDiagSink([this](std::string j) { if (transport_) transport_->publish("messages.module_diag", j); });
    worker_->setPlateSink(sender_->asSink());
    worker_->addFrameObserver([this](const FrameItem& f) {
        // While recording, burn the PC date/time + latest plate boxes into the recorded frame
        // (same overlay as the live view + evidence). When not recording, onFrame is a no-op,
        // so skip the draw entirely.
        if (recording_ && recording_->isRecording(f.gate) && overlay_)
            recording_->onFrame(f.gate, overlay_->draw(f.gate, f.image, /*withTime=*/true));
        else
            recording_->onFrame(f.gate, f.image);
    });
    // Live view is fed continuously from the capture stage (see CameraManager raw observer below),
    // not from the motion-gated detection path, matching the original's independent live thread.

    // Cameras: status callback BEFORE building (workers capture it at add time).
    cameras_ = std::make_unique<CameraManager>();
    cameras_->setOutputQueue(frames_);
    cameras_->setStatusCallback(notifier_->asCallback());
    cameras_->setRawFrameObserver([this](FrameItem&& f) {
        // Keep the latest frame per camera so a "screenshot" command can return it on demand.
        { std::lock_guard<std::mutex> lk(lastFrameMtx_); lastFrame_[f.gate] = f.image; }
        // If this gate is under manual control, send ONE "live_manual_control" message with
        // both sensors + current exposure/gain (throttled), and suppress the normal live.
        bool manualGate = false, sendNow = false;
        {
            std::lock_guard<std::mutex> lk(manualLiveMtx_);
            auto it = manualLive_.find(f.gate);
            if (it != manualLive_.end()) {
                if (std::chrono::steady_clock::now() >= it->second.expireAt) {
                    manualLive_.erase(it);                       // expired -> back to normal live
                    if (media_) media_->clearLiveSerial(f.gate);
                } else {
                    manualGate = true;
                    sendNow = (++it->second.counter % 2 == 0);   // ~every other frame
                }
            }
        }
        if (manualGate) { if (sendNow) publishManualLive(f); return; }
        // Continuous live feed from the capture stage, with the latest detection boxes stamped on.
        live_->onFrame(f.gate, overlay_ ? overlay_->draw(f.gate, f.image) : f.image);
    });

    // On the first frame of each camera, send the full crude image so the backend can let
    // the operator draw the ROI polygon (plate reading region) — matches the original
    // send_crud_file / "crud_image" on subject message.crud.
    cameras_->setFirstFrameObserver([this](const std::string& gate, const cv::Mat& fullFrame, bool colorVariant) {
        LOGI() << "Application: first frame for camera " << gate << " (" << fullFrame.cols << "x"
               << fullFrame.rows << ") " << (colorVariant ? "COLOUR" : "mono") << "; sending crud_image";
        // colorVariant => the pair's colour-evidence reference (role="color") for the colour-crop
        // editor; else the (mono) detection reference for the ROI/zone editor.
        if (media_) media_->sendCrudeImage(gate, fullFrame, colorVariant ? "color" : "");
        else        LOGW() << "Application: no media sender; crud_image not sent for camera " << gate;
    });
    cameras_->buildFromSettings();

    // Start everything.
    sender_->start();
    worker_->start();
    heartbeat_->start();
    cameras_->start();
    LOGI() << "Application: running with " << cameras_->cameraCount() << " camera(s)";
}

void Application::run() {
    running_ = true;
    while (running_) std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void Application::stop() {
    running_ = false;
    // Stop in reverse order of the data flow; each join is safe/idempotent.
    if (cameras_)  cameras_->stop();
    if (worker_)   worker_->stop();
    if (heartbeat_) heartbeat_->stop();
    if (sender_)   sender_->stop();
    // Destroy the transport now (closes NATS subscriptions) BEFORE the command/settings
    // handlers that captured `this` are torn down, so no late callback can fire into them.
    transport_.reset();
    // recording_/live_/media_/notifier_/router_/recognizers torn down by member destruction.
}

} // namespace lpr
