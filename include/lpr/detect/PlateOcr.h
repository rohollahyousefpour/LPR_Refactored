#pragma once
// PlateOcr - reads the characters off a cropped plate image. Backend-agnostic:
// it owns an InferModel (any engine) and runs the shared CTC decoder on the output.
#include "lpr/detect/InferModel.h"
#include <opencv2/core.hpp>
#include <memory>
#include <string>

namespace lpr {

class PlateOcr {
public:
    struct Config {
        Backend     backend   = Backend::Auto;
        std::string device    = "CPU";
        // Original Ocr_Model char_set for multi_language=0 (38 chars). The model emits 38+1 classes;
        // the CTC blank is the EXTRA last class (num_classes-1), and output index i -> alphabet[i].
        std::string alphabet  = " 0123456789abcdefghijlkmnoqstvwypuzxDS";
        bool        blankLast = true;               // blank = last class (num_classes-1)
        int         blankIndex = 0;                 // used only when blankLast == false
        cv::Size    inputSize  = cv::Size(184, 48); // original SetInputDims(184,48) (W x H)
        double      scale      = 1.0;               // raw u8 (model normalizes via PPP); no /255
        bool        swapRB     = false;             // original feeds BGR as-is (no cvtColor)
        bool        nhwcU8     = true;              // feed NHWC u8 like the original (OpenVINO PPP)
    };

    bool load(const std::string& modelPath, const Config& cfg);

    // Returns recognized text; sets confidence. Empty string if not loaded / no output.
    std::string read(const cv::Mat& plateBgr, double& confidence) const;

private:
    std::unique_ptr<InferModel> model_;
    Config                      cfg_;
    bool                        ready_ = false;
};

} // namespace lpr
