#include "lpr/basler/BaslerFactory.h"
#include "BaslerCamera.h"   // the vendored facade (global namespace, IS-A virtual_cap_url/CaptureSource)

namespace lpr {

std::unique_ptr<CaptureSource> makeBaslerFacade(const CameraSourceParams& p) {
    // ROI (x,y,w,h) + gate carry into the facade; the RGB serial is applied later via setAddress(),
    // and the mono serial (if any) via setMonoAddress() - the facade pairs them internally.
    auto cam = std::make_unique<BaslerCamera>(p.roi.x, p.roi.y, p.roi.width, p.roi.height, p.gate);
    return cam;   // upcast unique_ptr<BaslerCamera> -> unique_ptr<lpr::CaptureSource>
}

} // namespace lpr
