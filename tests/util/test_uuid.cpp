#include "lpr/util/Uuid.h"
#include <iostream>
#include <set>
static int fails=0;
#define CHECK(c) do{ if(!(c)){ std::cerr<<"  FAIL: "<<#c<<" ("<<__LINE__<<")\n"; ++fails; } }while(0)
int main(){
    auto u = lpr::generateUuidV4();
    CHECK(u.size() == 36);
    CHECK(u[8]=='-' && u[13]=='-' && u[18]=='-' && u[23]=='-');
    CHECK(u[14]=='4');               // version nibble
    std::set<std::string> seen;      // uniqueness over many
    for (int i=0;i<10000;++i) seen.insert(lpr::generateUuidV4());
    CHECK(seen.size()==10000);
    if(fails==0){ std::cout<<"uuid: ALL TESTS PASSED\n"; return 0; } return 1;
}
