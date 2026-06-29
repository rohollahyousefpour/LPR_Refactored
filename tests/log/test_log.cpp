#include "lpr/Log.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <ctime>
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do{ if(!(c)){ std::cerr<<"  FAIL: "<<#c<<" ("<<__LINE__<<")\n"; ++fails; } }while(0)
static std::string readAll(const fs::path& p){ std::ifstream f(p); std::stringstream s; s<<f.rdbuf(); return s.str(); }

int main(){
    fs::path dir = fs::temp_directory_path() / ("lpr_log_" + std::to_string(::time(nullptr)));
    auto& log = lpr::Logger::instance();
    log.init("testapp", dir.string());
    log.setConsole(false);
    log.setLevel(lpr::Level::Info);

    LOGD() << "this debug line must be filtered out";   // below Info
    LOGI() << "hello " << 42;
    LOGE() << "boom";

    std::string content;
    for (auto& e : fs::directory_iterator(dir)) content += readAll(e.path());

    CHECK(content.find("[INFO] hello 42") != std::string::npos);
    CHECK(content.find("[ERROR] boom")    != std::string::npos);
    CHECK(content.find("must be filtered") == std::string::npos);   // level filter works
    CHECK(content.find("] [INFO] ")       != std::string::npos);    // timestamp+level format
    CHECK(!log.isEnabled(lpr::Level::Debug));
    CHECK(log.isEnabled(lpr::Level::Warning));

    std::error_code ec; fs::remove_all(dir, ec);
    if (fails==0){ std::cout<<"log: ALL TESTS PASSED\n"; return 0; }
    return 1;
}
