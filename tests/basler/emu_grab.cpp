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

        {
            // Force a COLOR pixel format so a colour image comes back in colour (the emulated
            // camera defaults to Mono8 -> everything is grey). List what's available, try RGB8.
            GenApi::INodeMap& nm0 = device->raw()->GetNodeMap();
            GenApi::CEnumerationPtr pf = nm0.GetNode("PixelFormat");
            if (pf.IsValid()) {
                GenApi::NodeList_t entries; pf->GetEntries(entries);
                std::printf("PixelFormat options:");
                for (auto* e : entries) { GenApi::CEnumEntryPtr ee(e); if (ee.IsValid() && GenApi::IsAvailable(ee)) std::printf(" %s", ee->GetSymbolic().c_str()); }
                std::printf("\n");
                for (const char* want : {"RGB8", "BGR8", "RGB8Packed", "BayerRG8"}) {
                    GenApi::CEnumEntryPtr ee = pf->GetEntryByName(want);
                    if (ee.IsValid() && GenApi::IsAvailable(ee) && GenApi::IsWritable(pf)) {
                        pf->FromString(want); std::printf("PixelFormat set -> %s\n", want); break;
                    }
                }
            }
        }
        if (!dir.empty()) {
            GenApi::INodeMap& nm = device->raw()->GetNodeMap();
            GenApi::CStringPtr fn = nm.GetNode("ImageFilename");
            GenApi::CEnumerationPtr mode = nm.GetNode("ImageFileMode");
            if (fn.IsValid() && GenApi::IsWritable(fn)) { fn->SetValue(dir.c_str()); std::printf("ImageFilename set: %s\n", dir.c_str()); }
            else std::printf("ImageFilename node NOT available/writable\n");
            if (mode.IsValid() && GenApi::IsWritable(mode)) { mode->FromString("On"); std::printf("ImageFileMode=On\n"); }
            else std::printf("ImageFileMode node NOT available/writable\n");
        }

        // Optional: dump the configured node map as a .pfs feature file (argv[3]) so the module
        // can load it (CamconfigFile setting) and get the SAME colour-image behaviour at connect.
        if (argc > 3) {
            try { Pylon::CFeaturePersistence::Save(argv[3], &device->raw()->GetNodeMap());
                  std::printf("saved pfs: %s\n", argv[3]); }
            catch (const Pylon::GenericException& e) { std::printf("pfs save failed: %s\n", e.GetDescription()); }
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
