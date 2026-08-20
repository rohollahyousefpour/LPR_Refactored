// test_aoi_emulated
// -----------------
// Exercises the REAL hardware-AOI path (ConnectionSupervisor::connect ->
// openConfigureStore -> applyRoi) against Basler's built-in CAMERA EMULATION
// (PYLON_CAMEMU), so the crop math + Basler node ordering (offset-zero-first,
// increment alignment, in-bounds clamp) can be verified with NO physical camera.
//
// The emulated device exposes a real GenICam node map with Width/Height/OffsetX/
// OffsetY/PayloadSize, so after connect() we read the values the camera ACTUALLY
// holds and check:
//   1) they equal what applyRoi should have produced (independent recompute),
//   2) offset+size stays within the sensor (never throws / never out of range),
//   3) PayloadSize really shrinks -> the GigE bandwidth cut the feature promises.
//
// PYLON_CAMEMU must be set BEFORE PylonInitialize(); we set it in main().

#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <pylon/PylonIncludes.h>

#include "ConnectionSupervisor.h"
#include "CameraContext.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { if(!(cond)){ std::printf("  [FAIL] %s\n", msg); ++g_fail; } \
                              else { std::printf("  [ok]   %s\n", msg); } } while(0)

// Same rounding applyRoi uses: round DOWN to a valid increment at/above the min.
static int64_t alignDown(int64_t v, int64_t mn, int64_t inc) {
    const int64_t a = inc > 0 ? mn + ((v - mn) / inc) * inc : v;
    return a < mn ? mn : a;
}
static int64_t clampi(int64_t v, int64_t lo, int64_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

struct CaseResult { bool connected=false; int64_t W=0,H=0,X=0,Y=0,payload=0; };

// Open ONE emulated camera by serial with the given normalized crop, through the
// real supervisor path, and read back what the sensor holds.
static CaseResult runCase(const std::string& serial,
                          double xn, double yn, double wn, double hn) {
    CaseResult r;
    CameraContext ctx;                    // no sink -> onCaptureError is guarded
    ConnectionSupervisor sup(ctx);
    ConnectionSupervisor::Profile p;
    p.serial   = serial;
    p.freeRun  = true;                    // FreeRunConfigurator: trigger off, simplest role
    p.triggerOn= false;
    p.cropXn = xn; p.cropYn = yn; p.cropWn = wn; p.cropHn = hn;
    sup.addProfile(p);

    r.connected = sup.connect(serial);
    if (!r.connected) return r;

    std::lock_guard<std::mutex> lk(ctx.devMutex);
    auto it = ctx.devices.find(serial);
    if (it == ctx.devices.end() || !it->second) { r.connected = false; return r; }
    auto* c = it->second->raw();
    r.W = c->Width.GetValue();
    r.H = c->Height.GetValue();
    r.X = c->OffsetX.GetValue();
    r.Y = c->OffsetY.GetValue();
    r.payload = c->PayloadSize.IsReadable() ? c->PayloadSize.GetValue() : 0;
    return r;
}

// Read the emulated sensor's node ranges (offsets zeroed) so the test can compute
// the expected aligned/clamped AOI independently of applyRoi.
struct SensorCaps {
    int64_t Wmax,Hmax,Wmin,Hmin,Winc,Hinc,OXmin,OYmin,OXinc,OYinc;
    int64_t defaultW,defaultH,defaultPayload;   // the frame the camera powers up with (no crop)
    double  bpp;                                 // bytes/pixel (from default payload)
    int64_t fullSensorBytes;                     // Wmax*Hmax*bpp: the true uncropped-sensor frame
};
static bool readCaps(const std::string& serial, SensorCaps& s) {
    CameraContext ctx; ConnectionSupervisor sup(ctx);
    ConnectionSupervisor::Profile p; p.serial=serial; p.freeRun=true;   // no crop -> full frame
    sup.addProfile(p);
    if (!sup.connect(serial)) return false;
    std::lock_guard<std::mutex> lk(ctx.devMutex);
    auto* c = ctx.devices[serial]->raw();
    // The camera's power-on frame FIRST (this is what applyRoi's full-frame no-op leaves in place).
    s.defaultW = c->Width.GetValue(); s.defaultH = c->Height.GetValue();
    s.defaultPayload = c->PayloadSize.IsReadable() ? c->PayloadSize.GetValue() : 0;
    s.bpp = (s.defaultW>0 && s.defaultH>0) ? (double)s.defaultPayload/((double)s.defaultW*s.defaultH) : 1.0;
    // Then zero offsets to read the true sensor extents/increments.
    if (c->OffsetX.IsWritable()) c->OffsetX.SetValue(c->OffsetX.GetMin());
    if (c->OffsetY.IsWritable()) c->OffsetY.SetValue(c->OffsetY.GetMin());
    s.Wmax=c->Width.GetMax(); s.Hmax=c->Height.GetMax();
    s.Wmin=c->Width.GetMin(); s.Hmin=c->Height.GetMin();
    s.Winc=c->Width.GetInc(); s.Hinc=c->Height.GetInc();
    s.OXmin=c->OffsetX.GetMin(); s.OYmin=c->OffsetY.GetMin();
    s.OXinc=c->OffsetX.GetInc(); s.OYinc=c->OffsetY.GetInc();
    s.fullSensorBytes = (int64_t)std::llround((double)s.Wmax*s.Hmax*s.bpp);
    return true;
}

int main() {
    // Camera emulation MUST be requested before Pylon initializes its transport layers.
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
        std::vector<std::string> serials;
        std::printf("Enumerated %u device(s):\n", (unsigned)devs.size());
        for (auto& d : devs) {
            std::string s = d.GetSerialNumber().c_str();
            std::printf("  - serial=%-14s model=%s tl=%s\n",
                        s.c_str(), d.GetModelName().c_str(), d.GetDeviceClass().c_str());
            serials.push_back(s);
        }
        CHECK(serials.size() >= 2, "PYLON_CAMEMU=2 created two virtual cameras");
        if (serials.size() < 2) { Pylon::PylonTerminate(); return 1; }

        SensorCaps caps{};
        CHECK(readCaps(serials[0], caps), "connected to a virtual camera and read sensor caps");
        std::printf("Sensor: %lldx%lld (Wmin=%lld inc=%lld, Hmin=%lld inc=%lld) bpp=%.2f\n"
                    "  power-on frame=%lldx%lld (%lld B); full-sensor frame=%lld B\n",
                    (long long)caps.Wmax,(long long)caps.Hmax,(long long)caps.Wmin,(long long)caps.Winc,
                    (long long)caps.Hmin,(long long)caps.Hinc,caps.bpp,
                    (long long)caps.defaultW,(long long)caps.defaultH,(long long)caps.defaultPayload,
                    (long long)caps.fullSensorBytes);

        struct Scn { const char* name; double x,y,w,h; };
        const Scn scns[] = {
            {"full frame (no crop)",          0.00,0.00,0.00,0.00},
            {"centered 50%",                  0.25,0.25,0.50,0.50},
            {"mono plate strip",              0.30,0.45,0.40,0.12},
            {"out-of-bounds (x+w>1) clamps",  0.80,0.80,0.50,0.50},
            {"sub-pixel tiny -> min size",    0.10,0.10,0.001,0.001},
        };

        for (auto& sc : scns) {
            std::printf("\n== %s  norm(x=%.3f y=%.3f w=%.3f h=%.3f) ==\n", sc.name, sc.x,sc.y,sc.w,sc.h);
            // alternate the two cameras so we also prove both emulated devices work
            const std::string& serial = serials[(&sc - scns) % 2];
            CaseResult r = runCase(serial, sc.x, sc.y, sc.w, sc.h);
            CHECK(r.connected, "connected + configured through the real supervisor path");
            if (!r.connected) continue;

            // Independent recompute of the expected AOI (mirrors applyRoi).
            int64_t expW, expH, expX, expY;
            if (sc.w <= 0.0 || sc.h <= 0.0) {          // full frame: applyRoi is a no-op ->
                expW = caps.defaultW; expH = caps.defaultH; expX = 0; expY = 0;   // camera keeps power-on frame
            } else {
                expW = clampi(alignDown((int64_t)std::llround(sc.w*caps.Wmax), caps.Wmin, caps.Winc), caps.Wmin, caps.Wmax);
                expH = clampi(alignDown((int64_t)std::llround(sc.h*caps.Hmax), caps.Hmin, caps.Hinc), caps.Hmin, caps.Hmax);
                expX = clampi(alignDown((int64_t)std::llround(sc.x*caps.Wmax), caps.OXmin, caps.OXinc), caps.OXmin, caps.Wmax-expW);
                expY = clampi(alignDown((int64_t)std::llround(sc.y*caps.Hmax), caps.OYmin, caps.OYinc), caps.OYmin, caps.Hmax-expH);
            }
            std::printf("  applied  W=%lld H=%lld X=%lld Y=%lld payload=%lld B\n",
                        (long long)r.W,(long long)r.H,(long long)r.X,(long long)r.Y,(long long)r.payload);
            std::printf("  expected W=%lld H=%lld X=%lld Y=%lld\n",
                        (long long)expW,(long long)expH,(long long)expX,(long long)expY);

            CHECK(r.W==expW && r.H==expH, "Width/Height match expected (increment-aligned)");
            CHECK(r.X==expX && r.Y==expY, "OffsetX/Y match expected (in-bounds, aligned)");
            CHECK(r.X + r.W <= caps.Wmax && r.Y + r.H <= caps.Hmax, "offset+size stays within the sensor");
            if (sc.w > 0.0 && sc.h > 0.0) {
                // PayloadSize == cropped pixels * bytes/pixel, and strictly less than the
                // full uncropped sensor frame -> a real per-frame GigE bandwidth cut.
                const int64_t expBytes = (int64_t)std::llround((double)r.W*r.H*caps.bpp);
                CHECK(r.payload == expBytes, "PayloadSize == cropped W*H*bpp (frame really is smaller)");
                CHECK(r.payload > 0 && r.payload < caps.fullSensorBytes,
                      "PayloadSize < full-sensor frame (real GigE bandwidth cut)");
                std::printf("  bandwidth: full-sensor %lld -> cropped %lld B/frame (%.1f%% of sensor)\n",
                            (long long)caps.fullSensorBytes, (long long)r.payload,
                            caps.fullSensorBytes>0 ? 100.0*r.payload/caps.fullSensorBytes : 0.0);
            } else {
                CHECK(r.payload == caps.defaultPayload, "full-frame payload unchanged (no-op)");
            }
        }
    }
    catch (const Pylon::GenericException& e) {
        std::printf("[FAIL] pylon exception: %s\n", e.GetDescription());
        rc = 2; ++g_fail;
    }
    Pylon::PylonTerminate();

    std::printf("\n==== %s (%d failure(s)) ====\n", g_fail==0 ? "ALL AOI TESTS PASSED" : "AOI TESTS FAILED", g_fail);
    return (g_fail==0 && rc==0) ? 0 : 1;
}
