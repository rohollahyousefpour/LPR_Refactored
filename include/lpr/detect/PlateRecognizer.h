#pragma once
// PlateRecognizer - the full pipeline: detect plates (EAST) -> crop/rectify -> OCR.
// Backend-agnostic (each stage owns an InferModel), so it runs on any engine.
#include "lpr/detect/IPlateRecognizer.h"
#include "lpr/detect/EastTextDetector.h"
#include "lpr/detect/PlateOcr.h"
#include "lpr/detect/PlateResult.h"
#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace lpr {

class PlateRecognizer : public IPlateRecognizer {
public:
    bool load(const std::string& detectorModel, const std::string& ocrModel,
              Backend backend = Backend::Auto, const std::string& device = "CPU",
              const EastConfig& eastCfg = {}, const PlateOcr::Config& ocrCfg = {});

    // Detect + read every plate in the frame.
    std::vector<PlateResult> recognize(const cv::Mat& frame, const std::string& gate, long timestamp) override;

    EastTextDetector& detector() { return detector_; }
    PlateOcr&         ocr()      { return ocr_; }
    bool              ready() const { return ready_; }

    // Per-frame diagnostics: log box count + each OCR result (text+conf), even when filtered.
    void setDebugDiagnostics(bool on) { debugDiag_ = on; detector_.config().debugLog = on; }

    // Original-parity filters (all from settings): drop OCR below ocr_prob, and boxes
    // smaller than plate_width x plate_height (in image pixels). 0 disables a filter.
    void setOcrProb(float p)            { ocrProb_ = p; }
    void setMinPlateSize(int w, int h)  { plateMinW_ = w; plateMinH_ = h; }

private:
    EastTextDetector detector_;
    PlateOcr         ocr_;
    bool             ready_ = false;
    bool             debugDiag_ = false;
    float            ocrProb_  = 0.0f;   // min OCR confidence (Alpr_vino ocr_prob)
    int              plateMinW_ = 0;     // min box width  (plate_width)
    int              plateMinH_ = 0;     // min box height (plate_height)
};

} // namespace lpr
