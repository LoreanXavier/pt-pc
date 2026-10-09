#include "engine/audio/dsp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <utility>

#include "engine/audio/wwise_bank.h"

namespace pt::audio {
namespace {

constexpr float kPi = 3.14159265358979f;

class ParamReader {
public:
    explicit ParamReader(const std::vector<uint8_t>& data) : data_(data) {}

    float F32() {
        float v = 0.0f;
        if (pos_ + 4 <= data_.size()) {
            std::memcpy(&v, data_.data() + pos_, 4);
        }
        pos_ += 4;
        return v;
    }

    uint32_t U32() {
        uint32_t v = 0;
        if (pos_ + 4 <= data_.size()) {
            std::memcpy(&v, data_.data() + pos_, 4);
        }
        pos_ += 4;
        return v;
    }

    uint8_t U8() {
        const uint8_t v = pos_ < data_.size() ? data_[pos_] : 0;
        pos_ += 1;
        return v;
    }

private:
    const std::vector<uint8_t>& data_;
    size_t pos_ = 0;
};

struct ErTap {
    float ms;
    float gain;
};

// 0x13B4FE0
constexpr ErTap kErShortDarkHallLeft[] = {{0.0208333004f, -0.238924295f}, {1.52083325f, -0.222542495f}, {3.02083325f, 0.0940056965f},
    {4.52083349f, -0.0336034f}, {5.54166651f, 0.325573891f}, {6.02083349f, 0.00675110007f}, {7.04166651f, 0.367163688f},
    {7.52083349f, 0.00326190004f}, {8.54166698f, -0.2010452f}, {9.02083302f, -0.00588939991f}, {10.041667f, 0.105965398f},
    {10.520833f, 0.00558889983f}, {11.541667f, -0.0534484982f}, {11.666667f, 0.0617191009f}, {12.020833f, -0.00435080007f},
    {13.041667f, 0.0258406997f}, {13.166667f, -0.0694949999f}, {13.520833f, 0.0031053999f}, {14.541667f, -0.0116814002f},
    {14.666667f, 0.0583834015f}, {16.041666f, 0.00456479983f}, {16.166666f, -0.0430942997f}, {16.479166f, 0.297108501f},
    {17.666666f, 0.0293918997f}, {17.979166f, 0.33526969f}, {19.166666f, -0.0189168006f}, {19.479166f, -0.182341307f}, {20.666666f, 0.0117891002f},
    {20.979166f, 0.096143797f}, {22.166666f, -0.00710469997f}, {22.479166f, -0.0491092987f}, {23.666666f, 0.00410539983f},
    {23.979166f, 0.0234066993f}, {25.479166f, -0.0104684001f}, {26.979166f, 0.00420940015f}, {28.541666f, -0.0431605987f},
    {30.041666f, 0.0482851006f}, {31.125f, 0.167268395f}, {31.541666f, -0.0406646989f}, {32.625f, 0.188114807f}, {33.0416679f, 0.0299022999f},
    {34.125f, -0.103030503f}, {34.5416679f, -0.0203728005f}, {35.625f, 0.0542191006f}, {36.0416679f, 0.0132104f}, {37.125f, -0.0274467003f},
    {37.5416679f, -0.00827160012f}, {38.625f, 0.0132227f}, {39.0416679f, 0.00487820012f}, {40.125f, -0.00592250004f},
    {40.5416679f, -0.00281960005f}, {41.625f, 0.00235860003f}, {43.9583321f, 0.00752170011f}, {45.4583321f, -0.00846510008f},
    {46.9583321f, 0.00704429997f}, {48.4583321f, -0.0052176998f}, {49.9583321f, 0.00357639999f}, {50.3125f, 0.0162103996f},
    {51.8125f, 0.0182323009f}, {53.3125f, -0.00995959993f}, {54.8125f, 0.00523489993f}, {56.3125f, -0.00269520003f}, {66.0833359f, 0.00340329995f},
    {67.5833359f, 0.00373280002f}};
// 0x13B51E0
constexpr ErTap kErShortDarkHallRight[] = {{0.0208333004f, 0.256066412f}, {1.52083325f, 0.337683409f}, {3.02083325f, -0.212503493f},
    {4.52083349f, 0.129680201f}, {6.02083349f, -0.0769196004f}, {7.04166651f, -0.0681473985f}, {7.52083349f, 0.0437025987f},
    {8.54166698f, 0.077070199f}, {9.02083302f, -0.0242954995f}, {10.041667f, -0.0640463009f}, {10.166667f, 0.335205406f},
    {10.520833f, 0.0132042002f}, {11.541667f, 0.0474993996f}, {11.666667f, 0.377345413f}, {12.020833f, -0.00682190014f},
    {13.041667f, -0.0323466994f}, {13.166667f, -0.207089305f}, {13.520833f, 0.00330949994f}, {14.541667f, 0.0208186992f},
    {14.666667f, 0.109666802f}, {16.041666f, -0.0129650999f}, {16.166666f, -0.0554195009f}, {17.541666f, 0.00779369986f}, {17.666666f, 0.0265317f},
    {17.979166f, -0.0626398027f}, {19.041666f, -0.00451890007f}, {19.166666f, -0.0119746001f}, {19.479166f, 0.069686003f},
    {20.666666f, 0.00474270014f}, {20.979166f, -0.0583149008f}, {22.479166f, 0.0430020988f}, {23.979166f, -0.0294477008f},
    {25.479166f, 0.0189516991f}, {26.979166f, -0.0117597999f}, {27.041666f, -0.232993707f}, {28.479166f, 0.00708519993f},
    {28.541666f, -0.264919609f}, {29.979166f, -0.00417109998f}, {30.041666f, 0.143901497f}, {31.541666f, -0.0761372f}, {32.625f, -0.0350863002f},
    {33.0416679f, 0.038577199f}, {34.125f, 0.039467901f}, {34.5416679f, -0.0184439998f}, {35.625f, -0.0332067013f}, {36.0416679f, 0.00826979987f},
    {37.125f, 0.0244897008f}, {37.5416679f, -0.00331300008f}, {38.625f, -0.0165040009f}, {40.125f, 0.0108519001f}, {41.625f, -0.00668840017f},
    {42.4583321f, 0.0409747995f}, {43.125f, 0.00406190008f}, {43.9583321f, 0.0461238995f}, {45.4583321f, -0.0251691006f},
    {46.9583321f, 0.0133121004f}, {48.4583321f, -0.00666770013f}, {49.9583321f, 0.00320770009f}, {51.8125f, -0.0033609001f},
    {53.3125f, 0.00378499995f}, {54.8125f, -0.00320859998f}, {55.6875f, -0.00982309971f}, {57.1875f, -0.0110612996f}, {58.6875f, 0.00603569997f}};
// 0x13B58B0
constexpr ErTap kErSmallHallLeft[] = {{0.0208333004f, -0.254223108f}, {1.52083325f, -0.237390593f}, {3.02083325f, 0.100649104f},
    {4.10416651f, 0.346620709f}, {4.52083349f, -0.0360032991f}, {5.60416651f, 0.392357498f}, {6.02083349f, 0.00735339988f},
    {7.10416651f, -0.214935005f}, {7.52083349f, 0.00349660008f}, {8.60416698f, 0.114200003f}, {9.02083302f, 0.0597019009f},
    {10.104167f, -0.0577229001f}, {10.520833f, -0.0687628984f}, {11.604167f, 0.0280441996f}, {12.020833f, 0.0580400005f},
    {12.166667f, 0.301965803f}, {13.104167f, -0.0126251001f}, {13.520833f, -0.0429228991f}, {13.666667f, 0.342709899f},
    {14.604167f, 0.00503139989f}, {15.020833f, 0.0292838998f}, {15.166667f, -0.187637299f}, {16.520834f, -0.0190172009f},
    {16.666666f, 0.0986896008f}, {18.020834f, 0.0118463002f}, {18.166666f, -0.0503870994f}, {19.520834f, -0.00716819987f},
    {19.666666f, 0.0244784001f}, {21.020834f, 0.00425900007f}, {21.166666f, -0.0109655f}, {21.479166f, -0.0272343997f},
    {22.520834f, -0.00236979988f}, {22.666666f, 0.00439770008f}, {22.979166f, 0.111602202f}, {24.479166f, 0.0660896003f},
    {25.979166f, -0.0314260013f}, {27.479166f, 0.0135653f}, {28.979166f, -0.00512349978f}, {32.8541679f, 0.00244539999f},
    {34.3541679f, -0.00278460002f}, {35.8541679f, 0.00231990009f}, {37.1458321f, 0.00484779989f}, {38.6458321f, 0.00539579988f},
    {40.1458321f, -0.00302190008f}};
// 0x13B5A10
constexpr ErTap kErSmallHallRight[] = {{0.0208333004f, 0.274282992f}, {1.52083325f, 0.362415105f}, {3.02083325f, -0.231593296f},
    {4.52083349f, 0.141639307f}, {5.60416651f, -0.0736631975f}, {6.02083349f, -0.082871899f}, {7.10416651f, 0.0826603025f},
    {7.52083349f, 0.405862808f}, {8.60416698f, -0.0700064003f}, {9.02083302f, 0.380172998f}, {10.104167f, 0.0512023009f},
    {10.520833f, -0.208841607f}, {11.604167f, -0.0352106988f}, {12.020833f, 0.110267602f}, {13.104167f, 0.0227015f}, {13.520833f, -0.056255199f},
    {13.666667f, -0.0636764988f}, {14.604167f, -0.0141778002f}, {15.020833f, 0.0270362999f}, {15.166667f, 0.0724719018f},
    {16.104166f, 0.00856749993f}, {16.520834f, -0.0122023001f}, {16.666666f, -0.0605112985f}, {17.604166f, -0.00498769991f},
    {18.020834f, 0.00499710022f}, {18.166666f, 0.0447055995f}, {19.104166f, 0.00279400009f}, {19.666666f, -0.0306330007f},
    {19.979166f, -0.147529304f}, {21.166666f, 0.0198327992f}, {21.479166f, -0.167296797f}, {22.666666f, -0.0123108998f},
    {22.979166f, 0.0911409035f}, {24.166666f, 0.00750489999f}, {24.479166f, -0.0660042986f}, {25.666666f, -0.00436009979f},
    {25.979166f, 0.0441895984f}, {27.166666f, 0.00248869997f}, {27.479166f, -0.0280895997f}, {28.979166f, 0.0174327996f},
    {30.479166f, -0.0103438003f}, {31.354166f, 0.0134036001f}, {31.979166f, 0.00600960013f}, {32.8541679f, 0.0151645001f},
    {33.4791679f, -0.00343960011f}, {34.3541679f, -0.00831940025f}, {35.8541679f, 0.00434560003f}, {41.1041679f, -0.00280470005f},
    {42.6041679f, -0.00322189997f}};
// 0x13B7DF0
constexpr ErTap kErBathroomLeft[] = {{0.0208333004f, -0.249885097f}, {1.52083325f, -0.234171107f}, {3.02083325f, 0.0992496982f},
    {3.91666675f, 0.344433486f}, {4.52083349f, -0.0351604f}, {5.41666651f, 0.388508111f}, {6.02083349f, 0.0070921001f},
    {6.91666651f, -0.213705093f}, {8.41666698f, 0.112272598f}, {9.02083302f, -0.00615199981f}, {9.91666698f, -0.0570220016f},
    {10.520833f, 0.00582680013f}, {11.416667f, 0.0275360998f}, {12.020833f, -0.00451109977f}, {12.916667f, -0.0123052998f},
    {14.416667f, 0.00493550021f}, {14.458333f, 0.0629184991f}, {15.958333f, -0.0708276033f}, {17.458334f, 0.0596332997f},
    {18.416666f, 0.294418901f}, {18.958334f, -0.043912001f}, {19.916666f, 0.329943389f}, {20.458334f, 0.0299181007f}, {21.416666f, -0.180910304f},
    {21.958334f, -0.0194049999f}, {22.916666f, 0.0960865989f}, {23.458334f, 0.0121117001f}, {24.416666f, -0.0489291996f},
    {24.958334f, -0.00730129983f}, {25.916666f, 0.0234587006f}, {26.0f, -0.0458943993f}, {26.458334f, 0.00416419981f},
    {27.416666f, -0.0106108002f}, {27.5f, 0.0514621995f}, {28.916666f, 0.00419750018f}, {29.0f, -0.0434379987f}, {30.5f, 0.0316619016f},
    {32.0f, -0.0217271f}, {32.9791679f, 0.110950701f}, {33.5f, 0.0140942f}, {34.4791679f, 0.125319704f}, {35.0f, -0.0088018002f},
    {35.9791679f, -0.0687005967f}, {36.5f, 0.00525509985f}, {37.4791679f, 0.0361147001f}, {38.9791679f, -0.0183620993f},
    {39.9583321f, 0.00837900024f}, {40.4791679f, 0.00899850018f}, {41.4583321f, -0.00944919977f}, {41.9791679f, -0.00399819994f},
    {42.9583321f, 0.00797449984f}, {44.4583321f, -0.00585879991f}, {45.9583321f, 0.00399400014f}, {48.3958321f, 0.0130877001f},
    {49.8958321f, 0.0147676999f}, {51.3958321f, -0.00804340001f}, {52.8958321f, 0.00417830003f}, {59.0833321f, 0.00419529993f},
    {60.5833321f, 0.00473190006f}};
// 0x13B7FD0
constexpr ErTap kErBathroomRight[] = {{0.0208333004f, 0.256831706f}, {1.52083325f, 0.341239303f}, {3.02083325f, -0.216059998f},
    {4.52083349f, 0.131151497f}, {5.41666651f, -0.0696600974f}, {6.02083349f, -0.0777421966f}, {6.91666651f, 0.0782596022f},
    {7.52083349f, 0.0444042012f}, {8.41666698f, -0.0659670979f}, {9.02083302f, -0.0247783009f}, {9.91666698f, 0.0486163013f},
    {10.520833f, 0.0132564995f}, {11.416667f, -0.0330840014f}, {12.020833f, -0.00700950017f}, {12.916667f, 0.0215987004f},
    {12.958333f, 0.329995304f}, {14.416667f, -0.0131604001f}, {14.458333f, 0.372651607f}, {15.916667f, 0.00809289981f},
    {15.958333f, -0.203696102f}, {17.416666f, -0.00471579982f}, {17.458334f, 0.107547298f}, {18.958334f, -0.0546744987f},
    {19.916666f, -0.0590925999f}, {20.458334f, 0.0262711998f}, {21.416666f, 0.0666747987f}, {21.958334f, -0.0116692996f},
    {22.916666f, -0.0558969006f}, {23.458334f, 0.00474220002f}, {24.416666f, 0.0413693003f}, {24.5f, -0.239069402f}, {25.916666f, -0.0282521006f},
    {26.0f, -0.269879788f}, {27.416666f, 0.0183667f}, {27.5f, 0.1475669f}, {28.916666f, -0.0112802004f}, {29.0f, -0.0776316971f},
    {30.416666f, 0.00674259989f}, {30.5f, 0.0395894013f}, {31.916666f, -0.00398290018f}, {32.0f, -0.0190478005f}, {33.5f, 0.00850069989f},
    {34.4791679f, -0.0225920994f}, {35.9791679f, 0.0255753994f}, {37.4791679f, -0.0212296993f}, {38.4583321f, 0.0435857996f},
    {38.9791679f, 0.0156375002f}, {39.9583321f, 0.0492918007f}, {40.4791679f, -0.0106308004f}, {41.4583321f, -0.0270199999f},
    {41.9791679f, 0.00684279995f}, {42.9583321f, 0.0142548f}, {43.4791679f, -0.00431609992f}, {44.4583321f, -0.00729409978f},
    {52.7291679f, -0.0082620997f}, {54.2291679f, -0.00934110023f}, {55.7291679f, 0.00514520006f}};

struct ErPattern {
    const ErTap* left;
    size_t left_count;
    const ErTap* right;
    size_t right_count;
};

bool FindErPattern(uint32_t index, ErPattern& out) {
    switch (index) {
        case 10: out = {kErShortDarkHallLeft, std::size(kErShortDarkHallLeft), kErShortDarkHallRight, std::size(kErShortDarkHallRight)}; break;
        case 13: out = {kErSmallHallLeft, std::size(kErSmallHallLeft), kErSmallHallRight, std::size(kErSmallHallRight)}; break;
        case 24: out = {kErBathroomLeft, std::size(kErBathroomLeft), kErBathroomRight, std::size(kErBathroomRight)}; break;
        default: return false;
    }
    return true;
}

uint32_t NextLcg(uint32_t& state) {
    state = state * 0x0BB38435u + 0x3619636Bu;
    return state;
}

std::vector<uint32_t> PrimeDelays(const std::vector<float>& ms) {
    std::vector<uint32_t> out(ms.size());
    for (size_t i = 0; i < ms.size(); ++i) {
        uint32_t v = static_cast<uint32_t>(static_cast<int64_t>(static_cast<float>(kOutputRate) * ms[i] * 0.001f));
        if ((v & 1) == 0) {
            ++v;
        }
        const uint32_t bound = static_cast<uint32_t>(std::sqrt(static_cast<double>(v))) + 1;
        const uint32_t previous = i ? out[i - 1] : 0;
        for (;; v += 2) {
            bool prime = true;
            for (uint32_t d = 3; d < bound; d += 2) {
                if (v % d == 0) {
                    prime = false;
                    break;
                }
            }
            if (prime && v != previous) {
                break;
            }
        }
        out[i] = v;
    }
    return out;
}

class SampleDelay {
public:
    void Resize(uint32_t length) {
        buffer_.assign(std::max<uint32_t>(length, 4), 0.0f);
        pos_ = 0;
    }
    bool Active() const { return !buffer_.empty(); }
    float Push(float x) {
        const float y = buffer_[pos_];
        buffer_[pos_] = x;
        if (++pos_ == buffer_.size()) {
            pos_ = 0;
        }
        return y;
    }
    void Clear() { std::fill(buffer_.begin(), buffer_.end(), 0.0f); }

private:
    std::vector<float> buffer_;
    size_t pos_ = 0;
};

struct WwiseBiquad {
    float k[5] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float x1 = 0.0f;
    float x2 = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;

    void Design(uint32_t type, float freq, float gain_db, float q) {
        const float fs = static_cast<float>(kOutputRate);
        const float f = std::min(fs * 0.45f, freq);
        const float w = f * 2.0f * kPi / fs;
        const float cw = std::cos(w);
        const float sw = std::sin(w);
        const float a = std::pow(10.0f, gain_db * 0.025f);
        const float shelf = sw * 1.41421356f * std::sqrt(a);
        const float alpha = sw / (q + q);
        float b0 = 1.0f;
        float b1 = 0.0f;
        float b2 = 0.0f;
        float a0 = 1.0f;
        float a1 = 0.0f;
        float a2 = 0.0f;
        switch (type) {
            case 0:
                b0 = a * ((a + 1.0f) - (a - 1.0f) * cw + shelf);
                b1 = 2.0f * a * ((a - 1.0f) - (a + 1.0f) * cw);
                b2 = a * ((a + 1.0f) - (a - 1.0f) * cw - shelf);
                a0 = (a + 1.0f) + (a - 1.0f) * cw + shelf;
                a1 = -2.0f * ((a - 1.0f) + (a + 1.0f) * cw);
                a2 = (a + 1.0f) + (a - 1.0f) * cw - shelf;
                break;
            case 1:
                b0 = 1.0f + alpha * a;
                b1 = -2.0f * cw;
                b2 = 1.0f - alpha * a;
                a0 = 1.0f + alpha / a;
                a1 = -2.0f * cw;
                a2 = 1.0f - alpha / a;
                break;
            case 2:
                b0 = a * ((a + 1.0f) + (a - 1.0f) * cw + shelf);
                b1 = -2.0f * a * ((a - 1.0f) + (a + 1.0f) * cw);
                b2 = a * ((a + 1.0f) + (a - 1.0f) * cw - shelf);
                a0 = (a + 1.0f) - (a - 1.0f) * cw + shelf;
                a1 = 2.0f * ((a - 1.0f) - (a + 1.0f) * cw);
                a2 = (a + 1.0f) - (a - 1.0f) * cw - shelf;
                break;
            case 3:
            case 4: {
                const float t = std::tan(f * kPi / fs);
                const float k2 = type == 3 ? 1.0f / t : t;
                const float n = 1.0f / (k2 * k2 + 1.41421356f * k2 + 1.0f);
                b0 = n;
                b1 = type == 3 ? 2.0f * n : -2.0f * n;
                b2 = n;
                a1 = type == 3 ? 2.0f * n * (1.0f - k2 * k2) : 2.0f * n * (k2 * k2 - 1.0f);
                a2 = n * (k2 * k2 - 1.41421356f * k2 + 1.0f);
                break;
            }
            case 5:
            case 6:
                b0 = type == 5 ? alpha : 1.0f;
                b1 = type == 5 ? 0.0f : -2.0f * cw;
                b2 = type == 5 ? -alpha : 1.0f;
                a0 = 1.0f + alpha;
                a1 = -2.0f * cw;
                a2 = 1.0f - alpha;
                break;
            default: return;
        }
        k[0] = b0 / a0;
        k[1] = b1 / a0;
        k[2] = b2 / a0;
        k[3] = -a1 / a0;
        k[4] = -a2 / a0;
    }
    float Process(float x) {
        const float y = k[0] * x + k[1] * x1 + k[2] * x2 + k[3] * y1 + k[4] * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
    void Clear() { x1 = x2 = y1 = y2 = 0.0f; }
};

class RoomVerb : public Effect {
public:
    explicit RoomVerb(const std::vector<uint8_t>& params) {
        ParamReader r(params);
        const float decay = r.F32();
        const float hf_damping = r.F32();
        const float diffusion = r.F32();
        const float width = r.F32();
        float filter_gain[3];
        float filter_freq[3];
        float filter_q[3];
        for (int i = 0; i < 3; ++i) {
            filter_gain[i] = r.F32();
            filter_freq[i] = r.F32();
            filter_q[i] = r.F32();
        }
        const float front = DbToGain(r.F32());
        const float rear = DbToGain(r.F32());
        const float center = DbToGain(r.F32());
        r.F32();
        dry_gain_ = DbToGain(r.F32());
        const float er = DbToGain(r.F32());
        const float reverb = DbToGain(r.F32() - 3.0f);
        const bool enable_er = r.U8() != 0;
        const uint32_t er_pattern = r.U32();
        const float reverb_delay = r.F32();
        const float room_size = r.F32();
        const float front_back = r.F32();
        const float density = r.F32();
        const float room_shape = r.F32();
        const uint32_t units = std::clamp<uint32_t>(r.U32(), 1, 32);
        const bool tone = r.U8() != 0;
        uint32_t position[3];
        uint32_t curve[3];
        for (int i = 0; i < 3; ++i) {
            position[i] = r.U32();
            curve[i] = r.U32();
        }
        r.F32();
        r.F32();
        const float density_min = r.F32();
        const float density_max = r.F32();
        const float density_random = r.F32();
        const float shape_min = r.F32();
        const float shape_max = r.F32();
        const float diffusion_scale = r.F32();
        const float diffusion_max = r.F32();
        const float diffusion_random = r.F32();
        const float dc_cut = r.F32();
        const float unit_input_delay = r.F32();
        const float unit_input_random = r.F32();
        const float fs = static_cast<float>(kOutputRate);

        const uint32_t lines = units * 4;
        const float spread_high = density_min + (density_max - density_min) * 0.01f * density;
        const float spread_low = spread_high * (shape_min + (shape_max - shape_min) * 0.01f * room_shape);
        std::vector<float> ms(lines);
        uint32_t state = 0x6B4CC7u;
        for (uint32_t i = 0; i < lines; ++i) {
            const uint32_t n = NextLcg(state);
            const float range = (spread_high + (spread_high - spread_low)) - spread_low;
            const float base = spread_low + range * static_cast<float>(i) / static_cast<float>(lines - 1);
            ms[i] = base + static_cast<float>(n) * 2.3283064e-10f * base * density_random * 0.01f;
        }
        std::sort(ms.begin(), ms.end());
        const std::vector<uint32_t> lengths = PrimeDelays(ms);
        units_.resize(units);
        uint32_t tail = 0;
        uint32_t input_state = 0x1BC0F99u;
        const float input_base = unit_input_delay / static_cast<float>(units);
        const double decay_samples = static_cast<double>(decay) * kOutputRate;
        for (uint32_t u = 0; u < units; ++u) {
            Unit& unit = units_[u];
            for (int j = 0; j < 4; ++j) {
                unit.length[j] = lengths[j * units + u];
                unit.lines[j].Resize(unit.length[j]);
            }
            const double log3 = -3.0 * unit.length[3] / decay_samples;
            double damping = 1.0 - static_cast<double>(hf_damping) * hf_damping;
            if (damping * log3 * 0.5756462732485116 > 1.0) {
                damping = 1.0 / (log3 * 0.5756462732485116);
            }
            const float ratio = static_cast<float>(std::sqrt(1.0 - damping));
            damping = 1.0 - static_cast<double>(ratio) * ratio;
            for (int j = 0; j < 4; ++j) {
                const double lg = -3.0 * unit.length[j] / decay_samples;
                const double b = std::min(0.999, damping * lg * 0.5756462732485116);
                unit.feed[j] = static_cast<float>(std::pow(10.0, lg) * (1.0 - b));
                unit.pole[j] = static_cast<float>(b);
            }
            const uint32_t n = NextLcg(input_state);
            const float t = input_base + static_cast<float>(n) * 2.3283064e-10f * input_base * unit_input_random * 0.01f;
            const uint32_t samples = static_cast<uint32_t>(static_cast<int64_t>(t * 0.001f * fs));
            unit.input.Resize(samples);
            tail += std::max<uint32_t>(samples, 4);
        }

        float stage_ms[4];
        float scaled = diffusion_max * diffusion_scale * 0.01f;
        stage_ms[0] = diffusion_max + diffusion_max * diffusion_random * 0.01f * 0.364111f;
        stage_ms[1] = scaled + scaled * diffusion_random * 0.01f * 0.8184801f;
        scaled = scaled * diffusion_scale * 0.01f;
        stage_ms[2] = scaled + scaled * diffusion_random * 0.01f * 0.021083385f;
        scaled = scaled * diffusion_scale * 0.01f;
        stage_ms[3] = scaled + scaled * diffusion_random * 0.01f * 0.28434122f;
        std::vector<float> stages(stage_ms, stage_ms + 4);
        std::sort(stages.begin(), stages.end());
        const std::vector<uint32_t> stage_lengths = PrimeDelays(stages);
        for (int i = 0; i < 4; ++i) {
            diffusers_[i].length = std::max<uint32_t>(stage_lengths[i], 4);
            diffusers_[i].history.assign(static_cast<size_t>(diffusers_[i].length) * 2, 0.0f);
            diffusers_[i].gain = std::clamp(diffusion * 0.0247212f - static_cast<float>(3 - i) * 0.61803f, 0.0f, 0.618034f);
        }

        if (const uint32_t samples = static_cast<uint32_t>(static_cast<int64_t>(reverb_delay * 0.001f * fs))) {
            predelay_.Resize(samples);
        }
        ErPattern pattern;
        if (enable_er && FindErPattern(er_pattern, pattern)) {
            const float size = std::exp2(room_size * 0.01f);
            const float first = size * std::min(pattern.left[0].ms, pattern.right[0].ms);
            const float last = std::max(pattern.left[pattern.left_count - 1].ms, pattern.right[pattern.right_count - 1].ms);
            const uint32_t span = std::max<uint32_t>(static_cast<uint32_t>(static_cast<int64_t>(fs * (size * last - first) * 0.001f)), 4);
            if (span < 0x10000) {
                er_length_ = span & 0xFFFCu;
                er_line_.assign(er_length_, 0.0f);
                auto build = [&](const ErTap* taps, size_t count, std::vector<std::pair<uint32_t, float>>& out) {
                    uint32_t previous = 0xFFFFFFFFu;
                    for (size_t i = 0; i < count; ++i) {
                        uint32_t d = static_cast<uint32_t>(static_cast<int64_t>(fs * (size * taps[i].ms - first) * 0.001f)) & 0xFFFFFFFCu;
                        if (er_length_ <= d) {
                            d = er_length_ - 4;
                        }
                        if (d != previous) {
                            out.push_back({d, taps[i].gain});
                        }
                        previous = d;
                    }
                };
                build(pattern.left, pattern.left_count, er_left_);
                build(pattern.right, pattern.right_count, er_right_);
                if (const uint32_t samples = static_cast<uint32_t>(static_cast<int64_t>(first * 0.001f * fs))) {
                    er_input_.Resize(samples);
                }
                if (const uint32_t samples = static_cast<uint32_t>(static_cast<int64_t>(front_back * 0.001f * fs))) {
                    er_rear_left_.Resize(samples);
                    er_rear_right_.Resize(samples);
                }
                er_enabled_ = true;
            }
        }
        if (tone) {
            for (int i = 0; i < 3; ++i) {
                if (position[i] == 0 || position[i] > 3 || (position[i] == 1 && !er_enabled_)) {
                    continue;
                }
                Tone t;
                t.position = position[i];
                t.left.Design(curve[i], filter_freq[i], filter_gain[i], filter_q[i]);
                t.right = t.left;
                tones_.push_back(t);
            }
        }
        dc_coef_ = 1.0f - dc_cut * 2.0f * kPi / fs;
        dc_gain_ = (units < 2 ? 1.0f : 1.0f / std::sqrt(static_cast<float>(units))) * 1.41421356f;
        const float w1 = width * 0.001627189f + 0.707106f;
        const float w2 = 1.0f - w1 * w1 > 0.0f ? std::sqrt(1.0f - w1 * w1) : 0.0f;
        constexpr float kFold = 1.41421356f;
        reverb_main_ = front * reverb * w1;
        reverb_cross_ = front * reverb * w2;
        er_main_ = er_enabled_ ? front * er * w1 : 0.0f;
        er_cross_ = er_enabled_ ? front * er * w2 : 0.0f;
        rear_main_ = kFold * rear * reverb * w1;
        rear_cross_ = kFold * rear * reverb * w2;
        rear_er_main_ = er_enabled_ ? kFold * rear * er * w1 : 0.0f;
        rear_er_cross_ = er_enabled_ ? kFold * rear * er * w2 : 0.0f;
        center_ = 0.70710678f * 0.707106f * center * reverb;
        tail += static_cast<uint32_t>(std::max(reverb_delay, 0.0f) * 0.001f * fs);
        for (const Diffuser& d : diffusers_) {
            tail += d.length;
        }
        tail_seconds_ = static_cast<float>(tail) / fs + decay * 1.2f + 0.05f;
    }

    bool MonoInput() const override { return true; }

    void Process(float* left, float* right, uint32_t frames) override {
        constexpr float kInputNorm = 0.40824829f;
        for (uint32_t i = 0; i < frames; ++i) {
            const float in_l = left[i];
            const float in_r = right[i];
            float x = (in_l + in_r) * kInputNorm;
            for (Tone& t : tones_) {
                if (t.position == 3) {
                    x = t.left.Process(x);
                }
            }
            float rev = predelay_.Active() ? predelay_.Push(x) : x;
            float er_l = 0.0f;
            float er_r = 0.0f;
            float rear_l = 0.0f;
            float rear_r = 0.0f;
            if (er_enabled_) {
                const float er_in = er_input_.Active() ? er_input_.Push(x) : x;
                er_line_[er_pos_] = er_in;
                for (const auto& [d, g] : er_left_) {
                    er_l += g * er_line_[er_pos_ >= d ? er_pos_ - d : er_pos_ + er_length_ - d];
                }
                for (const auto& [d, g] : er_right_) {
                    er_r += g * er_line_[er_pos_ >= d ? er_pos_ - d : er_pos_ + er_length_ - d];
                }
                if (++er_pos_ == er_length_) {
                    er_pos_ = 0;
                }
                for (Tone& t : tones_) {
                    if (t.position == 1) {
                        er_l = t.left.Process(er_l);
                        er_r = t.right.Process(er_r);
                    }
                }
                rear_l = er_rear_left_.Active() ? er_rear_left_.Push(er_l) : er_l;
                rear_r = er_rear_right_.Active() ? er_rear_right_.Push(er_r) : er_r;
            }
            for (Diffuser& d : diffusers_) {
                float* slot = d.history.data() + static_cast<size_t>(d.pos) * 2;
                const float y = slot[0] + (rev - slot[1]) * d.gain;
                slot[0] = rev;
                slot[1] = y;
                rev = y;
                if (++d.pos == d.length) {
                    d.pos = 0;
                }
            }
            for (Tone& t : tones_) {
                if (t.position == 2) {
                    rev = t.left.Process(rev);
                }
            }
            float bus[6] = {};
            for (size_t u = 0; u < units_.size(); ++u) {
                Unit& unit = units_[u];
                rev = unit.input.Push(rev);
                float s[4];
                for (int j = 0; j < 4; ++j) {
                    const float out = unit.lines[j].Read();
                    unit.state[j] = unit.pole[j] * unit.state[j] + unit.feed[j] * out;
                    s[j] = unit.state[j];
                }
                const float half = -0.5f * (s[0] + s[1] + s[2] + s[3]);
                unit.lines[0].Write(s[1] + half + rev);
                unit.lines[1].Write(s[2] + half + rev);
                unit.lines[2].Write(s[3] + half + rev);
                unit.lines[3].Write(s[0] + half + rev);
                const size_t odd = u & 1;
                bus[odd] += s[0] - s[1] + s[2] - s[3];
                bus[2 + odd] += s[0] + s[1] - s[2] - s[3];
                bus[4 + odd] += s[0] - s[1] - s[2] + s[3];
            }
            for (int k = 0; k < 6; ++k) {
                const float v = dc_gain_ * bus[k];
                dc_[k][1] = dc_coef_ * dc_[k][1] + (v - dc_[k][0]);
                dc_[k][0] = v;
                bus[k] = dc_[k][1];
            }
            const float center = center_ * (bus[2] + bus[3]);
            const float out_l = reverb_main_ * bus[0] + reverb_cross_ * bus[1] + er_main_ * er_l + er_cross_ * er_r + center +
                                rear_main_ * bus[4] + rear_cross_ * bus[5] + rear_er_main_ * rear_l + rear_er_cross_ * rear_r;
            const float out_r = reverb_cross_ * bus[0] + reverb_main_ * bus[1] + er_cross_ * er_l + er_main_ * er_r + center +
                                rear_cross_ * bus[4] + rear_main_ * bus[5] + rear_er_cross_ * rear_l + rear_er_main_ * rear_r;
            left[i] = in_l * dry_gain_ + out_l;
            right[i] = in_r * dry_gain_ + out_r;
        }
    }

    float TailSeconds() const override { return tail_seconds_; }

    void MuteDry() override { dry_gain_ = 0.0f; }

    void Reset() override {
        for (Unit& unit : units_) {
            for (auto& line : unit.lines) {
                line.Clear();
            }
            unit.input.Clear();
            std::fill(std::begin(unit.state), std::end(unit.state), 0.0f);
        }
        for (Diffuser& d : diffusers_) {
            std::fill(d.history.begin(), d.history.end(), 0.0f);
            d.pos = 0;
        }
        predelay_.Clear();
        er_input_.Clear();
        er_rear_left_.Clear();
        er_rear_right_.Clear();
        std::fill(er_line_.begin(), er_line_.end(), 0.0f);
        er_pos_ = 0;
        for (Tone& t : tones_) {
            t.left.Clear();
            t.right.Clear();
        }
        for (auto& dc : dc_) {
            dc[0] = dc[1] = 0.0f;
        }
    }

private:
    class Line {
    public:
        void Resize(uint32_t length) {
            buffer_.assign(std::max<uint32_t>(length, 4), 0.0f);
            pos_ = 0;
        }
        float Read() const { return buffer_[pos_]; }
        void Write(float x) {
            buffer_[pos_] = x;
            if (++pos_ == buffer_.size()) {
                pos_ = 0;
            }
        }
        void Clear() { std::fill(buffer_.begin(), buffer_.end(), 0.0f); }

    private:
        std::vector<float> buffer_;
        size_t pos_ = 0;
    };

    struct Unit {
        uint32_t length[4] = {};
        Line lines[4];
        float feed[4] = {};
        float pole[4] = {};
        float state[4] = {};
        SampleDelay input;
    };

    struct Diffuser {
        uint32_t length = 4;
        uint32_t pos = 0;
        float gain = 0.0f;
        std::vector<float> history;
    };

    struct Tone {
        uint32_t position = 0;
        WwiseBiquad left;
        WwiseBiquad right;
    };

    std::vector<Unit> units_;
    Diffuser diffusers_[4];
    SampleDelay predelay_;
    bool er_enabled_ = false;
    uint32_t er_length_ = 0;
    uint32_t er_pos_ = 0;
    std::vector<float> er_line_;
    std::vector<std::pair<uint32_t, float>> er_left_;
    std::vector<std::pair<uint32_t, float>> er_right_;
    SampleDelay er_input_;
    SampleDelay er_rear_left_;
    SampleDelay er_rear_right_;
    std::vector<Tone> tones_;
    float dc_[6][2] = {};
    float dc_coef_ = 0.0f;
    float dc_gain_ = 1.0f;
    float dry_gain_ = 1.0f;
    float reverb_main_ = 0.0f;
    float reverb_cross_ = 0.0f;
    float er_main_ = 0.0f;
    float er_cross_ = 0.0f;
    float rear_main_ = 0.0f;
    float rear_cross_ = 0.0f;
    float rear_er_main_ = 0.0f;
    float rear_er_cross_ = 0.0f;
    float center_ = 0.0f;
    float tail_seconds_ = 1.0f;
};

// Wwise Matrix Reverb as the eboot's plug-in (0x73: FX 0x5B8550, parameters 0x5C4C40, SetParamsBlock 0x5C4DD0). It runs on the bus's
// 7.1 channels: the lines take the sum of the channels (without the LFE unless processLFE), each channel gets its own signed sum of
// the lines (0x13B98A0), and the port folds those outputs to stereo the way it folds voices.
class MatrixReverb : public Effect {
public:
    explicit MatrixReverb(const std::vector<uint8_t>& params) {
        ParamReader r(params);
        const float reverb_time = r.F32();
        const float hf_ratio = r.F32();
        count_ = r.U32();
        dry_gain_ = std::pow(10.0f, r.F32() * 0.05f);
        wet_gain_ = std::pow(10.0f, r.F32() * 0.05f);
        const float pre_delay = r.F32();
        r.U8();
        const uint32_t mode = r.U32();
        // 0x5B8770: 4, 8, 12 or 16 lines; any other count leaves the plug-in without a process function
        /* Wwise Matrix Reverb has process functions for 4, 8, 12 or 16 lines only; any other count leaves the plug-in silent (0x5B8770). */
        if (count_ != 4 && count_ != 8 && count_ != 12 && count_ != 16) {
            count_ = 0;
            return;
        }
        reverb_time_ = reverb_time;
        pre_delay_ = pre_delay;
        std::array<float, 16> delay_ms{};
        for (uint32_t i = 0; i < count_; ++i) {
            const float custom = r.F32();
            delay_ms[i] = mode == 1 ? custom : kDefaultDelayMs[i];
        }
        // each length is the first odd number from rate x time on without a divisor from 3 up to its root (0x5B8770), then sorted
        for (uint32_t i = 0; i < count_; ++i) {
            uint32_t n = static_cast<uint32_t>(static_cast<int64_t>(delay_ms[i] * 0.001f * static_cast<float>(kOutputRate)));
            n += (n & 1u) ^ 1u;
            const int limit = static_cast<int>(std::sqrt(static_cast<double>(n))) + 1;
            for (int d = 3; d < limit;) {
                if (n % static_cast<uint32_t>(d) == 0) {
                    n += 2;
                    d = 3;
                } else {
                    d += 2;
                }
            }
            lengths_[i] = n;
        }
        std::sort(lengths_.begin(), lengths_.begin() + count_);
        for (uint32_t i = 0; i < count_; ++i) {
            lines_[i].assign(lengths_[i], 0.0f);
        }
        // 0x11AB840: line gain g = 0.001 ^ (length / rate / reverb time) and a one-pole low pass b per line (Jot), with
        // b = (1 - hfRatio^2) log10(g) ln(10) / 4, the factor clamped so that the longest line's b is at most 1
        const double inv_rate = 1.0 / static_cast<double>(kOutputRate);
        const double ln10_4 = std::log(10.0) / 4.0;
        double k = 1.0 / static_cast<double>(hf_ratio);
        k = 1.0 - 1.0 / (k * k);
        const double lg_last = std::log10(std::pow(0.001, inv_rate * lengths_[count_ - 1] / static_cast<double>(reverb_time)));
        if (1.0 < k * lg_last * ln10_4) {
            k = 1.0 / (lg_last * ln10_4);
        }
        for (uint32_t i = 0; i < count_; ++i) {
            const double g = std::pow(0.001, inv_rate * lengths_[i] / static_cast<double>(reverb_time));
            const double b = k * std::log10(g) * ln10_4;
            feed_[i] = static_cast<float>(g * (1.0 - b));
            pole_[i] = static_cast<float>(b);
        }
        // 0x5B8EE0: tone correction (1 - beta z^-1) / (1 - beta), beta = (1 - 1 / hfRatio) / (1 + 1 / hfRatio)
        const float inv_hf = 1.0f / hf_ratio;
        const float beta = (1.0f - inv_hf) / (inv_hf + 1.0f);
        tone_gain_ = 1.0f / (1.0f - beta);
        tone_prev_gain_ = -beta / (1.0f - beta);
        // 0x5B86C0: a DC blocker at 10 Hz and the pre-delay in whole samples
        dc_pole_ = -62.831856f / static_cast<float>(kOutputRate) + 1.0f;
        pre_delay_line_.assign(static_cast<size_t>(std::max<int64_t>(0, static_cast<int64_t>(static_cast<float>(kOutputRate) * pre_delay))), 0.0f);
        // rows 0 to 6: FL, FR, FC and the four surrounds of the bus (a Wwise buffer keeps the LFE last), folded like the voices
        const float fold = 0.70710678f;
        for (uint32_t j = 0; j < count_; ++j) {
            fold_l_[j] = kOutputSigns[0][j] + fold * kOutputSigns[2][j] + fold * kOutputSigns[3][j] + fold * kOutputSigns[5][j];
            fold_r_[j] = kOutputSigns[1][j] + fold * kOutputSigns[2][j] + fold * kOutputSigns[4][j] + fold * kOutputSigns[6][j];
        }
        feedback_ = -2.0f / static_cast<float>(count_);
    }

    void Process(float* left, float* right, uint32_t frames) override {
        if (count_ == 0) {
            return;
        }
        std::array<float, 16> f{};
        for (uint32_t i = 0; i < frames; ++i) {
            // the lines' outputs through their low passes
            float out_l = 0.0f;
            float out_r = 0.0f;
            float sum = 0.0f;
            for (uint32_t j = 0; j < count_; ++j) {
                state_[j] = pole_[j] * state_[j] + feed_[j] * lines_[j][pos_[j]];
                f[j] = state_[j];
                out_l += f[j] * fold_l_[j];
                out_r += f[j] * fold_r_[j];
                sum += f[j];
            }
            // the lines take the sum of the bus channels (SumInput: a send carries the sum of the voice's 7.1 gains)
            const float in = left[i] + right[i];
            left[i] = dry_gain_ * left[i] + wet_gain_ * out_l;
            right[i] = dry_gain_ * right[i] + wet_gain_ * out_r;
            const float dc = dc_pole_ * dc_out_ + in - dc_in_;
            dc_in_ = in;
            dc_out_ = dc;
            float delayed = dc;
            if (!pre_delay_line_.empty()) {
                delayed = pre_delay_line_[pre_pos_];
                pre_delay_line_[pre_pos_] = dc;
                if (++pre_pos_ == pre_delay_line_.size()) {
                    pre_pos_ = 0;
                }
            }
            const float v = tone_prev_gain_ * tone_prev_ + tone_gain_ * delayed;
            tone_prev_ = delayed;
            // Householder feedback, each line fed from the next one (the lane shuffle of the process functions)
            const float c = feedback_ * sum;
            for (uint32_t j = 0; j < count_; ++j) {
                lines_[j][pos_[j]] = f[j + 1 == count_ ? 0 : j + 1] + c + v;
                if (++pos_[j] == lengths_[j]) {
                    pos_[j] = 0;
                }
            }
        }
    }

    // 0x11AA480 keeps the effect running for rate x reverb time samples after its input
    float TailSeconds() const override { return reverb_time_ + pre_delay_; }

    void MuteDry() override { dry_gain_ = 0.0f; }

    bool SumInput() const override { return true; }

    void Reset() override {
        for (uint32_t j = 0; j < count_; ++j) {
            std::fill(lines_[j].begin(), lines_[j].end(), 0.0f);
            pos_[j] = 0;
            state_[j] = 0.0f;
        }
        std::fill(pre_delay_line_.begin(), pre_delay_line_.end(), 0.0f);
        pre_pos_ = 0;
        tone_prev_ = 0.0f;
        dc_in_ = dc_out_ = 0.0f;
    }

private:
    // 0x13B9AA0, milliseconds
    static constexpr float kDefaultDelayMs[16] = {13.62f, 15.66f, 17.52f, 19.02f, 20.83f, 22.6f, 24.05f, 24.78f,
                                                  25.6f,  26.09f, 26.55f, 26.91f, 28.04f, 29.09f, 29.9f,  30.86f};
    // 0x13B98A0: the sign of each line in the output of each channel
    static constexpr float kOutputSigns[7][16] = {
        {1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1},
        {1, 1, -1, -1, 1, 1, -1, -1, 1, 1, -1, -1, 1, 1, -1, -1},
        {-1, 1, 1, -1, -1, 1, 1, -1, -1, 1, 1, -1, -1, 1, 1, -1},
        {-1, -1, -1, 1, 1, 1, -1, 1, -1, -1, 1, 1, 1, 1, -1, -1},
        {1, -1, -1, -1, 1, 1, -1, 1, 1, -1, -1, 1, -1, 1, -1, 1},
        {1, -1, 1, 1, 1, -1, -1, -1, -1, 1, 1, -1, -1, -1, 1, 1},
        {1, 1, -1, 1, -1, -1, -1, 1, -1, 1, -1, 1, 1, -1, 1, -1},
    };

    uint32_t count_ = 0;
    float reverb_time_ = 0.0f;
    float pre_delay_ = 0.0f;
    float dry_gain_ = 1.0f;
    float wet_gain_ = 1.0f;
    float feedback_ = 0.0f;
    float tone_gain_ = 1.0f;
    float tone_prev_gain_ = 0.0f;
    float tone_prev_ = 0.0f;
    float dc_pole_ = 1.0f;
    float dc_in_ = 0.0f;
    float dc_out_ = 0.0f;
    std::array<uint32_t, 16> lengths_{};
    std::array<std::vector<float>, 16> lines_{};
    std::array<uint32_t, 16> pos_{};
    std::array<float, 16> feed_{};
    std::array<float, 16> pole_{};
    std::array<float, 16> state_{};
    std::array<float, 16> fold_l_{};
    std::array<float, 16> fold_r_{};
    std::vector<float> pre_delay_line_;
    size_t pre_pos_ = 0;
};

class StereoDelay : public Effect {
public:
    explicit StereoDelay(const std::vector<uint8_t>& params) {
        ParamReader r(params);
        left_input_ = r.U32();
        const float left_time = r.F32();
        left_feedback_ = DbToGain(r.F32());
        left_cross_ = DbToGain(r.F32());
        right_input_ = r.U32();
        const float right_time = r.F32();
        right_feedback_ = DbToGain(r.F32());
        right_cross_ = DbToGain(r.F32());
        const uint32_t filter_type = r.U32();
        const float filter_gain = r.F32();
        const float filter_freq = r.F32();
        const float filter_q = r.F32();
        dry_gain_ = DbToGain(r.F32());
        wet_gain_ = DbToGain(r.F32());
        const float balance = r.F32();
        const bool enable_feedback = r.U8() != 0;
        const bool enable_cross = r.U8() != 0;
        if (!enable_feedback) {
            left_feedback_ = right_feedback_ = 0.0f;
        }
        if (!enable_cross) {
            left_cross_ = right_cross_ = 0.0f;
        }
        // 0x5D9780, 0x5DA540
        auto length = [](float seconds) {
            const float t = std::max(seconds, 1024.0f / static_cast<float>(kOutputRate));
            return (static_cast<uint32_t>(std::floor(t * static_cast<float>(kOutputRate))) + 3u) & ~3u;
        };
        line_l_.assign(length(left_time), 0.0f);
        line_r_.assign(length(right_time), 0.0f);
        if (filter_type != 0) {
            filter_on_ = true;
            filter_l_.Design(filter_type - 1, filter_freq, filter_gain, filter_q);
            filter_r_ = filter_l_;
        }
        const float d = std::clamp((balance + 100.0f) * 0.005f, 0.0f, 1.0f);
        front_ = std::sqrt(1.0f - d);
        rear_ = std::sqrt(d) * 0.70710678f;
        // 0x5D99D0
        auto decay = [](float gain) { return std::min(20.0f * std::log10(std::max(gain, 1e-9f)), -0.1f); };
        float tail = std::max(enable_feedback ? -60.0f / decay(left_feedback_) * left_time : left_time,
                              enable_feedback ? -60.0f / decay(right_feedback_) * right_time : right_time);
        if (enable_cross) {
            tail += -60.0f / decay(left_cross_ * right_cross_) * (enable_feedback ? 2.0f : 1.0f) * (left_time + right_time);
        }
        tail_ = std::min(tail, 60.0f);
    }

    bool FrontInput() const override { return left_input_ == 0 && right_input_ == 0; }

    // 0x62A990, delay lines 0x5DA7D0
    void Process(float* left, float* right, uint32_t frames) override {
        for (uint32_t i = 0; i < frames; ++i) {
            const float in_l = Input(left_input_, left[i], right[i], left[i]);
            const float in_r = Input(right_input_, left[i], right[i], right[i]);
            const float dl = line_l_[pos_l_];
            const float dr = line_r_[pos_r_];
            float wl = in_l + left_feedback_ * dl + right_cross_ * dr;
            float wr = in_r + right_feedback_ * dr + left_cross_ * dl;
            if (filter_on_) {
                wl = filter_l_.Process(wl);
                wr = filter_r_.Process(wr);
            }
            line_l_[pos_l_] = wl;
            line_r_[pos_r_] = wr;
            if (++pos_l_ == line_l_.size()) {
                pos_l_ = 0;
            }
            if (++pos_r_ == line_r_.size()) {
                pos_r_ = 0;
            }
            const float wet_l = wet_gain_ * (front_ * dl + rear_ * dl);
            const float wet_r = wet_gain_ * (front_ * dr + rear_ * dr);
            left[i] = left[i] * dry_gain_ + wet_l;
            right[i] = right[i] * dry_gain_ + wet_r;
        }
    }

    float TailSeconds() const override { return tail_; }

    void MuteDry() override { dry_gain_ = 0.0f; }

    void Reset() override {
        std::fill(line_l_.begin(), line_l_.end(), 0.0f);
        std::fill(line_r_.begin(), line_r_.end(), 0.0f);
        pos_l_ = pos_r_ = 0;
        filter_l_.Clear();
        filter_r_.Clear();
    }

private:
    static float Input(uint32_t type, float l, float r, float own) {
        switch (type) {
            case 0: return own;
            case 3: return 0.0f;
            default: return (l + r) * 0.5f;
        }
    }

    uint32_t left_input_ = 0;
    uint32_t right_input_ = 0;
    float left_feedback_ = 0.0f;
    float left_cross_ = 0.0f;
    float right_feedback_ = 0.0f;
    float right_cross_ = 0.0f;
    float dry_gain_ = 1.0f;
    float wet_gain_ = 1.0f;
    float front_ = 1.0f;
    float rear_ = 0.0f;
    std::vector<float> line_l_;
    std::vector<float> line_r_;
    size_t pos_l_ = 0;
    size_t pos_r_ = 0;
    bool filter_on_ = false;
    WwiseBiquad filter_l_;
    WwiseBiquad filter_r_;
    float tail_ = 1.0f;
};

// Peak Limiter (0x5AD9C0 setup, 0x5ADC60 execute, 0x5AE4B0 linked channels): the channel peak held for the lookahead, a dB envelope with
// attack over half the lookahead and the release time, gain 10^((1 / ratio - 1) * env / 20) on the input delayed by the lookahead
class PeakLimiter : public Effect {
public:
    explicit PeakLimiter(const std::vector<uint8_t>& params) {
        ParamReader r(params);
        threshold_db_ = r.F32();
        const float ratio = r.F32();
        const float lookahead = r.F32();
        const float release = r.F32();
        output_gain_ = std::pow(10.0f, r.F32() * 0.05f);
        lookahead_ = std::max<uint32_t>(1, static_cast<uint32_t>(static_cast<float>(kOutputRate) * lookahead));
        attack_coef_ = std::exp(-2.2f / (static_cast<float>(lookahead_) * 0.5f));
        release_coef_ = std::exp(-2.2f / (static_cast<float>(kOutputRate) * std::max(release, 1e-4f)));
        slope_ = static_cast<float>(static_cast<double>(1.0f / std::max(ratio, 1.0f) - 1.0f) * 0.05);
        delay_l_.assign(lookahead_, 0.0f);
        delay_r_.assign(lookahead_, 0.0f);
        for (auto& channel : delay_surround_) {
            channel.assign(lookahead_, 0.0f);
        }
    }

    void SetDetector(const float* peak) override { detector_ = peak; }

    void Process(float* left, float* right, uint32_t frames) override {
        // 0x5AE4B0 links the channels of the bus: the peak of a frame is the largest of its channels, which the engine hands over
        // for the master's 7.1 channels (SetDetector); without it the limiter detects its stereo input
        const float* detector = std::exchange(detector_, nullptr);
        auto peak_at = [&](uint32_t i) { return detector ? detector[i] : std::max(std::fabs(left[i]), std::fabs(right[i])); };
        if (first_ && frames > 0) {
            first_ = false;
            const uint32_t count = std::min(lookahead_, frames);
            for (uint32_t i = 0; i < count; ++i) {
                if (peak_at(i) > held_) {
                    held_ = peak_at(i);
                    hold_ = count - i;
                }
            }
            target_ = std::max(FastGainToDb(held_) - threshold_db_, 0.0f);
        }
        for (uint32_t i = 0; i < frames; ++i) {
            const float out_l = delay_l_[pos_];
            const float out_r = delay_r_[pos_];
            const float peak = peak_at(i);
            delay_l_[pos_] = left[i];
            delay_r_[pos_] = right[i];
            if (++pos_ == lookahead_) {
                pos_ = 0;
            }
            if (hold_ == 0 || held_ < peak) {
                held_ = peak;
                target_ = std::max(FastGainToDb(peak) - threshold_db_, 0.0f);
                hold_ = lookahead_;
            } else {
                --hold_;
            }
            const float coef = target_ - env_ < 0.0f ? release_coef_ : attack_coef_;
            env_ = target_ + (env_ - target_) * coef;
            const float gain = FastPow10(slope_ * env_);
            left[i] = out_l * gain;
            right[i] = out_r * gain;
        }
        if (output_gain_ != 1.0f) {
            for (uint32_t i = 0; i < frames; ++i) {
                left[i] *= output_gain_;
                right[i] *= output_gain_;
            }
        }
    }

    void ProcessSurround(float* left, float* right, std::array<float*, 8>& speakers, uint32_t frames) override {
        const float* detector = std::exchange(detector_, nullptr);
        auto peak_at = [&](uint32_t i) { return detector ? detector[i] : std::max(std::fabs(left[i]), std::fabs(right[i])); };
        if (first_ && frames > 0) {
            first_ = false;
            const uint32_t count = std::min(lookahead_, frames);
            for (uint32_t i = 0; i < count; ++i) {
                if (peak_at(i) > held_) {
                    held_ = peak_at(i);
                    hold_ = count - i;
                }
            }
            target_ = std::max(FastGainToDb(held_) - threshold_db_, 0.0f);
        }
        for (uint32_t i = 0; i < frames; ++i) {
            const uint32_t pos = pos_;
            const float out_l = delay_l_[pos];
            const float out_r = delay_r_[pos];
            std::array<float, 8> delayed{};
            for (size_t channel = 0; channel < delayed.size(); ++channel) {
                delayed[channel] = delay_surround_[channel][pos];
                delay_surround_[channel][pos] = speakers[channel][i];
            }
            const float peak = peak_at(i);
            delay_l_[pos] = left[i];
            delay_r_[pos] = right[i];
            if (++pos_ == lookahead_) pos_ = 0;
            if (hold_ == 0 || held_ < peak) {
                held_ = peak;
                target_ = std::max(FastGainToDb(peak) - threshold_db_, 0.0f);
                hold_ = lookahead_;
            } else {
                --hold_;
            }
            const float coef = target_ - env_ < 0.0f ? release_coef_ : attack_coef_;
            env_ = target_ + (env_ - target_) * coef;
            const float gain = FastPow10(slope_ * env_);
            left[i] = out_l * gain;
            right[i] = out_r * gain;
            for (size_t channel = 0; channel < delayed.size(); ++channel) {
                speakers[channel][i] = delayed[channel] * gain;
            }
        }
        if (output_gain_ != 1.0f) {
            for (uint32_t i = 0; i < frames; ++i) {
                left[i] *= output_gain_;
                right[i] *= output_gain_;
                for (float* channel : speakers) channel[i] *= output_gain_;
            }
        }
    }

    float TailSeconds() const override { return static_cast<float>(lookahead_ * 2) / kOutputRate + 0.02f; }

    void Reset() override {
        std::fill(delay_l_.begin(), delay_l_.end(), 0.0f);
        std::fill(delay_r_.begin(), delay_r_.end(), 0.0f);
        for (auto& channel : delay_surround_) std::fill(channel.begin(), channel.end(), 0.0f);
        pos_ = 0;
        env_ = 0.0f;
        held_ = 0.0f;
        hold_ = 0;
        target_ = 0.0f;
        first_ = true;
    }

private:
    const float* detector_ = nullptr;
    float threshold_db_ = 0.0f;
    float slope_ = -0.045f;
    float output_gain_ = 1.0f;
    float attack_coef_ = 0.0f;
    float release_coef_ = 0.0f;
    uint32_t lookahead_ = 1;
    std::vector<float> delay_l_;
    std::vector<float> delay_r_;
    std::array<std::vector<float>, 8> delay_surround_;
    uint32_t pos_ = 0;
    float env_ = 0.0f;
    float held_ = 0.0f;
    uint32_t hold_ = 0;
    float target_ = 0.0f;
    bool first_ = true;
};

}

float DbToGain(float db) {
    return db <= -144.0f ? 0.0f : std::pow(10.0f, db / 20.0f);
}

// 0x5F2450, 0x5AE4B0: 10^x with the exponent from the scaled integer and a cubic for the mantissa
float FastPow10(float x) {
    if (x < -37.0f) {
        return 0.0f;
    }
    const uint32_t bits = static_cast<uint32_t>(static_cast<int64_t>(x * 27866352.0f + 1.0653532e9f));
    const uint32_t mantissa_bits = (bits & 0x7FFFFFu) | 0x3F800000u;
    const uint32_t exponent_bits = bits & 0xFF800000u;
    float m;
    float e;
    std::memcpy(&m, &mantissa_bits, sizeof(m));
    std::memcpy(&e, &exponent_bits, sizeof(e));
    return e * (m * (m * 0.32518977f + 0.020805772f) + 0.65304345f);
}

// 0x5AE4B0, 0x603230, 0x11A8BB0: 20 * log10(x) from the float's exponent and a series in its mantissa
float FastGainToDb(float x) {
    uint32_t bits;
    std::memcpy(&bits, &x, sizeof(bits));
    const uint32_t mantissa_bits = (bits & 0x7FFFFFu) | 0x3F800000u;
    float m;
    std::memcpy(&m, &mantissa_bits, sizeof(m));
    const float s = (m - 1.0f) / (m + 1.0f);
    const float e = (static_cast<float>(static_cast<int32_t>(bits >> 23 & 0xFFu)) - 127.0f) * 0.6931472f;
    return ((s + s) * (s * s * 0.33333334f + 1.0f) + e) * 8.68589f;
}

float GainToDb(float gain) {
    return gain <= 1e-8f ? -160.0f : 20.0f * std::log10(gain);
}

float LpfToCutoffHz(float lpf) {
    lpf = std::clamp(lpf, 0.0f, 100.0f);
    if (lpf < 30.0f) {
        return (30.0f - lpf) * 433.33334f + 7000.0f;
    }
    return 16.797443f * std::exp2((100.0f - lpf) * 0.12432813f);
}

void ButterworthLowPass(float cutoff_hz, float* coef) {
    const float w = cutoff_hz / static_cast<float>(kOutputRate) <= 0.45f ? cutoff_hz / static_cast<float>(kOutputRate) * kPi : 0.45f * kPi;
    const float k = 1.0f / std::tan(w);
    const float kk = k * k;
    const float a0 = 1.0f / (kk + 1.41421356f * k + 1.0f);
    coef[0] = a0;
    coef[1] = 2.0f * a0;
    coef[2] = a0;
    coef[3] = 2.0f * a0 * (1.0f - kk);
    coef[4] = a0 * (kk - 1.41421356f * k + 1.0f);
}

float OnePoleCoefficient(float cutoff_hz) {
    if (cutoff_hz >= 0.45f * kOutputRate) {
        return 1.0f;
    }
    return 1.0f - std::exp(-2.0f * kPi * cutoff_hz / kOutputRate);
}

void Biquad::Set(Type type, float freq, float q, float gain_db) {
    const float w0 = 2.0f * kPi * std::clamp(freq, 1.0f, 0.49f * kOutputRate) / kOutputRate;
    const float cw = std::cos(w0);
    const float sw = std::sin(w0);
    const float alpha = sw / (2.0f * std::max(q, 0.01f));
    const float a = std::pow(10.0f, gain_db / 40.0f);
    float nb0 = 1.0f;
    float nb1 = 0.0f;
    float nb2 = 0.0f;
    float na0 = 1.0f;
    float na1 = 0.0f;
    float na2 = 0.0f;
    switch (type) {
        case Type::LowPass:
            nb0 = (1.0f - cw) * 0.5f;
            nb1 = 1.0f - cw;
            nb2 = nb0;
            na0 = 1.0f + alpha;
            na1 = -2.0f * cw;
            na2 = 1.0f - alpha;
            break;
        case Type::HighPass:
            nb0 = (1.0f + cw) * 0.5f;
            nb1 = -(1.0f + cw);
            nb2 = nb0;
            na0 = 1.0f + alpha;
            na1 = -2.0f * cw;
            na2 = 1.0f - alpha;
            break;
        case Type::BandPass:
            nb0 = alpha;
            nb1 = 0.0f;
            nb2 = -alpha;
            na0 = 1.0f + alpha;
            na1 = -2.0f * cw;
            na2 = 1.0f - alpha;
            break;
        case Type::Notch:
            nb0 = 1.0f;
            nb1 = -2.0f * cw;
            nb2 = 1.0f;
            na0 = 1.0f + alpha;
            na1 = -2.0f * cw;
            na2 = 1.0f - alpha;
            break;
        case Type::Peaking:
            nb0 = 1.0f + alpha * a;
            nb1 = -2.0f * cw;
            nb2 = 1.0f - alpha * a;
            na0 = 1.0f + alpha / a;
            na1 = -2.0f * cw;
            na2 = 1.0f - alpha / a;
            break;
        case Type::LowShelf: {
            const float sa = 2.0f * std::sqrt(a) * alpha;
            nb0 = a * ((a + 1.0f) - (a - 1.0f) * cw + sa);
            nb1 = 2.0f * a * ((a - 1.0f) - (a + 1.0f) * cw);
            nb2 = a * ((a + 1.0f) - (a - 1.0f) * cw - sa);
            na0 = (a + 1.0f) + (a - 1.0f) * cw + sa;
            na1 = -2.0f * ((a - 1.0f) + (a + 1.0f) * cw);
            na2 = (a + 1.0f) + (a - 1.0f) * cw - sa;
            break;
        }
        case Type::HighShelf: {
            const float sa = 2.0f * std::sqrt(a) * alpha;
            nb0 = a * ((a + 1.0f) + (a - 1.0f) * cw + sa);
            nb1 = -2.0f * a * ((a - 1.0f) + (a + 1.0f) * cw);
            nb2 = a * ((a + 1.0f) + (a - 1.0f) * cw - sa);
            na0 = (a + 1.0f) - (a - 1.0f) * cw + sa;
            na1 = 2.0f * ((a - 1.0f) - (a + 1.0f) * cw);
            na2 = (a + 1.0f) - (a - 1.0f) * cw - sa;
            break;
        }
    }
    b0 = nb0 / na0;
    b1 = nb1 / na0;
    b2 = nb2 / na0;
    a1 = na1 / na0;
    a2 = na2 / na0;
}

void DelayLine::Resize(size_t length) {
    buffer_.assign(std::max<size_t>(length, 1), 0.0f);
    pos_ = 0;
}

void DelayLine::Clear() {
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
}

std::unique_ptr<Effect> CreateEffect(const FxObject& fx) {
    switch (fx.plugin_id) {
        case codec::RoomVerb: return std::make_unique<RoomVerb>(fx.params);
        case codec::MatrixReverb: return std::make_unique<MatrixReverb>(fx.params);
        case codec::StereoDelay: return std::make_unique<StereoDelay>(fx.params);
        case codec::PeakLimiter: return std::make_unique<PeakLimiter>(fx.params);
        default: return nullptr;
    }
}

}
