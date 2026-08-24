#include "lpr/capture/CameraSourceFactory.h"
#include "lpr/capture/DualCaptureSource.h"
#include "lpr/capture/VideoFileCaptureSource.h"   // OpenCV-only, always available
#ifdef LPR_WITH_VLC
#include "lpr/capture/VlcCaptureSource.h"
#endif
#ifdef LPR_WITH_GSTREAMER
#include "lpr/capture/GStreamerCaptureSource.h"
#endif
#ifdef LPR_WITH_PYLON
#include "lpr/capture/BaslerCaptureSource.h"
#endif

namespace lpr {

namespace { CameraSourceFactory::Creator& baslerCreator() { static CameraSourceFactory::Creator c; return c; } }
void CameraSourceFactory::setBaslerCreator(Creator c) { baslerCreator() = std::move(c); }

std::unique_ptr<CaptureSource> CameraSourceFactory::create(const CameraSourceParams& p) {
    const CameraKind kind = parseCameraKind(p.typeOfLink);

    // Build a bare source of the requested kind (address set by the caller).
    auto makeBare = [&]() -> std::unique_ptr<CaptureSource> {
        switch (kind) {
            case CameraKind::Video:
                return std::make_unique<VideoFileCaptureSource>();
            case CameraKind::Rtsp:
#ifdef LPR_WITH_VLC
                return std::make_unique<VlcCaptureSource>();
#else
                throw CameraSourceError("VLC/rtsp backend not compiled in (build with WITH_VLC)");
#endif
            case CameraKind::Gstreamer:
#ifdef LPR_WITH_GSTREAMER
                return std::make_unique<GStreamerCaptureSource>();
#else
                throw CameraSourceError("GStreamer backend not compiled in (build with WITH_GSTREAMER)");
#endif
            case CameraKind::Pylon:
                // Prefer the full facade (registered by the app); fall back to the minimal grabber.
                if (baslerCreator()) return baslerCreator()(p);
#ifdef LPR_WITH_PYLON
                return std::make_unique<BaslerCaptureSource>(p.roi);
#else
                throw CameraSourceError("Pylon/Basler backend not compiled in (build with WITH_PYLON)");
#endif
            case CameraKind::PointGrey:
                throw CameraSourceError("PointGrey/'grey' backend is not implemented");
            case CameraKind::Unknown:
            default:
                throw CameraSourceError("Unknown type_of_link: '" + p.typeOfLink + "'");
        }
    };

    auto main = makeBare();
    main->setAddress(p.address, p.delayMs);
    main->setCameraId(p.gate);   // so generic sources can attribute faults to this camera

    // Dual mono+RGB.
    if (!p.monoAddress.empty() && p.monoAddress != "-1") {
        // The Basler facade manages the RGB+mono pair internally (master/slave + sync), so we feed
        // both addresses to the SAME source rather than wrapping two in a DualCaptureSource.
        if (kind == CameraKind::Pylon && baslerCreator()) {
            main->setMonoAddress(p.monoAddress, p.delayMs);
            return main;
        }
        auto mono = makeBare();
        mono->setAddress(p.monoAddress, p.delayMs);
        mono->setCameraId(p.gate);
        return std::make_unique<DualCaptureSource>(std::move(main), std::move(mono));
    }
    return main;
}

} // namespace lpr
