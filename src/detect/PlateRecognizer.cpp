#include "lpr/detect/PlateRecognizer.h"
#include "lpr/Log.h"

namespace lpr {

bool PlateRecognizer::load(const std::string& detectorModel, const std::string& ocrModel,
                           Backend backend, const std::string& device,
                           const EastConfig& eastCfg, const PlateOcr::Config& ocrCfg) {
    bool d = detector_.load(detectorModel, backend, device, eastCfg);

    PlateOcr::Config oc = ocrCfg;
    oc.backend = backend;
    oc.device  = device;
    bool o = ocr_.load(ocrModel, oc);

    ready_ = d && o;
    LOGI() << "PlateRecognizer: " << (ready_ ? "ready" : "NOT ready")
           << " (detector=" << d << ", ocr=" << o << ")";
    return ready_;
}

std::vector<PlateResult> PlateRecognizer::recognize(const cv::Mat& frame,
                                                    const std::string& gate, long timestamp) {
    std::vector<PlateResult> results;
    if (!ready_ || frame.empty()) return results;

    auto boxes = detector_.detect(frame);
    if (debugDiag_)
        LOGI() << "PlateRecognizer[" << gate << "]: detector found " << boxes.size() << " box(es)";

    int idx = 0;
    for (const cv::RotatedRect& box : boxes) {
        // Min plate size (original: skip rot_rect smaller than plate_width x plate_height).
        if ((plateMinW_ > 0 && box.size.width  < plateMinW_) ||
            (plateMinH_ > 0 && box.size.height < plateMinH_)) {
            if (debugDiag_) LOGI() << "PlateRecognizer[" << gate << "]: box " << idx++
                                   << " -> too small (" << box.size.width << "x" << box.size.height << ")";
            continue;
        }

        cv::Mat crop;
        EastTextDetector::fourPointsTransform(frame, box, crop);
        if (crop.empty()) {
            if (debugDiag_) LOGI() << "PlateRecognizer[" << gate << "]: box " << idx++ << " -> empty crop";
            continue;
        }

        double conf = 0.0;
        std::string text = ocr_.read(crop, conf);
        if (debugDiag_)
            LOGI() << "PlateRecognizer[" << gate << "]: box " << idx++ << " crop="
                   << crop.cols << "x" << crop.rows << " ocr='" << text << "' conf=" << conf;
        if (text.empty()) continue;
        // Confidence filter (original Alpr_vino: drop if conf < ocr_prob).
        if (ocrProb_ > 0.0f && static_cast<float>(conf) < ocrProb_) continue;

        PlateResult r;
        r.text       = text;
        r.confidence = static_cast<float>(conf);
        r.box        = box;
        r.plateImage = crop;
        r.timestamp  = timestamp;
        r.gate       = gate;
        results.push_back(std::move(r));
    }
    return results;
}

} // namespace lpr
