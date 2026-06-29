#include "lpr/detect/InferModel.h"
#include <cassert>
#include <iostream>

int main() {
    using namespace lpr;

    // OpenCV-DNN is always compiled in -> must construct and self-report.
    auto dnn = makeInferModel(Backend::OpenCvDnn);
    assert(dnn && dnn->backend() == Backend::OpenCvDnn);
    std::cout << "dnn backend: " << toString(dnn->backend()) << "\n";

    // Auto must always yield *some* backend.
    auto autoModel = makeInferModel(Backend::Auto);
    assert(autoModel);
    std::cout << "auto backend: " << toString(autoModel->backend()) << "\n";

    // A backend that wasn't compiled in must throw (rather than silently misbehave).
    auto requireThrowsIfAbsent = [](Backend b) {
        try { auto m = makeInferModel(b); std::cout << toString(b) << ": present\n"; }
        catch (const std::exception& e) { std::cout << toString(b) << ": not compiled (" << e.what() << ")\n"; }
    };
    requireThrowsIfAbsent(Backend::OpenVino);
    requireThrowsIfAbsent(Backend::TensorRt);
    requireThrowsIfAbsent(Backend::OnnxRuntime);
    requireThrowsIfAbsent(Backend::Hailo);

    assert(parseBackend("trt") == Backend::TensorRt);
    assert(parseBackend("vino") == Backend::OpenVino);
    assert(parseBackend("cpu") == Backend::OpenCvDnn);
    assert(parseBackend("onnx") == Backend::OnnxRuntime);
    assert(parseBackend("npu") == Backend::Hailo);

    std::cout << "infer_factory: OK\n";
    return 0;
}
