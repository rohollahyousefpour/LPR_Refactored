#pragma once
// LiveOverlay - a small thread-safe hand-off so the live stream (fed continuously from the
// capture stage) can show the latest detection annotations (vehicle boxes, plate boxes, count)
// produced on the separate detection thread. The DetectionWorker publish()es the newest
// annotations per gate; the live path calls draw() to stamp them onto each outgoing frame.
#include "lpr/detect/IPlateRecognizer.h"   // VehicleAnnotation
#include "lpr/detect/PlateResult.h"
#include "lpr/util/Time.h"                  // formatLocalTime

#include <opencv2/imgproc.hpp>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace lpr {

class LiveOverlay {
public:
    struct Plate { cv::RotatedRect box; std::string text; cv::Mat crop; };

    void publish(const std::string& gate,
                 std::vector<VehicleAnnotation> vehicles,
                 std::vector<Plate> plates,
                 int vehicleCount) {
        std::lock_guard<std::mutex> lk(mtx_);
        Entry& e = byGate_[gate];
        e.vehicles = std::move(vehicles);
        e.plates   = std::move(plates);
        e.count    = vehicleCount;
    }

    // Returns an annotated COPY of frame (or the frame itself if nothing to draw for this gate).
    cv::Mat draw(const std::string& gate, const cv::Mat& frame, bool withTime = false) const {
        Entry e;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = byGate_.find(gate);
            if (it != byGate_.end()) e = it->second;
        }
        if (frame.empty()) return frame;
        const bool nothingToDraw = e.vehicles.empty() && e.plates.empty() && e.count < 0;
        if (nothingToDraw && !withTime) return frame;

        cv::Mat canvas = frame.clone();
        const cv::Rect fr(0, 0, canvas.cols, canvas.rows);
        for (const VehicleAnnotation& v : e.vehicles) {
            cv::Rect b = v.box & fr;
            if (b.width < 2 || b.height < 2) continue;
            cv::rectangle(canvas, b, cv::Scalar(255, 200, 0), 2);
            if (v.trackId >= 0) {
                const std::string id = "#" + std::to_string(v.trackId);
                cv::putText(canvas, id, {b.x, std::max(0, b.y) + 18}, cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 0), 4);
                cv::putText(canvas, id, {b.x, std::max(0, b.y) + 18}, cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(255, 200, 0), 2);
            }
        }
        for (const Plate& p : e.plates) {
            cv::Point2f pts[4]; p.box.points(pts);
            for (int i = 0; i < 4; ++i) cv::line(canvas, pts[i], pts[(i + 1) % 4], cv::Scalar(0, 255, 0), 2);
            cv::Point org(static_cast<int>(p.box.center.x - p.box.size.width / 2),
                          static_cast<int>(p.box.center.y - p.box.size.height / 2) - 6);
            cv::putText(canvas, p.text, org, cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 0, 0), 4);
            cv::putText(canvas, p.text, org, cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 255), 2);
        }
        // Larger time/labels, scaled to frame width so they stay legible after the recorder
        // downscales the frame.
        const double tscale = std::max(1.0, canvas.cols / 1200.0);
        const int    tth    = std::max(2, (int)std::lround(tscale * 2));
        int topY = (int)std::lround(tscale * 28);
        if (withTime) {   // PC date/time, top-left
            const std::string ts = formatLocalTime("%Y-%m-%d %H:%M:%S");
            cv::putText(canvas, ts, {12, topY}, cv::FONT_HERSHEY_SIMPLEX, tscale, cv::Scalar(0, 0, 0), tth * 2);
            cv::putText(canvas, ts, {12, topY}, cv::FONT_HERSHEY_SIMPLEX, tscale, cv::Scalar(255, 255, 255), tth);
            topY += (int)std::lround(tscale * 34);
        }
        if (e.count >= 0) {
            const std::string label = "Vehicles: " + std::to_string(e.count);
            cv::putText(canvas, label, {12, topY}, cv::FONT_HERSHEY_SIMPLEX, tscale, cv::Scalar(0, 0, 0), tth * 2);
            cv::putText(canvas, label, {12, topY}, cv::FONT_HERSHEY_SIMPLEX, tscale, cv::Scalar(0, 255, 0), tth);
        }
        // Plate close-up thumbnail(s), top-right (same as the evidence image).
        if (withTime) {
            int insetY = 12;
            const int insetH = 70, margin = 12;
            for (const Plate& p : e.plates) {
                if (p.crop.empty() || insetY + insetH + 4 >= canvas.rows) continue;
                cv::Mat thumb = p.crop;
                if (thumb.channels() == 1) cv::cvtColor(thumb, thumb, cv::COLOR_GRAY2BGR);
                const int w = std::max(1, (int)std::lround(thumb.cols * (double)insetH / thumb.rows));
                cv::Mat resized; cv::resize(thumb, resized, cv::Size(w, insetH));
                const int x = std::max(0, canvas.cols - w - margin);
                cv::Rect dst(x, insetY, std::min(w, canvas.cols - x), insetH);
                resized(cv::Rect(0, 0, dst.width, dst.height)).copyTo(canvas(dst));
                cv::rectangle(canvas, dst, cv::Scalar(0, 255, 255), 2);
                insetY += insetH + 8;
            }
        }
        return canvas;
    }

private:
    struct Entry {
        std::vector<VehicleAnnotation> vehicles;
        std::vector<Plate>             plates;
        int                            count = -1;
    };
    mutable std::mutex          mtx_;
    std::map<std::string, Entry> byGate_;
};

} // namespace lpr
