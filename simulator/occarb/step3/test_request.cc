// OCCARB Step 3 unit test: Request::is_dma defaults to false for every constructor.
// Build+run from simulator/:  g++ -std=c++11 -I ramulator occarb/step3/test_request.cc -o /tmp/test_request && /tmp/test_request
#include <cstdio>
#include "Request.h"
using namespace ramulator;
static int fails = 0;
#define CHECK(cond, msg) do { if (cond) printf("PASS %s\n", msg); else { printf("FAIL %s\n", msg); fails++; } } while (0)

int main() {
    auto cb = [](Request&) {};
    vector<int> av(6, 0);
    Request r0;
    Request r1(0x1000L, Request::Type::READ, 0);
    Request r2(0x2000L, Request::Type::WRITE, cb, 1);
    Request r3(av, Request::Type::READ, cb, 2);
    CHECK(!r0.is_dma, "3b default ctor        -> is_dma == false");
    CHECK(!r1.is_dma, "3b (addr,type,core)     -> is_dma == false");
    CHECK(!r2.is_dma, "3b (addr,type,cb,core)  -> is_dma == false");
    CHECK(!r3.is_dma, "3b (vec,type,cb,core)   -> is_dma == false");
    r1.is_dma = true;
    Request copy = r1;                       // requests are copied into controller queues
    CHECK(copy.is_dma, "3b is_dma survives copy (queue insertion)");
    CHECK(copy.coreid == 0 && copy.type == Request::Type::READ, "3b other fields unchanged");
    return fails ? 1 : 0;
}
