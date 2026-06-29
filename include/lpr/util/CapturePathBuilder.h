#pragma once
// CapturePathBuilder (was PathAndUUIDGenerator) - builds and creates the dated
// directories where a camera's full-frame and plate-crop images are saved, and
// returns fresh uuid-named jpg paths. Rewritten boost-free (std::filesystem +
// std::chrono + lpr::generateUuidV4).
#include <string>

namespace lpr {

struct CapturePaths {
    std::string fullImage;   // absolute path for the full-frame jpg
    std::string plateImage;  // absolute path for the plate-crop jpg
    std::string relative;    // gate/YYYY/MM/DD/uuid.jpg  (relative to base)
};

class CapturePathBuilder {
public:
    explicit CapturePathBuilder(std::string basePath);

    // Creates  <base>/full/<gate>/YYYY/MM/DD  and  <base>/plate/<gate>/YYYY/MM/DD,
    // returns the two file paths (fresh uuid .jpg) plus the relative path.
    // Throws std::runtime_error if a directory cannot be created.
    CapturePaths build(const std::string& gate) const;

private:
    std::string basePath_;
};

} // namespace lpr
