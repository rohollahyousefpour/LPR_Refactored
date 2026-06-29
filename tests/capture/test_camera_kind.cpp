// SDK-free: the camera-kind dispatch logic.
#include "lpr/capture/CameraKind.h"
#include <iostream>
static int fails=0;
#define CHECK(c) do{ if(!(c)){ std::cerr<<"  FAIL: "<<#c<<" ("<<__LINE__<<")\n"; ++fails; } }while(0)
int main(){
    using lpr::CameraKind;
    CHECK(lpr::parseCameraKind("video")     == CameraKind::Video);
    CHECK(lpr::parseCameraKind("rtsp")      == CameraKind::Rtsp);
    CHECK(lpr::parseCameraKind("gstreamer") == CameraKind::Gstreamer);
    CHECK(lpr::parseCameraKind("pylon")     == CameraKind::Pylon);
    CHECK(lpr::parseCameraKind("grey")      == CameraKind::PointGrey);
    CHECK(lpr::parseCameraKind("")          == CameraKind::Unknown);
    CHECK(lpr::parseCameraKind("RTSP")      == CameraKind::Unknown);   // case-sensitive
    CHECK(lpr::parseCameraKind("ip")        == CameraKind::Unknown);
    if(fails==0){ std::cout<<"camera_kind: ALL TESTS PASSED\n"; return 0; } return 1;
}
