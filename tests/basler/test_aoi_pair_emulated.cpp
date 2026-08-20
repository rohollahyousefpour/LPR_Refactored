// test_aoi_pair_emulated
// ----------------------
// The TWO-CAMERA case: one colour + one mono camera sharing a single camera_id
// (as a BaslerCamera pair does). Verifies BOTH crops at once against two Pylon
// emulated cameras:
//   * MONO plate camera  -> ALWAYS cropped; here mode=roi => bounding box of the
//     ROI polygon (the auto "from the plate region" behaviour).
//   * COLOUR camera      -> optional; cropped because rgb_crop_enable=1 (manual rect).
// Both go through the real settings -> computeAoiCrop -> applyRoi chain.

#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <string>
#include <pylon/PylonIncludes.h>
#include <nlohmann/json.hpp>

#include "BaslerCamera.h"
#include "ConnectionSupervisor.h"
#include "CameraContext.h"
#include "lpr/config/SettingsManager.h"

using nlohmann::json;

struct BaslerAoiTestAccess {
    static void compute(const BaslerCamera& c, ConnectionSupervisor::Profile& p, bool mono) {
        c.computeAoiCrop(p, mono);
    }
};

static int g_fail = 0;
#define CHECK(cond, msg) do { if(!(cond)){ std::printf("  [FAIL] %s\n", msg); ++g_fail; } \
                              else { std::printf("  [ok]   %s\n", msg); } } while(0)
static int64_t alignDown(int64_t v,int64_t mn,int64_t inc){int64_t a=inc>0?mn+((v-mn)/inc)*inc:v;return a<mn?mn:a;}
static int64_t clampi(int64_t v,int64_t lo,int64_t hi){return v<lo?lo:(v>hi?hi:v);}
static bool near0(double a,double b){return std::fabs(a-b)<1e-4;}

struct Caps { int64_t Wmax,Hmax,Wmin,Hmin,Winc,Hinc,OXmin,OYmin,OXinc,OYinc,defW,defH,defPayload; double bpp; };
struct Applied { bool connected=false; int64_t W=0,H=0,X=0,Y=0,payload=0; };

static bool readCaps(const std::string& serial, Caps& s){
    CameraContext ctx; ConnectionSupervisor sup(ctx);
    ConnectionSupervisor::Profile p; p.serial=serial; p.freeRun=true; sup.addProfile(p);
    if(!sup.connect(serial)) return false;
    std::lock_guard<std::mutex> lk(ctx.devMutex);
    auto* c=ctx.devices[serial]->raw();
    s.defW=c->Width.GetValue(); s.defH=c->Height.GetValue();
    s.defPayload=c->PayloadSize.IsReadable()?c->PayloadSize.GetValue():0;
    s.bpp=(s.defW>0&&s.defH>0)?(double)s.defPayload/((double)s.defW*s.defH):1.0;
    if(c->OffsetX.IsWritable()) c->OffsetX.SetValue(c->OffsetX.GetMin());
    if(c->OffsetY.IsWritable()) c->OffsetY.SetValue(c->OffsetY.GetMin());
    s.Wmax=c->Width.GetMax(); s.Hmax=c->Height.GetMax();
    s.Wmin=c->Width.GetMin(); s.Hmin=c->Height.GetMin();
    s.Winc=c->Width.GetInc(); s.Hinc=c->Height.GetInc();
    s.OXmin=c->OffsetX.GetMin(); s.OYmin=c->OffsetY.GetMin();
    s.OXinc=c->OffsetX.GetInc(); s.OYinc=c->OffsetY.GetInc();
    return true;
}
static Applied applyToEmu(const std::string& serial, ConnectionSupervisor::Profile p){
    Applied r; CameraContext ctx; ConnectionSupervisor sup(ctx);
    p.serial=serial; p.freeRun=true; sup.addProfile(p);
    r.connected=sup.connect(serial); if(!r.connected) return r;
    std::lock_guard<std::mutex> lk(ctx.devMutex);
    auto* c=ctx.devices[serial]->raw();
    r.W=c->Width.GetValue(); r.H=c->Height.GetValue();
    r.X=c->OffsetX.GetValue(); r.Y=c->OffsetY.GetValue();
    r.payload=c->PayloadSize.IsReadable()?c->PayloadSize.GetValue():0;
    return r;
}
static void expectAoi(const Caps& s,double xn,double yn,double wn,double hn,
                      int64_t& eW,int64_t& eH,int64_t& eX,int64_t& eY){
    eW=clampi(alignDown((int64_t)std::llround(wn*s.Wmax),s.Wmin,s.Winc),s.Wmin,s.Wmax);
    eH=clampi(alignDown((int64_t)std::llround(hn*s.Hmax),s.Hmin,s.Hinc),s.Hmin,s.Hmax);
    eX=clampi(alignDown((int64_t)std::llround(xn*s.Wmax),s.OXmin,s.OXinc),s.OXmin,s.Wmax-eW);
    eY=clampi(alignDown((int64_t)std::llround(yn*s.Hmax),s.OYmin,s.OYinc),s.OYmin,s.Hmax-eH);
}

int main(){
#ifdef _WIN32
    _putenv_s("PYLON_CAMEMU","2");
#else
    setenv("PYLON_CAMEMU","2",1);
#endif
    Pylon::PylonInitialize();
    int rc=0;
    try{
        Pylon::DeviceInfoList_t devs;
        Pylon::CTlFactory::GetInstance().EnumerateDevices(devs);
        CHECK(devs.size()>=2,"two virtual cameras available for a pair");
        if(devs.size()<2){ Pylon::PylonTerminate(); return 1; }
        const std::string colorSerial=devs[0].GetSerialNumber().c_str();
        const std::string monoSerial =devs[1].GetSerialNumber().c_str();
        std::printf("Pair: colour=%s  mono=%s\n", colorSerial.c_str(), monoSerial.c_str());

        Caps caps{}; CHECK(readCaps(colorSerial,caps),"read sensor caps");
        std::printf("Sensor %lldx%lld, bpp=%.2f, full-sensor frame=%lld B\n",
                    (long long)caps.Wmax,(long long)caps.Hmax,caps.bpp,
                    (long long)std::llround((double)caps.Wmax*caps.Hmax*caps.bpp));

        const int CID=7;   // ONE camera_id shared by the colour+mono pair
        // ROI polygon (used by the mono roi-mode auto crop) -> bbox (0.20,0.35,0.60,0.30).
        // Colour manual rect (rgb_crop_*) -> (0.30,0.45,0.40,0.12).
        json settings = json::array({
            json{{"name","type_of_link"},{"value","pylon"}},
            json{{"name","CameraAddress"},{"value",colorSerial}},
            json{{"name","MonoCameraAddress"},{"value",monoSerial}},
            json{{"name","mono_crop_mode"},{"value","roi"}},
            json{{"name","rgb_crop_enable"},{"value",1}},
            json{{"name","rgb_crop_mode"}, {"value","manual"}},
            json{{"name","rgb_crop_x"},{"value",0.30}}, json{{"name","rgb_crop_y"},{"value",0.45}},
            json{{"name","rgb_crop_w"},{"value",0.40}}, json{{"name","rgb_crop_h"},{"value",0.12}},
        });
        json points = json::array({ json::array({0.20,0.35}), json::array({0.80,0.35}),
                                    json::array({0.80,0.65}), json::array({0.20,0.65}) });
        json body = {{"settings",json::array()},
                     {"cameras_data",json::array({ json{{"camera_id",CID},{"settings",settings},{"points",points}} })}};
        lpr::SettingsManager::instance().loadAll(body);

        {
            BaslerCamera cam(0,0,0,0,"7");

            // ---- MONO plate camera: always cropped, mode=roi => ROI-polygon bbox ----
            std::printf("\n== MONO (always crop, roi = ROI-polygon bbox) ==\n");
            ConnectionSupervisor::Profile pm;
            BaslerAoiTestAccess::compute(cam, pm, /*mono*/true);
            std::printf("  computeAoiCrop(mono) -> norm(%.3f,%.3f,%.3f,%.3f)\n", pm.cropXn,pm.cropYn,pm.cropWn,pm.cropHn);
            CHECK(near0(pm.cropXn,0.20)&&near0(pm.cropYn,0.35)&&near0(pm.cropWn,0.60)&&near0(pm.cropHn,0.30),
                  "mono crop = ROI-polygon bounding box (auto)");
            Applied am=applyToEmu(monoSerial,pm);
            int64_t mW,mH,mX,mY; expectAoi(caps,0.20,0.35,0.60,0.30,mW,mH,mX,mY);
            CHECK(am.connected,"mono connected");
            CHECK(am.W==mW&&am.H==mH&&am.X==mX&&am.Y==mY,"mono hardware AOI matches ROI bbox (pixel-exact)");
            std::printf("  mono hardware: %lldx%lld @ (%lld,%lld) payload=%lld B (%.1f%% of sensor)\n",
                        (long long)am.W,(long long)am.H,(long long)am.X,(long long)am.Y,(long long)am.payload,
                        100.0*am.payload/((double)caps.Wmax*caps.Hmax*caps.bpp));

            // ---- COLOUR camera: optional, enabled -> manual rect ----
            std::printf("\n== COLOUR (optional, enabled -> manual rect) ==\n");
            ConnectionSupervisor::Profile pc;
            BaslerAoiTestAccess::compute(cam, pc, /*mono*/false);
            std::printf("  computeAoiCrop(colour) -> norm(%.3f,%.3f,%.3f,%.3f)\n", pc.cropXn,pc.cropYn,pc.cropWn,pc.cropHn);
            CHECK(near0(pc.cropXn,0.30)&&near0(pc.cropYn,0.45)&&near0(pc.cropWn,0.40)&&near0(pc.cropHn,0.12),
                  "colour crop = manual rgb_crop_* rect");
            Applied ac=applyToEmu(colorSerial,pc);
            int64_t cW,cH,cX,cY; expectAoi(caps,0.30,0.45,0.40,0.12,cW,cH,cX,cY);
            CHECK(ac.connected,"colour connected");
            CHECK(ac.W==cW&&ac.H==cH&&ac.X==cX&&ac.Y==cY,"colour hardware AOI matches manual rect (pixel-exact)");
            std::printf("  colour hardware: %lldx%lld @ (%lld,%lld) payload=%lld B (%.1f%% of sensor)\n",
                        (long long)ac.W,(long long)ac.H,(long long)ac.X,(long long)ac.Y,(long long)ac.payload,
                        100.0*ac.payload/((double)caps.Wmax*caps.Hmax*caps.bpp));

            // Both members of the pair are simultaneously cropped -> the pair's total GigE
            // load is the sum of the two shrunken frames, not two full sensors.
            const double fullPair = 2.0*caps.Wmax*caps.Hmax*caps.bpp;
            const double cropPair = (double)am.payload + ac.payload;
            CHECK(cropPair < fullPair, "pair's combined frame bytes < two full sensors (real bandwidth cut)");
            std::printf("\n  PAIR bandwidth: %.0f -> %.0f B/frame (%.1f%% of two full sensors)\n",
                        fullPair, cropPair, 100.0*cropPair/fullPair);

            // The software-ROI guard: detection runs on the MONO, which is hardware-cropped, so
            // detectionCropNorm must report that crop -> the pipeline skips the software ROI/mask
            // (otherwise the full-sensor-normalized polygon re-applied on the cropped frame would
            // shrink it and cut plates in half).
            std::printf("\n== software-ROI guard (detectionCropNorm) ==\n");
            auto dc = lpr::SettingsManager::instance().detectionCropNorm(CID);
            CHECK(dc.has_value(), "detectionCropNorm reports a crop for the pylon pair -> software ROI/mask skipped");
            if (dc) CHECK(near0(dc->x,0.20)&&near0(dc->y,0.35)&&near0(dc->width,0.60)&&near0(dc->height,0.30),
                          "detectionCropNorm == mono ROI bbox (same rect the sensor was cropped to)");

            // ---- Hybrid #1: safety margin grows the crop (drift tolerance) ----
            std::printf("\n== safety margin (aoi_crop_margin=20%%) ==\n");
            {
                json sm = settings; sm.push_back(json{{"name","aoi_crop_margin"},{"value",20}});
                json b = {{"settings",json::array()},{"cameras_data",json::array({ json{{"camera_id",CID},{"settings",sm},{"points",points}} })}};
                lpr::SettingsManager::instance().loadAll(b);
                auto rc = lpr::SettingsManager::instance().aoiCropNorm(CID, false);   // colour manual (0.3,0.45,0.4,0.12)
                CHECK(rc.has_value(), "colour crop still present with margin");
                if (rc) CHECK(near0(rc->x,0.26)&&near0(rc->y,0.438)&&near0(rc->width,0.48)&&near0(rc->height,0.144),
                              "20%% margin: 0.40->0.48 w, 0.12->0.144 h, centred (x 0.30->0.26)");
                ConnectionSupervisor::Profile pmar; BaslerAoiTestAccess::compute(cam, pmar, /*mono*/false);
                Applied am2 = applyToEmu(colorSerial, pmar);
                int64_t eW,eH,eX,eY; expectAoi(caps,0.26,0.438,0.48,0.144,eW,eH,eX,eY);
                CHECK(am2.W==eW && am2.H==eH, "hardware AOI grew with the margin (wider than the un-margined 1638)");
                std::printf("  colour hardware WITH margin: %lldx%lld @ (%lld,%lld)\n",
                            (long long)am2.W,(long long)am2.H,(long long)am2.X,(long long)am2.Y);
            }

            // ---- Hybrid #2: config mode un-crops for setup (full sensor) ----
            std::printf("\n== config mode (aoi_config_mode=1 -> full sensor) ==\n");
            {
                json sc = settings; sc.push_back(json{{"name","aoi_config_mode"},{"value",1}});
                json b = {{"settings",json::array()},{"cameras_data",json::array({ json{{"camera_id",CID},{"settings",sc},{"points",points}} })}};
                lpr::SettingsManager::instance().loadAll(b);
                CHECK(!lpr::SettingsManager::instance().detectionCropNorm(CID).has_value(), "config mode -> detectionCropNorm nullopt (software ROI restored)");
                CHECK(!lpr::SettingsManager::instance().aoiCropNorm(CID, true).has_value(), "config mode -> mono full frame too");
                ConnectionSupervisor::Profile pc; BaslerAoiTestAccess::compute(cam, pc, /*mono*/true);
                CHECK(pc.cropWn==0.0 && pc.cropHn==0.0, "computeAoiCrop -> full frame under config mode");
                Applied ac2 = applyToEmu(monoSerial, pc);
                CHECK(ac2.W==caps.defW && ac2.H==caps.defH, "hardware stays FULL sensor under config mode (snapshot shows whole scene)");
                std::printf("  mono hardware UNDER config mode: %lldx%lld (power-on/full, no crop)\n",
                            (long long)ac2.W,(long long)ac2.H);
            }
        }

        // Negative: a NON-pylon camera never reports a hardware crop, so the software ROI stays on.
        {
            json b2 = {{"settings",json::array()},{"cameras_data",json::array({
                json{{"camera_id",9},
                     {"settings",json::array({ json{{"name","type_of_link"},{"value","rtsp"}} })},
                     {"points",json::array({ json::array({0.2,0.2}), json::array({0.8,0.2}), json::array({0.8,0.8}) })}} })}};
            lpr::SettingsManager::instance().loadAll(b2);
            CHECK(!lpr::SettingsManager::instance().detectionCropNorm(9).has_value(),
                  "non-pylon camera -> no hardware crop -> software ROI kept (unchanged behaviour)");
        }
    }
    catch(const Pylon::GenericException& e){ std::printf("[FAIL] pylon: %s\n",e.GetDescription()); rc=2; ++g_fail; }
    Pylon::PylonTerminate();
    std::printf("\n==== %s (%d failure(s)) ====\n", g_fail==0?"ALL PAIR-AOI TESTS PASSED":"PAIR-AOI TESTS FAILED", g_fail);
    return (g_fail==0&&rc==0)?0:1;
}
