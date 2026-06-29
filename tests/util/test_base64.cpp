#include "lpr/util/Base64.h"
#include <iostream>
static int fails=0;
#define CHECK(c) do{ if(!(c)){ std::cerr<<"  FAIL: "<<#c<<" ("<<__LINE__<<")\n"; ++fails; } }while(0)
int main(){
    CHECK(lpr::base64_encode(std::string("Man")) == "TWFu");          // classic vector
    CHECK(lpr::base64_encode(std::string("M")) == "TQ==");
    CHECK(lpr::base64_decode(std::string("TWFu")) == "Man");
    const std::string msg = "client_deep_lpr \x01\x02\xff end";
    CHECK(lpr::base64_decode(lpr::base64_encode(msg)) == msg);   // binary round-trip
    if(fails==0){ std::cout<<"base64: ALL TESTS PASSED\n"; return 0; } return 1;
}
