#pragma once
// CtcDecoder - backend-agnostic OCR post-processing (greedy CTC). Pure STL/algorithm,
// so it runs identically under OpenVINO, TensorRT, or OpenCV-DNN, on x86 or ARM.
#include <string>
#include <vector>

namespace lpr {

// data: row-major [timesteps x numClasses] logits. numClasses == alphabet.size()+1
// (one extra class is the CTC blank, at blankIndex). Collapses repeats, drops blanks.
// confidence (optional) = geometric-mean softmax prob of the kept characters.
std::string ctcGreedyDecode(const std::vector<float>& data, int timesteps, int numClasses,
                            const std::string& alphabet, int blankIndex, double* confidence = nullptr);

} // namespace lpr
