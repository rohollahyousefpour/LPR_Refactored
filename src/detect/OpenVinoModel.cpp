#ifdef LPR_WITH_OPENVINO
#include "lpr/detect/OpenVinoModel.h"
#include "lpr/Log.h"

#include <openvino/core/preprocess/pre_post_process.hpp>
#include <cstring>

namespace lpr {

bool OpenVinoModel::load(const std::string& modelPath, const std::string& device) {
    try {
        std::shared_ptr<ov::Model> model = core_.read_model(modelPath);

        if (fmt_ == InputFormat::NhwcU8) {
            // Match the original (Ocr_Model / Detection_plate): feed raw HWC uint8 BGR and let the
            // model normalize internally. ppp.input().tensor() = u8 NHWC; model layout = NHWC.
            ov::preprocess::PrePostProcessor ppp(model);
            ppp.input().tensor().set_element_type(ov::element::u8).set_layout("NHWC");
            ppp.input().model().set_layout("NHWC");
            ppp.output().tensor().set_element_type(ov::element::f32);
            model = ppp.build();
        }

        compiled_ = core_.compile_model(model, device.empty() ? "CPU" : device);
        request_  = compiled_.create_infer_request();
        ready_    = true;
        LOGI() << "OpenVinoModel: loaded '" << modelPath << "' on " << device
               << " (" << compiled_.outputs().size() << " outputs, "
               << (fmt_ == InputFormat::NhwcU8 ? "NHWC/u8" : "NCHW/f32") << ")";
        return true;
    } catch (const std::exception& e) {
        LOGE() << "OpenVinoModel: failed to load '" << modelPath << "': " << e.what();
        return false;
    }
}

std::vector<cv::Mat> OpenVinoModel::infer(const cv::Mat& blob) {
    if (!ready_) return {};

    ov::Tensor input;
    if (fmt_ == InputFormat::NhwcU8) {
        // blob is a raw HWC CV_8U image (rows=H, cols=W, channels=C). Shape {1,H,W,C} u8.
        CV_Assert(blob.depth() == CV_8U);
        cv::Mat hwc = blob.isContinuous() ? blob : blob.clone();
        ov::Shape inShape{ 1, static_cast<size_t>(hwc.rows),
                              static_cast<size_t>(hwc.cols),
                              static_cast<size_t>(hwc.channels()) };
        input = ov::Tensor(ov::element::u8, inShape, hwc.data);
        request_.set_input_tensor(input);
        request_.infer();
    } else {
        // Contiguous NCHW float blob (cv::dnn::blobFromImage).
        ov::Shape inShape;
        for (int i = 0; i < blob.dims; ++i)
            inShape.push_back(static_cast<size_t>(blob.size[i]));
        input = ov::Tensor(ov::element::f32, inShape, const_cast<float*>(blob.ptr<float>()));
        request_.set_input_tensor(input);
        request_.infer();
    }

    std::vector<cv::Mat> outs;
    for (size_t i = 0; i < compiled_.outputs().size(); ++i) {
        ov::Tensor ot = request_.get_output_tensor(i);
        ov::Shape sh  = ot.get_shape();
        std::vector<int> dims;
        for (auto d : sh) dims.push_back(static_cast<int>(d));
        cv::Mat m(static_cast<int>(dims.size()), dims.data(), CV_32F);
        std::memcpy(m.data, ot.data<float>(), ot.get_byte_size());
        outs.push_back(m);
    }
    return outs;
}

} // namespace lpr
#endif // LPR_WITH_OPENVINO
