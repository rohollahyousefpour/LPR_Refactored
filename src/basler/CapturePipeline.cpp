#include "CapturePipeline.h"
#include "AppLogger.h"
#include <opencv2/core.hpp>
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
