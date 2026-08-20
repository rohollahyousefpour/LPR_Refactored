// emu_grab: open the first Pylon emulated camera, optionally point it at a directory of
// custom images (CamEmu ImageFilename + ImageFileMode=On), grab a frame, save it as PNG.
//   emu_grab <out.png> [imageDir]
#include <cstdlib>
#include <cstdio>
#include <string>
#include <pylon/PylonIncludes.h>
#include <GenApi/GenApi.h>
#include <opencv2/imgcodecs.hpp>
#include "CameraDevice.h"

int main(int argc, char** argv) {
#ifdef _WIN32
    _putenv_s("PYLON_CAMEMU", "1");
#else
    setenv("PYLON_CAMEMU", "1", 1);
#endif
    const char* out = argc > 1 ? argv[1] : "emu_frame.png";
    std::string dir = argc > 2 ? argv[2] : "";
    Pylon::PylonInitialize();
    int rc = 0;
    try {
        Pylon::IPylonDevice* dev = Pylon::CTlFactory::GetInstance().CreateFirstDevice();
        auto device = std::make_unique<CameraDevice>(dev, "emu");
        if (!device->open()) { std::printf("open failed\n"); Pylon::PylonTerminate(); return 1; }

        if (!dir.empty()) {
            GenApi::INodeMap& nm = device->raw()->GetNodeMap();
            GenApi::CStringPtr fn = nm.GetNode("ImageFilename");
            GenApi::CEnumerationPtr mode = nm.GetNode("ImageFileMode");
            if (fn.IsValid() && GenApi::IsWritable(fn)) { fn->SetValue(dir.c_str()); std::printf("ImageFilename set: %s\n", dir.c_str()); }
            else std::printf("ImageFilename node NOT available/writable\n");
            if (mode.IsValid() && GenApi::IsWritable(mode)) { mode->FromString("On"); std::printf("ImageFileMode=On\n"); }
            else std::printf("ImageFileMode node NOT available/writable\n");
        }

        device->startGrabbing();
        cv::Mat frame; bool isColor = false;
        for (int i = 0; i < 5; ++i) device->retrieveBGR(frame, isColor, 2000);
        if (frame.empty()) { std::printf("empty frame\n"); rc = 2; }
        else { cv::imwrite(out, frame); std::printf("saved %s  %dx%d  color=%d\n", out, frame.cols, frame.rows, (int)isColor); }
        device->stopGrabbing();
    } catch (const Pylon::GenericException& e) {
        std::printf("pylon exception: %s\n", e.GetDescription()); rc = 3;
    }
    Pylon::PylonTerminate();
    return rc;
}
