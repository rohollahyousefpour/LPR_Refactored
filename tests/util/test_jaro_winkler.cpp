#include "lpr/util/JaroWinkler.h"
#include <cmath>
#include <iostream>
static int fails=0;
#define CHECK(c) do{ if(!(c)){ std::cerr<<"  FAIL: "<<#c<<" ("<<__LINE__<<")\n"; ++fails; } }while(0)
int main(){
    CHECK(std::abs(lpr::jaroWinklerDistance("plate","plate") - 1.0) < 1e-9);  // identical
    CHECK(lpr::jaroWinklerDistance("",  "abc") == 0.0);                       // empty
    double d = lpr::jaroWinklerDistance("MARTHA","MARHTA");
    CHECK(d > 0.95 && d < 0.97);                                              // ~0.961 known
    CHECK(lpr::jaroWinklerDistance("plate","plant") < 1.0);
    CHECK(lpr::jaroWinklerDistance("plate","xyzab") < lpr::jaroWinklerDistance("plate","plant"));
    if(fails==0){ std::cout<<"jaro_winkler: ALL TESTS PASSED\n"; return 0; } return 1;
}
