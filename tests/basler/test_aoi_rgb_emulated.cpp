// test_aoi_rgb_emulated
// ---------------------
// End-to-end demonstration of the COLOUR-camera AOI path against Pylon camera
// emulation (PYLON_CAMEMU), covering the piece test_aoi_emulated skipped: the
// settings-driven BaslerCamera::computeAoiCrop() that decides whether/where a
// colour camera crops.
//
// Chain exercised (all real production code, no physical camera):
//   camera_settings JSON  --loadAll-->  SettingsManager
//        --computeAoiCrop(mono=false)-->  normalized crop in the Profile
//        --ConnectionSupervisor::connect--> applyRoi --> Basler Width/Height/Offset
//
// A single colour camera is registered mono=false, so this is exactly the
// "one colour camera" case: NO crop by default; cropped only when rgb_crop_enable=1
// (manual rect or the ROI-polygon bounding box).

#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <pylon/PylonIncludes.h>
#include <nlohmann/json.hpp>

#include "BaslerCamera.h"
#include "ConnectionSupervisor.h"
#include "CameraContext.h"
#include "lpr/config/SettingsManager.h"

using nlohmann::json;

// Test seam declared as a friend in BaslerCamera.h -> reach the private crop calc.
struct BaslerAoiTestAccess {
    static void compute(const BaslerCamera& c, ConnectionSupervisor::Profile& p, bool mono) {
        c.computeAoiCrop(p, mono);
    }
};

static int g_fail = 0;
#define CHECK(cond, msg) do { if(!(cond)){ std::printf("  [FAIL] %s\n", msg); ++g_fail; } \
                              else { std::printf("  [ok]   %s\n", msg); } } while(0)

static int64_t alignDown(int64_t v, int64_t mn, int64_t inc) {
    const int64_t a = inc > 0 ? mn + ((v - mn) / inc) * inc : v;
    return a < mn ? mn : a;
}
static int64_t clampi(int64_t v, int64_t lo, int64_t hi) { return v<lo?lo:(v>hi?hi:v); }
static bool near0(double a, double b) { return std::fabs(a-b) < 1e-4; }

// Push one camera's settings (+ optional ROI polygon) into the real SettingsManager.
static void loadCameraSettings(int camId, const json& settings, const json& points) {
    json body = {
        {"settings", json::array()},   // LPR-level (unused here)
        {"cameras_data", json::array({
            json{{"camera_id", camId}, {"settings", settings}, {"points", points}}
        })}
    };
    lpr::SettingsManager::instance().loadAll(body);
}

struct SensorCaps { int64_t Wmax,Hmax,Wmin,Hmin,Winc,Hinc,OXmin,OYmin,OXinc,OYinc,defW,defH,defPayload; double bpp; };
struct Applied { bool connected=false; int64_t W=0,H=0,X=0,Y=0,payload=0; };

static bool readCaps(const std::string& serial, SensorCaps& s) {
    CameraContext ctx; ConnectionSupervisor sup(ctx);
    ConnectionSupervisor::Profile p; p.serial=serial; p.freeRun=true; sup.addProfile(p);
    if (!sup.connect(serial)) return false;
    std::lock_guard<std::mutex> lk(ctx.devMutex);
    auto* c = ctx.devices[serial]->raw();
    s.defW=c->Width.GetValue(); s.defH=c->Height.GetValue();
    s.defPayload=c->PayloadSize.IsReadable()?c->PayloadSize.GetValue():0;
    s.bpp = (s.defW>0&&s.defH>0)?(double)s.defPayload/((double)s.defW*s.defH):1.0;
    if (c->OffsetX.IsWritable()) c->OffsetX.SetValue(c->OffsetX.GetMin());
    if (c->OffsetY.IsWritable()) c->OffsetY.SetValue(c->OffsetY.GetMin());
    s.Wmax=c->Width.GetMax(); s.Hmax=c->Height.GetMax();
    s.Wmin=c->Width.GetMin(); s.Hmin=c->Height.GetMin();
    s.Winc=c->Width.GetInc(); s.Hinc=c->Height.GetInc();
    s.OXmin=c->OffsetX.GetMin(); s.OYmin=c->OffsetY.GetMin();
    s.OXinc=c->OffsetX.GetInc(); s.OYinc=c->OffsetY.GetInc();
    return true;
}

// Apply a (crop-filled) profile to the emulated camera and read back the AOI.
static Applied applyToEmu(const std::string& serial, ConnectionSupervisor::Profile p) {
    Applied r;
    CameraContext ctx; ConnectionSupervisor sup(ctx);
    p.serial=serial; p.freeRun=true;
    sup.addProfile(p);
    r.connected = sup.connect(serial);
    if (!r.connected) return r;
    std::lock_guard<std::mutex> lk(ctx.devMutex);
    auto* c = ctx.devices[serial]->raw();
    r.W=c->Width.GetValue(); r.H=c->Height.GetValue();
    r.X=c->OffsetX.GetValue(); r.Y=c->OffsetY.GetValue();
    r.payload=c->PayloadSize.IsReadable()?c->PayloadSize.GetValue():0;
    return r;
}

int main() {
#ifdef _WIN32
    _putenv_s("PYLON_CAMEMU", "2");
#else
    setenv("PYLON_CAMEMU", "2", 1);
#endif
    Pylon::PylonInitialize();
    int rc = 0;
    try {
        Pylon::DeviceInfoList_t devs;
        Pylon::CTlFactory::GetInstance().EnumerateDevices(devs);
        CHECK(devs.size() >= 1, "virtual camera available");
        if (devs.empty()) { Pylon::PylonTerminate(); return 1; }
        const std::string serial = devs[0].GetSerialNumber().c_str();

        SensorCaps caps{};
        CHECK(readCaps(serial, caps), "read sensor caps from virtual camera");
        std::printf("Virtual camera %s: sensor %lldx%lld, power-on %lldx%lld, bpp=%.2f\n",
                    serial.c_str(),(long long)caps.Wmax,(long long)caps.Hmax,
                    (long long)caps.defW,(long long)caps.defH,caps.bpp);

        const int CID = 7;   // this camera's id (facade gate = "7")
        // Build ONE colour-camera facade (mono=false). PylonInitialize is ref-counted;
        // keep it in an inner scope so its dtor's PylonTerminate runs before ours.
        {
            BaslerCamera cam(0,0,0,0,"7");

            // ---- Case A: single colour camera, crop DISABLED (default) ----
            std::printf("\n== A) colour cam, rgb_crop_enable=0 (default) ==\n");
            loadCameraSettings(CID, json::array({ json{{"name","rgb_crop_enable"},{"value",0}} }), json::array());
            {
                ConnectionSupervisor::Profile p;
                BaslerAoiTestAccess::compute(cam, p, /*mono*/false);
                std::printf("  computeAoiCrop -> crop norm(%.3f,%.3f,%.3f,%.3f)\n", p.cropXn,p.cropYn,p.cropWn,p.cropHn);
                CHECK(p.cropWn==0.0 && p.cropHn==0.0, "no crop computed (disabled -> full frame)");
                Applied a = applyToEmu(serial, p);
                CHECK(a.connected, "connected through supervisor");
                CHECK(a.W==caps.defW && a.H==caps.defH, "hardware stays full-frame (no AOI applied)");
                std::printf("  hardware: %lldx%lld @ (%lld,%lld) payload=%lld B\n",
                            (long long)a.W,(long long)a.H,(long long)a.X,(long long)a.Y,(long long)a.payload);
            }

            // ---- Case B: crop ENABLED, manual rectangle ----
            std::printf("\n== B) colour cam, rgb_crop_enable=1, mode=manual, rect(0.30,0.45,0.40,0.12) ==\n");
            loadCameraSettings(CID, json::array({
                json{{"name","rgb_crop_enable"},{"value",1}},
                json{{"name","rgb_crop_mode"},  {"value","manual"}},
                json{{"name","rgb_crop_x"},{"value",0.30}}, json{{"name","rgb_crop_y"},{"value",0.45}},
                json{{"name","rgb_crop_w"},{"value",0.40}}, json{{"name","rgb_crop_h"},{"value",0.12}},
            }), json::array());
            {
                ConnectionSupervisor::Profile p;
                BaslerAoiTestAccess::compute(cam, p, /*mono*/false);
                std::printf("  computeAoiCrop -> crop norm(%.3f,%.3f,%.3f,%.3f)\n", p.cropXn,p.cropYn,p.cropWn,p.cropHn);
                CHECK(near0(p.cropXn,0.30)&&near0(p.cropYn,0.45)&&near0(p.cropWn,0.40)&&near0(p.cropHn,0.12),
                      "manual rect read from rgb_crop_* settings");
                Applied a = applyToEmu(serial, p);
                int64_t eW=clampi(alignDown((int64_t)std::llround(0.40*caps.Wmax),caps.Wmin,caps.Winc),caps.Wmin,caps.Wmax);
                int64_t eH=clampi(alignDown((int64_t)std::llround(0.12*caps.Hmax),caps.Hmin,caps.Hinc),caps.Hmin,caps.Hmax);
                int64_t eX=clampi(alignDown((int64_t)std::llround(0.30*caps.Wmax),caps.OXmin,caps.OXinc),caps.OXmin,caps.Wmax-eW);
                int64_t eY=clampi(alignDown((int64_t)std::llround(0.45*caps.Hmax),caps.OYmin,caps.OYinc),caps.OYmin,caps.Hmax-eH);
                CHECK(a.connected, "connected through supervisor");
                CHECK(a.W==eW&&a.H==eH&&a.X==eX&&a.Y==eY, "hardware AOI matches the manual rect (pixel-exact)");
                CHECK(a.payload>0 && a.payload<caps.defPayload*4 && a.payload==(int64_t)std::llround((double)a.W*a.H*caps.bpp),
                      "PayloadSize == cropped W*H*bpp (bandwidth cut)");
                std::printf("  hardware: %lldx%lld @ (%lld,%lld) payload=%lld B (%.1f%% of full sensor)\n",
                            (long long)a.W,(long long)a.H,(long long)a.X,(long long)a.Y,(long long)a.payload,
                            100.0*a.payload/((double)caps.Wmax*caps.Hmax*caps.bpp));
            }

            // ---- Case C: crop ENABLED, mode=roi (bbox of the ROI polygon) ----
            std::printf("\n== C) colour cam, rgb_crop_enable=1, mode=roi, polygon bbox(0.25,0.40,0.50,0.20) ==\n");
            loadCameraSettings(CID, json::array({
                json{{"name","rgb_crop_enable"},{"value",1}},
                json{{"name","rgb_crop_mode"},  {"value","roi"}},
            }), json::array({ json::array({0.25,0.40}), json::array({0.75,0.40}),
                              json::array({0.75,0.60}), json::array({0.25,0.60}) }));
            {
                ConnectionSupervisor::Profile p;
                BaslerAoiTestAccess::compute(cam, p, /*mono*/false);
                std::printf("  computeAoiCrop -> crop norm(%.3f,%.3f,%.3f,%.3f)\n", p.cropXn,p.cropYn,p.cropWn,p.cropHn);
                CHECK(near0(p.cropXn,0.25)&&near0(p.cropYn,0.40)&&near0(p.cropWn,0.50)&&near0(p.cropHn,0.20),
                      "crop == bounding box of the ROI polygon");
                Applied a = applyToEmu(serial, p);
                int64_t eW=clampi(alignDown((int64_t)std::llround(0.50*caps.Wmax),caps.Wmin,caps.Winc),caps.Wmin,caps.Wmax);
                int64_t eH=clampi(alignDown((int64_t)std::llround(0.20*caps.Hmax),caps.Hmin,caps.Hinc),caps.Hmin,caps.Hmax);
                int64_t eX=clampi(alignDown((int64_t)std::llround(0.25*caps.Wmax),caps.OXmin,caps.OXinc),caps.OXmin,caps.Wmax-eW);
                int64_t eY=clampi(alignDown((int64_t)std::llround(0.40*caps.Hmax),caps.OYmin,caps.OYinc),caps.OYmin,caps.Hmax-eH);
                CHECK(a.connected, "connected through supervisor");
                CHECK(a.W==eW&&a.H==eH&&a.X==eX&&a.Y==eY, "hardware AOI matches the ROI bbox (pixel-exact)");
                std::printf("  hardware: %lldx%lld @ (%lld,%lld) payload=%lld B (%.1f%% of full sensor)\n",
                            (long long)a.W,(long long)a.H,(long long)a.X,(long long)a.Y,(long long)a.payload,
                            100.0*a.payload/((double)caps.Wmax*caps.Hmax*caps.bpp));
            }
        }   // cam dtor -> PylonTerminate (ref-count back to our own init)
    }
    catch (const Pylon::GenericException& e) {
        std::printf("[FAIL] pylon exception: %s\n", e.GetDescription()); rc=2; ++g_fail;
    }
    Pylon::PylonTerminate();
    std::printf("\n==== %s (%d failure(s)) ====\n", g_fail==0?"ALL RGB-AOI TESTS PASSED":"RGB-AOI TESTS FAILED", g_fail);
    return (g_fail==0 && rc==0) ? 0 : 1;
}
