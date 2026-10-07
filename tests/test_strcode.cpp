#include <cstdio>
#include <cstdint>
#include <string_view>

#include "engine/core/strcode.h"

namespace {

struct HashVector {
    const char* text;
    uint64_t expected;
};

const HashVector kVectors[] = {
    {"f000", 0xBCF210724BC1ULL},
    {"jack", 0xF2CC6FEC116CULL},
    {"demoscriptchangegamestep.lua", 0x70205ED85FD4ULL},
    {"trapgotoending.lua", 0x9AB4F6943BC1ULL},
    {"ShTrapExecDoorCallbackDataElement", 0x451A58FDD3FAULL},
    {"", 0x000000000000ULL},
    {"ujzBde4gx7d2ncf", 0xA1EC39E5BCBCULL},
    {"_/epf5_d6hoAA7d6", 0xD8968B18C51DULL},
    {"7zdoc5is/j4h6t5Dl", 0xEFCF8F3780E3ULL},
    {"g76Amxg5Fe6d9n1D4_u|7|xtplEpf6t", 0xB56C6F3C5D06ULL},
    {"31v.s8eh2/kvj1/cCe56uvEw817|efr0", 0xB88D46ECB8CAULL},
    {"ECedEtB6D.sFyCwb|wk9h1dnsipzz1fk.", 0x19B8D17A7DCFULL},
    {"z5ri_5rF/wDyojfljoCoa17lqsaj/4x96uiE29BDd|D5zzzzg0Azdmen.khv8dg", 0x4BF37DC029FFULL},
    {"a6j4gx9ben9yjAqw8x0hh1|00tfjgvq0Ek3bn3xjE4b3tBfEq3xkwo442vAo9mpz", 0xE4BD08D3110AULL},
    {"om31wbbr0qmE8w.wxfogo0mvn099a0BwBfChyFm0l_Avfz|zfkkibj7|Bj980Cwj5", 0x6D2C79F4FA3DULL},
    {"5ibaBg3i_mnbqns2p7uq4/idw|C73/2i4j32b.l8ajlj09h5duD3350g5dpmrcg2.5be.u9282mEr.2402pE3q5m.i/hz.ueCp_enCthjFBCxjqi|ogz1kCokF_2zv/", 0x0A21D094F10DULL},
    {"mwufxbv5|.Fbyv39s2ehogfqrclri_Dqzj4261EufrdEl_erbAfqf8oeqh|av5/r9ic3FphkqdlmtAt3ns.2Dlrwbqcab25m20p.gCB_C14z2tEnovmFAizwdiaeAq_k", 0x3F51433969DDULL},
    {"dfCy2Cs8pEsc|lkr.aqxv5upctnwlavyf0r2Bmp2afqfjz7czbttAof73jCF8yu1js9BjcF2A_E2i326bD7FDEBofbciAxgy.5dAbA4Dp1qa|e24fC3e0qeqpnoB|1ye0", 0x735C2B9E05C2ULL},
    {"Dsc9ABme8jvqBEt96ia0d1rDgEnD1sF3s|||h5mtf0bs|e2.rynne7fj3qxi8A2rhFxo11zbka1D.ztj/wyuhvauvzhmFasqxezy7ex_rdrgdCsAjpr_2umx_bAz55nfd/.9iBs1d5ik0/vstqBqzBpt05CzhkBken215o.v._i5mpflv5fupxq6mb/y/3nyrvd1r6xi", 0xF2E2B119ED31ULL},
    {"D23AnfrpyzB._tbic_F071aez3|.pgojj3DgEB|f5caio6cBFtiAq3A_Ehget37myqo8aa4t|ruBp03p5pb/FBtdbm1DB/fqoC_xo1cEvF/xDzmas2en1mtmo|oqsg919lo1/Cd8jzdnb8j/dFdlz.FuhfkvmlB3|ctCyxv.kgafrfw/h5nywt_fdF0mx4.mux0bA/pAzcyc|edqme8vxrv9cqFEurta8Aebog0F|yq_1i1latEj8puu|x8f2mzkp", 0x1B47B51287BFULL},
};

}

int main() {
    int failures = 0;
    for (const auto& v : kVectors) {
        const uint64_t got = std::string_view(v.text).empty() ? 0 : pt::StrCode64(v.text);
        if (got != v.expected) {
            std::printf("StrCode64 mismatch for %s: %012llX expected %012llX\n", v.text, static_cast<unsigned long long>(got), static_cast<unsigned long long>(v.expected));
            ++failures;
        }
    }
    std::printf("%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
