#include "lpr/util/CapturePathBuilder.h"
#include "lpr/util/Time.h"
#include "lpr/util/Uuid.h"

#include <filesystem>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace lpr {

CapturePathBuilder::CapturePathBuilder(std::string basePath)
    : basePath_(std::move(basePath)) {}

CapturePaths CapturePathBuilder::build(const std::string& gate) const {
    const std::string datePath = formatLocalTime("%Y/%m/%d");   // zero-padded, sortable
    const std::string name     = generateUuidV4() + ".jpg";

    const fs::path fullDir  = fs::path(basePath_) / "full"  / gate / datePath;
    const fs::path plateDir = fs::path(basePath_) / "plate" / gate / datePath;

    std::error_code ec;
    fs::create_directories(fullDir, ec);
    if (ec) throw std::runtime_error("Failed to create directory: " + fullDir.string());
    fs::create_directories(plateDir, ec);
    if (ec) throw std::runtime_error("Failed to create directory: " + plateDir.string());

    CapturePaths p;
    p.fullImage  = (fullDir  / name).string();
    p.plateImage = (plateDir / name).string();
    p.relative   = gate + "/" + datePath + "/" + name;
    return p;
}

} // namespace lpr
