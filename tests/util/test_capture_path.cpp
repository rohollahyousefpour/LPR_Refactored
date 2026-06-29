#include "lpr/util/CapturePathBuilder.h"
#include <filesystem>
#include <iostream>
namespace fs = std::filesystem;
static int fails=0;
#define CHECK(c) do{ if(!(c)){ std::cerr<<"  FAIL: "<<#c<<" ("<<__LINE__<<")\n"; ++fails; } }while(0)
int main(){
    fs::path base = fs::temp_directory_path() / ("lpr_cap_" + std::to_string(::time(nullptr)));
    lpr::CapturePathBuilder b(base.string());
    auto p = b.build("7");
    CHECK(p.fullImage.find("full") != std::string::npos);
    CHECK(p.plateImage.find("plate") != std::string::npos);
    CHECK(p.fullImage.find("7")  != std::string::npos);   // gate
    CHECK(p.fullImage.size()>4 && p.fullImage.substr(p.fullImage.size()-4)==".jpg");
    CHECK(fs::exists(fs::path(p.fullImage).parent_path()));   // dir was created
    CHECK(fs::exists(fs::path(p.plateImage).parent_path()));
    CHECK(p.relative.rfind("7/",0)==0);                       // starts with gate
    std::error_code ec; fs::remove_all(base, ec);             // cleanup
    if(fails==0){ std::cout<<"capture_path: ALL TESTS PASSED\n"; return 0; } return 1;
}
