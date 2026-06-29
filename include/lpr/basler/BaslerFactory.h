#pragma once
// Bridges the full Basler facade (lpr_basler) to the generic CameraSourceFactory without a
// circular library dependency: lpr_basler defines makeBaslerFacade(); the app registers it via
// CameraSourceFactory::setBaslerCreator(). The returned BaslerCamera is a CaptureSource that
// self-configures trigger/exposure/gain/sync/bandwidth from settings on connect.
#include <memory>
#include "lpr/capture/CameraSourceFactory.h"

namespace lpr {
std::unique_ptr<CaptureSource> makeBaslerFacade(const CameraSourceParams& p);
}
