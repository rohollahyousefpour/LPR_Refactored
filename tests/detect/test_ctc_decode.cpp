#include "lpr/detect/CtcDecoder.h"
#include <cassert>
#include <iostream>
#include <vector>

static void put(std::vector<float>& d, int t, int C, int cls) { d[(size_t)t * C + cls] = 5.f; }

int main() {
    const std::string A = "0123456789";
    const int C = 11, blank = 10;

    // blank-last: argmax 1,1,blank,2,3,3 -> "123"
    { const int T = 6; std::vector<float> d(T * C, 0.f);
      int seq[T] = {1,1,blank,2,3,3}; for (int t=0;t<T;++t) put(d,t,C,seq[t]);
      double conf=0; auto s = lpr::ctcGreedyDecode(d,T,C,A,blank,&conf);
      std::cout << "case1='" << s << "' conf=" << conf << "\n"; assert(s=="123"); assert(conf>0.5); }

    // blank-first (index 0): chars shift by one. argmax 1,2,2,3 -> alphabet[0,1,2]="012"
    { const int T = 4, bf = 0; std::vector<float> d(T * C, 0.f);
      int seq[T] = {1,2,2,3}; for (int t=0;t<T;++t) put(d,t,C,seq[t]);
      double conf=0; auto s = lpr::ctcGreedyDecode(d,T,C,A,bf,&conf);
      std::cout << "case2='" << s << "'\n"; assert(s=="012"); }

    // all blanks -> empty, zero confidence
    { const int T = 3; std::vector<float> d(T * C, 0.f);
      for (int t=0;t<T;++t) put(d,t,C,blank);
      double conf=1; auto s = lpr::ctcGreedyDecode(d,T,C,A,blank,&conf);
      assert(s.empty()); assert(conf==0.0); }

    std::cout << "ctc_decode: OK\n";
    return 0;
}
