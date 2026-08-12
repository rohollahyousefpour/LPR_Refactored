#include "CapturePipeline.h"
#include "AppLogger.h"
#include "SettingsManager_.h"
#include "lpr/basler/GigeRateAdapt.h"
#include <opencv2/core.hpp>

using lpr::GigeAdaptCfg;
using lpr::GigeAdaptDecision;
using lpr::GigeAdaptAction;
using lpr::gigeAdaptDecide;
#include <algorithm>
#include <ctime>
#include <thread>
#include <chrono>
#include <vector>
#include <unordered_map>

CapturePipeline::CapturePipeline(CameraContext& ctx,
                                 std::function<void(const std::string&)> onFault)
    : ctx_(ctx), onFault_(std::move(onFault)) {}

void CapturePipeline::stop() { ctx_.live = false; }

void CapturePipeline::run() {
    ctx_.live = true;

    // Resolver for commands: lock, find, return raw ptr. Safe because device
    // removal happens only on this (capture) thread -- see CameraContext invariant.
    const CameraResolver resolver = [this](const std::string& serial) -> CameraDevice* {
        std::lock_guard<std::mutex> lk(ctx_.devMutex);
        auto it = ctx_.devices.find(serial);
        return (it == ctx_.devices.end() || !it->second) ? nullptr : it->second.get();
    };

    struct Target { std::string serial; CameraDevice* dev; IExposureStrategy* exp; };

    // Consecutive non-Ok grabs per camera. A single timeout/bad frame is
    // tolerated; only sustained failure escalates to a reconnect, so a momentary
    // hiccup doesn't tear down and rebuild a healthy camera.
    std::unordered_map<std::string, int> consecutiveFails;
    constexpr int kFailThreshold = 3;

    // ── Live GigE rate adaptation ────────────────────────────────────────────
    // Real closed-loop control (beyond the static optimize-at-connect): watch the
    // incomplete-frame / timeout (packet-loss) rate over a sliding window and, when
    // the network degrades, throttle the camera's AcquisitionFrameRate DOWN in place
    // (no reconnect); when the link is clean again, ease it back up toward the
    // connect-time cap. Triggered slaves are skipped (their rate is the master's
    // trigger). All knobs are settings-tunable.
    struct Adapt { double ceil = 0, cur = 0; int grabs = 0, bad = 0; long lastStepMs = 0;
                   bool inited = false, skip = false; };
    std::unordered_map<std::string, Adapt> adapt;
    bool adaptEnabled = true; int adaptWindow = 150; GigeAdaptCfg acfg;
    {
        auto& sm = SettingsManager::instance();
        adaptEnabled = sm.getLpr<int>("gige_adapt_enable").value_or(1) != 0;
        adaptWindow  = std::max(20, sm.getLpr<int>("gige_adapt_window").value_or(150));
        acfg.downPct   = (double)sm.getLpr<float>("gige_adapt_loss_pct").value_or(2.0f);
        acfg.upPct     = (double)sm.getLpr<float>("gige_adapt_recover_loss_pct").value_or(0.2f);
        acfg.downF     = (double)sm.getLpr<float>("gige_adapt_down_factor").value_or(0.75f);
        acfg.upF       = (double)sm.getLpr<float>("gige_adapt_up_factor").value_or(1.15f);
        acfg.minFps    = (double)sm.getLpr<float>("gige_adapt_min_fps").value_or(1.0f);
        acfg.recoverMs = (long)sm.getLpr<int>("gige_adapt_recover_sec").value_or(15) * 1000;
        LOGI() << "[bw-adapt] " << (adaptEnabled ? "enabled" : "disabled")
               << " (window=" << adaptWindow << " down>=" << acfg.downPct << "% floor=" << acfg.minFps
               << "fps recover=" << (acfg.recoverMs / 1000) << "s)";
    }
    const auto nowMs = [] {
        return (long)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    auto adaptRate = [&](const std::string& serial, CameraDevice& dev, CameraDevice::GrabStatus st) {
        if (!adaptEnabled) return;
        Adapt& a = adapt[serial];
        if (!a.inited) {
            a.inited = true;
            a.cur = a.ceil = dev.acquisitionFrameRate();
            // A triggered slave's rate is driven by the master's trigger, not
            // AcquisitionFrameRate — skip it (and never touch its rate-limit node).
            try {
                auto* c = dev.raw();
                a.skip = (a.ceil <= 0.0) || (c && c->TriggerMode.IsReadable() &&
                          c->TriggerMode.GetValue() == Basler_UniversalCameraParams::TriggerMode_On);
            } catch (...) { a.skip = (a.ceil <= 0.0); }
        }
        if (a.skip) return;
        ++a.grabs;
        if (st == CameraDevice::GrabStatus::BadFrame || st == CameraDevice::GrabStatus::Timeout) ++a.bad;
        if (a.grabs < adaptWindow) return;
        const double lossPct = 100.0 * (double)a.bad / (double)a.grabs;
        const long now = nowMs();
        const GigeAdaptDecision d = gigeAdaptDecide(a.cur, a.ceil, a.lastStepMs, lossPct, now, acfg);
        if (d.action != GigeAdaptAction::None && dev.setAcquisitionFrameRate(d.newFps)) {
            if (d.action == GigeAdaptAction::Throttle)
                LOGW() << "[bw-adapt][" << serial << "] packet loss " << lossPct << "% over "
                       << a.grabs << " frames -> THROTTLE fps " << a.cur << " -> " << d.newFps;
            else
                LOGI() << "[bw-adapt][" << serial << "] link clean (" << lossPct << "% loss) -> RECOVER fps "
                       << a.cur << " -> " << d.newFps;
            a.cur = d.newFps; a.lastStepMs = now;
        }
        a.grabs = 0; a.bad = 0;   // reset the window
    };

    LOGI() << "[capture] loop start (expected=" << ctx_.expectedCount << ")";

    while (ctx_.live) {
        // Whole-iteration guard: a throw anywhere in the body (deliver, command
        // drain, snapshot) must not kill the capture thread silently. Log,
        // signal once, and keep looping.
        try {
            // 1) Apply pending control commands (manual exposure, trigger, etc.)
            ctx_.commands.drain(resolver, ctx_.exposureController);

            // 2) Snapshot connected devices + their exposure strategies under lock.
            std::vector<Target> targets;
            {
                std::lock_guard<std::mutex> lk(ctx_.devMutex);
                for (auto& [serial, dev] : ctx_.devices) {
                    if (!dev) continue;
                    auto eit = ctx_.exposure.find(serial);
                    targets.push_back({serial, dev.get(),
                                       eit != ctx_.exposure.end() ? eit->second.get() : nullptr});
                }
            }

            std::vector<cv::Mat> frames;
            std::vector<int>     isColor;
            const std::time_t ts = std::time(nullptr);

            // 3) Grab one frame per camera.
            for (auto& t : targets) {
                cv::Mat img; bool color = false;
                const auto status = t.dev->retrieveBGR(img, color, ctx_.grabTimeoutMs);

                // Feed the live rate-adaptation controller every grab (Ok or lost).
                adaptRate(t.serial, *t.dev, status);

                if (status != CameraDevice::GrabStatus::Ok) {
                    const int fails = ++consecutiveFails[t.serial];
                    const bool deviceError = (status == CameraDevice::GrabStatus::DeviceError);
                    // Escalate immediately on a device error, or after repeated
                    // transient failures.
                    if (deviceError || fails >= kFailThreshold) {
                        LOGW() << "[capture][" << t.serial << "] grab failed (status="
                               << static_cast<int>(status) << ", fails=" << fails << ") -> fault";
                        consecutiveFails.erase(t.serial);
                        if (onFault_) onFault_(t.serial);   // removes + schedules reconnect
                    } else {
                        LOGI() << "[capture][" << t.serial << "] transient grab miss ("
                               << fails << "/" << kFailThreshold << ")";
                    }
                    continue;
                }
                consecutiveFails.erase(t.serial);   // healthy again

                // Exposure control must never take down the loop -- guard it.
                if (t.exp) {
                    try { t.exp->apply(*t.dev, img); }
                    catch (const cv::Exception& e) { AppLogger::LogCvException(e, "[capture] exposure " + t.serial); }
                    catch (const std::exception& e) { AppLogger::LogException(e, "[capture] exposure " + t.serial); }
                    catch (...) { AppLogger::LogUnknownException("[capture] exposure " + t.serial); }
                }
                frames.push_back(std::move(img));
                isColor.push_back(color ? 1 : 0);
            }

            // 4) Pair + deliver.
            deliver(frames, isColor, ts);
        }
        catch (const cv::Exception& e) {
            AppLogger::LogCvException(e, "[capture] loop body");
        }
        catch (const Pylon::GenericException& e) {
            AppLogger::LogPylonException(e.GetDescription(), "[capture] loop body");
        }
        catch (const std::exception& e) {
            AppLogger::LogException(e, "[capture] loop body");
        }
        catch (...) {
            AppLogger::LogUnknownException("[capture] loop body");
        }

        // 5) Pace the loop.
        std::this_thread::sleep_for(std::chrono::milliseconds(ctx_.delay));
    }

    LOGI() << "[capture] loop exit";
}

void CapturePipeline::deliver(std::vector<cv::Mat>& frames,
                              std::vector<int>& isColor, std::time_t ts) {
    if (!ctx_.sink) return;

    if (frames.size() == 2) {
        // Order mono-first when exactly one is mono (left = mono, right = color).
        if (isColor[0] == 0 && isColor[1] == 1)
            ctx_.sink->onFramePair(frames[0], frames[1], ts);
        else if (isColor[0] == 1 && isColor[1] == 0)
            ctx_.sink->onFramePair(frames[1], frames[0], ts);
        else
            ctx_.sink->onFramePair(frames[0], frames[1], ts); // both same kind
    }
    else if (frames.size() == 1) {
        if (ctx_.expectedCount <= 1)
            ctx_.sink->onFramePair(frames[0], frames[0], ts);  // lone camera: normal
        else
            ctx_.sink->onIncompletePair(frames[0], ts);        // pair, peer missing
    }
    else {
        LOGW() << "[capture] no frames this cycle";
    }
}
