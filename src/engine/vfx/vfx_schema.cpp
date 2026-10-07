#include "engine/vfx/vfx_schema.h"

#include <algorithm>
#include <array>

namespace pt::vfx {

namespace {

constexpr PropDef kFxAddPoolVectorNode[] = {{0xDD140AE7, 2}, {0xD791673F, 2}, {0x53C923A7, 2}, {0x430E9E8E, 1}, {0x25D54FF0, 4}};
constexpr PropDef kFxApplyRandomVectorNode[] = {{0xEC22E2C7, 1}, {0x05FFD4EB, 1}};
constexpr PropDef kFxBulletLineProgramEffectNode[] = {
    {0x3D1BF456, 2}, {0x9F412E57, 2}, {0x6A5FA950, 2}, {0xD310A646, 2}, {0xB4F75F03, 2}, {0xAB20DE36, 2}, {0x31985701, 2}, {0x094FB2B9, 2},
    {0x0EB57E93, 2}, {0x2C62EB6A, 2}, {0x61A6E2F0, 2}, {0xD43DE798, 3}, {0x7636E1F6, 1}, {0x217CE1B0, 2}, {0x362A038B, 2}, {0xE6832126, 2},
    {0x4111B159, 2}, {0x28AC37D4, 2}, {0xFA572C45, 2}, {0xEEE69CCA, 0}, {0xBE3BD61E, 0}, {0x7CA4AE7B, 0}, {0x39441A80, 0}, {0x612D357A, 1},
    {0x33FE189E, 1}, {0x8941E3B3, 1}, {0x5FEFA472, 1}, {0x3A9AC68E, 2}, {0x302CE9CF, 0}, {0xECCE68E7, 1}, {0x1B98FED9, 1}, {0xC8BC49E6, 3},
    {0x8AB1CA2B, 2}, {0x38A73E2B, 2}, {0x1118DD61, 2}, {0x21738C2A, 4}, {0xE5ACF62A, 2}, {0xF1DDC1EE, 2}, {0x9BF77157, 2}, {0xF8CE4CAA, 2},
    {0xFC08FF2F, 2}, {0xE2937ED7, 2}, {0xAB96FA51, 2}, {0x4067A508, 0}, {0xB65BBD54, 0}, {0xEC22E2C7, 1}, {0x05FFD4EB, 1}, {0xE9DAB877, 4},
    {0x48428ECB, 1}, {0xCF9D35A1, 1}, {0x6739DFC3, 2}, {0x283A241E, 2}, {0xAD7105FB, 2}, {0x328263C6, 0}, {0xC5BFC21D, 2}, {0xD704532B, 2},
    {0x77DEDF8D, 1}, {0x781AAC2C, 1}, {0xAE8F8BA3, 2}, {0x7127EB8E, 2}, {0xC00B4A79, 2}, {0xA90978FB, 2}, {0x5E28DEE9, 0}, {0x72F33CA7, 4},
    {0xCFBDAC7B, 1}, {0xA74BE2E8, 2}, {0x1B7F9286, 2}, {0xF4DA0FEC, 2}
};
constexpr PropDef kFxCameraAngleVectorNode[] = {{0x506F061C, 1}, {0xEECAF4DF, 0}};
constexpr PropDef kFxCameraCorrectionMaterialNode[] = {
    {0x7636E1F6, 1}, {0x2435A351, 2}, {0xBDEBC3F4, 2}, {0x3C826A5C, 2}, {0x25EF42E3, 2}, {0x03ADA714, 2}, {0x779A8842, 2}, {0x75546309, 2},
    {0x97E06A7B, 2}, {0xFB8F3EFF, 2}, {0x2296332E, 2}, {0xDE6A379F, 2}, {0x25C95427, 2}, {0x68E43C74, 2}, {0x6AA1ACFC, 2}, {0xFEB64AD5, 2},
    {0x02ED8BF5, 2}, {0x1C9DB76F, 2}, {0x8FAFF419, 2}, {0x9DE24629, 2}, {0x164A2569, 2}, {0x972FD939, 2}, {0x1118DD61, 2}, {0x7BF7756D, 2},
    {0x48428ECB, 1}, {0x0E3C1540, 2}, {0x72F33CA7, 4}
};
constexpr PropDef kFxCameraCorrectionVectorNode[] = {
    {0x3BF5868A, 2}, {0xE210591C, 1}, {0xBA7C713E, 2}, {0x22D34390, 0}, {0xB4A484CB, 2}, {0x56C2B781, 2}, {0x8C3C9820, 2}, {0x78176DC8, 2},
    {0x3E251573, 2}
};
constexpr PropDef kFxCameraFollowVectorNode[] = {{0x73D22AD2, 3}, {0x1ACEBD89, 0}, {0x849D3E0C, 0}};
constexpr PropDef kFxCenterDistRateVectorNode[] = {
    {0xC73B3191, 0}, {0xABD319EB, 0}, {0xE4D092DD, 0}, {0x2F51BCE2, 0}, {0x6FC9EBDD, 3}, {0x7E85E8D8, 3}, {0x4819755F, 3}, {0x3CEF4C99, 3},
    {0x38B1A286, 4}
};
constexpr PropDef kFxCenterScrollVectorNode[] = {{0xAA90B6B2, 0}, {0x40EB719E, 4}, {0x91E731AD, 3}};
constexpr PropDef kFxCheckLightInterceptionVectorNode[] = {{0x4C8B8306, 0}, {0x25A4D811, 1}, {0x527D0675, 2}, {0x96CE333D, 0}, {0x654A17E9, 1}};
constexpr PropDef kFxChildEmitVectorNode[] = {{0xE4B7906D, 4}, {0xF68E107E, 2}, {0x4DC95B4E, 4}, {0x6F37DB13, 4}};
constexpr PropDef kFxCloneShapeNode[] = {
    {0x9E69F4A9, 3}, {0xA850262D, 1}, {0x1CEC027A, 0}, {0xB0A863CB, 0}, {0xF825ECD8, 0}, {0x8CC43F28, 3}, {0x3A424C45, 3}, {0x5C1B5DF3, 6},
    {0x9EAFFD2A, 1}
};
constexpr PropDef kFxCollisionCheckVectorNode[] = {{0x9BFBFF91, 4}, {0xBEDD7B1A, 1}, {0x4DC95B4E, 4}, {0xB45EFCBE, 2}, {0x6F37DB13, 4}};
constexpr PropDef kFxColorVectorNode[] = {{0x78F4ACF2, 3}};
constexpr PropDef kFxCompositionVectorNode[] = {{0x6FE5383F, 2}, {0x6D3F4A3F, 0}, {0xF3E0E378, 0}, {0x970C0E28, 0}, {0x5D6AF65B, 0}};
constexpr PropDef kFxConstLifeNode[] = {{0x33FE189E, 1}};
constexpr PropDef kFxConstNumEmitNode[] = {{0xB07EC7A5, 1}};
constexpr PropDef kFxConstScaleVectorNode[] = {{0x47A7A4D1, 0}, {0xC4B7E974, 0}, {0xB79D2D2F, 0}, {0x78669CAC, 0}, {0xC0883E50, 2}};
constexpr PropDef kFxConstVectorNode[] = {{0x4F41F8F8, 2}, {0x1F695AD3, 0}, {0x48ADB96A, 3}, {0x2F55184A, 1}};
constexpr PropDef kFxConstantMaterialNode[] = {
    {0x7636E1F6, 1}, {0x217CE1B0, 2}, {0x362A038B, 2}, {0x1C9DB76F, 2}, {0x8FAFF419, 2}, {0x9DE24629, 2}, {0x164A2569, 2}, {0x972FD939, 2},
    {0x1118DD61, 2}, {0xB65BBD54, 0}, {0x1364CE7E, 2}, {0xACB0CDBB, 2}, {0x6B880E78, 2}, {0xF1A308EB, 2}, {0x99EA2F62, 2}, {0x1F7E7CF7, 2},
    {0x27F25FE0, 2}, {0xF77D9DEA, 4}, {0xFBFFDA19, 2}, {0x48428ECB, 1}, {0x0E3C1540, 2}, {0xBB53E22D, 2}, {0x344C4F9B, 1}, {0xD7FBBE28, 1},
    {0x99C0EF09, 0}, {0xBAF73B2F, 0}, {0x72F33CA7, 4}
};
constexpr PropDef kFxDecalShapeNode[] = {
    {0x9E69F4A9, 3}, {0x36998A19, 2}, {0xA850262D, 1}, {0x800A313F, 0}, {0x5AFF77F2, 0}, {0x4C8B8306, 0}, {0x1CEC027A, 0}, {0x999DC0F3, 4},
    {0xF825ECD8, 0}, {0x8CC43F28, 3}, {0x3A424C45, 3}, {0x888E606F, 1}, {0x5C1B5DF3, 6}, {0xD0343D4B, 2}, {0x2D058C6C, 4}, {0x9EAFFD2A, 1},
    {0x88F0F0A9, 1}, {0xA35A373D, 2}, {0x44178224, 2}, {0xC31A37A7, 0}, {0xA28A6C30, 3}, {0x48428ECB, 1}, {0xA90978FB, 2}, {0xC6CC8CEF, 4},
    {0x72F33CA7, 4}, {0x7F916B75, 0}
};
constexpr PropDef kFxDelayNumEmitNode[] = {{0x48503BF9, 1}, {0x4CD9C31F, 1}, {0x33FE189E, 1}, {0x0A539A6A, 1}, {0xEC22E2C7, 1}, {0x05FFD4EB, 1}};
constexpr PropDef kFxDistanceEmitVectorNode[] = {{0x9AA63A20, 2}};
constexpr PropDef kFxDistanceNumEmitNode[] = {{0x9AA63A20, 2}, {0x996A1E16, 1}};
constexpr PropDef kFxDistortionMaterialNode[] = {{0x28AC37D4, 2}, {0xFA572C45, 2}, {0x50E78B53, 0}, {0x0E3C1540, 2}, {0x72F33CA7, 4}};
constexpr PropDef kFxDragTimeVectorNode[] = {{0xD2B8C5F0, 2}, {0x0DE810D9, 1}, {0xC0883E50, 2}};
constexpr PropDef kFxDynamicLuminanceMaterialNode[] = {
    {0x7636E1F6, 1}, {0x217CE1B0, 2}, {0x362A038B, 2}, {0x972FD939, 2}, {0x2BA9021F, 0}, {0xAAAD4DE6, 4}, {0xF8CE4CAA, 2}, {0xFC08FF2F, 2},
    {0xE2937ED7, 2}, {0xAB96FA51, 2}, {0xB65BBD54, 0}, {0x48428ECB, 1}, {0x0E3C1540, 2}, {0xBB53E22D, 2}, {0x344C4F9B, 1}, {0xD7FBBE28, 1},
    {0x99C0EF09, 0}, {0xBAF73B2F, 0}, {0x72F33CA7, 4}
};
constexpr PropDef kFxEffectTimeScaleVectorNode[] = {{0x06C4552D, 2}, {0xC7ABD8B2, 1}, {0xE01E4183, 2}};
constexpr PropDef kFxGameTimeInterpolateVectorNode[] = {{0x759B9632, 2}, {0xCA216724, 2}, {0x4C4B29BF, 0}};
constexpr PropDef kFxGraphEmitNode[] = {
    {0xA4842DF8, 2}, {0x69639E1F, 2}, {0x9C1E8FBF, 2}, {0x20ABC542, 2}, {0x2DA33043, 1}, {0x2A0357DD, 0}, {0x3DBF79B0, 2}, {0xF6B4B0EF, 2}
};
constexpr PropDef kFxInputFilterVectorNode[] = {{0xE3B399C2, 1}, {0xF7CBCFD2, 0}};
constexpr PropDef kFxInterpolateLineVectorNode[] = {{0x5F70F6A6, 3}, {0x9F381F3B, 3}, {0xE9DAB877, 4}};
constexpr PropDef kFxIntervalProbabilityEmitNode[] = {
    {0x48503BF9, 1}, {0x4CD9C31F, 1}, {0x3C8D5204, 2}, {0xD620F511, 0}, {0x9761B6D5, 1}, {0x33FE189E, 1}, {0x309EF6C9, 1}, {0x6AC9578A, 1},
    {0x95FB860B, 1}, {0x6DDAE7F8, 2}, {0xEC22E2C7, 1}, {0x05FFD4EB, 1}, {0xE9DAB877, 4}
};
constexpr PropDef kFxKeyframeVectorNode[] = {
    {0x261502A4, 1}, {0x4DBFDB86, 1}, {0x9B835C1F, 1}, {0x4078CFD1, 1}, {0x96237796, 1}, {0x93E612A1, 1}, {0xC90B7E24, 1}, {0xFB83BB36, 2},
    {0xC0508FF5, 2}, {0xF5E50B70, 2}, {0xD2D302AC, 2}, {0xEB6517B9, 2}, {0x90F78320, 2}, {0xE7F1341A, 2}, {0x355BBCF5, 2}
};
constexpr PropDef kFxLightInfluenceMaterialNode[] = {
    {0xADCCD482, 2}, {0x217CE1B0, 2}, {0x362A038B, 2}, {0x972FD939, 2}, {0x41488ACD, 2}, {0xB65BBD54, 0}, {0x9A230069, 2}, {0xAD94236D, 0},
    {0x48428ECB, 1}, {0x50E78B53, 0}, {0x0E3C1540, 2}, {0x40BE742E, 0}, {0xBB53E22D, 2}, {0x344C4F9B, 1}, {0xD7FBBE28, 1}, {0x99C0EF09, 0},
    {0xBAF73B2F, 0}, {0x72F33CA7, 4}
};
constexpr PropDef kFxLineShapeNode[] = {
    {0x9E69F4A9, 3}, {0xA850262D, 1}, {0x1CEC027A, 0}, {0x43B4F24E, 0}, {0xF825ECD8, 0}, {0x8CC43F28, 3}, {0x3A424C45, 3}, {0x9EAFFD2A, 1},
    {0xE434246B, 1}, {0xA90978FB, 2}, {0x1AFFA980, 2}
};
constexpr PropDef kFxLocusBladeShapeNode[] = {
    {0x9E69F4A9, 3}, {0xA850262D, 1}, {0x78F4ACF2, 3}, {0x180E7B16, 2}, {0x1CEC027A, 0}, {0xF825ECD8, 0}, {0x8CC43F28, 3}, {0x3A424C45, 3},
    {0x8A0C7B2B, 4}, {0x9EAFFD2A, 1}, {0x4BAF9414, 2}, {0x0B7CC884, 2}
};
constexpr PropDef kFxLodVectorNode[] = {{0xDE057317, 2}, {0x29584B20, 2}, {0x907C9DEC, 2}, {0x106ECD47, 2}, {0x010EA99A, 2}};
constexpr PropDef kFxModelPrimitiveShapeNode[] = {
    {0x9E69F4A9, 3}, {0xA850262D, 1}, {0x0F2216D0, 0}, {0x1CEC027A, 0}, {0xC80F79DB, 0}, {0x7241DEDA, 0}, {0x4A226D21, 0}, {0xF825ECD8, 0},
    {0x8CC43F28, 3}, {0x3A424C45, 3}, {0x5C1B5DF3, 6}, {0x9EAFFD2A, 1}, {0x0E49C84B, 0}, {0xA90978FB, 2}
};
constexpr PropDef kFxModelShapeNode[] = {
    {0x9E69F4A9, 3}, {0xA850262D, 1}, {0x1CEC027A, 0}, {0xF825ECD8, 0}, {0x9C167551, 2}, {0x39ECAC08, 2}, {0x8CC43F28, 3}, {0x3A424C45, 3},
    {0x5C1B5DF3, 6}, {0x9EAFFD2A, 1}
};
constexpr PropDef kFxModuleGraph[] = {
    {0xCDC62989, 1}, {0xD865337D, 3}, {0x4BA61406, 3}, {0xA850262D, 1}, {0xDFC9ED5B, 0}, {0x9BA4FB3B, 5}, {0x42FAFE2B, 1}, {0x8DB85414, 1},
    {0x4114D9A8, 1}, {0x380ADBEB, 1}, {0x49780D5C, 1}
};
constexpr PropDef kFxMultipleVectorNode[] = {{0xC44EE238, 3}, {0x1F695AD3, 0}, {0xE9DAB877, 4}, {0x2F55184A, 1}};
constexpr PropDef kFxMultiplyVectorNode[] = {{0x6FE5383F, 2}, {0x6D3F4A3F, 0}, {0xF3E0E378, 0}, {0x970C0E28, 0}, {0x5D6AF65B, 0}};
constexpr PropDef kFxNumLodEmitNode[] = {
    {0xBE7AE6F0, 0}, {0xCAB8CE1D, 2}, {0xBC2A4902, 2}, {0xDE057317, 2}, {0xFB5568AF, 2}, {0x17332672, 2}, {0x1F27A68D, 1}
};
constexpr PropDef kFxOldVersionMaterialNode[] = {
    {0xDE02160D, 1}, {0x82839428, 3}, {0x1F14DF64, 2}, {0xF88C633C, 2}, {0x2642E962, 2}, {0xB13FD162, 3}, {0x48428ECB, 1}, {0x50E78B53, 0},
    {0x0E3C1540, 2}, {0x72F33CA7, 4}
};
constexpr PropDef kFxOrientationAnimeVectorNode[] = {
    {0x6662D7FA, 1}, {0x8FD2A63F, 1}, {0xF0351193, 3}, {0xAA60D112, 3}, {0xCF8619C3, 0}, {0x3F606248, 1}, {0xF88410C1, 3}, {0xA4607F13, 3}
};
constexpr PropDef kFxOrientationVectorNode[] = {{0xD7EEB2EE, 2}, {0xEA72A4BE, 2}, {0xFF8073A8, 2}, {0xD9563544, 1}};
constexpr PropDef kFxOscillateVector2Node[] = {{0x35971A79, 0}};
constexpr PropDef kFxOscillateVectorNode[] = {{0x3096A361, 3}, {0xDB2E8027, 2}, {0xEC22E2C7, 1}, {0x05FFD4EB, 1}, {0xA2EAD6CA, 0}};
constexpr PropDef kFxPlaneRotShapeNode[] = {
    {0x9E69F4A9, 3}, {0x78F17705, 1}, {0xAD7CD8F6, 4}, {0x51D15822, 3}, {0x36998A19, 2}, {0xA850262D, 1}, {0x81E5CB5E, 2}, {0x0BB3ACCC, 2},
    {0x0F2216D0, 0}, {0x1CEC027A, 0}, {0xF825ECD8, 0}, {0x8CC43F28, 3}, {0x3A424C45, 3}, {0x9EAFFD2A, 1}, {0x3C0849CE, 0}, {0xC633B47F, 1},
    {0xE434246B, 1}, {0xA90978FB, 2}
};
constexpr PropDef kFxPlaneShapeNode[] = {
    {0x9E69F4A9, 3}, {0x78F17705, 1}, {0x36998A19, 2}, {0xA850262D, 1}, {0x81E5CB5E, 2}, {0x0BB3ACCC, 2}, {0x0F2216D0, 0}, {0x1CEC027A, 0},
    {0xF825ECD8, 0}, {0x8CC43F28, 3}, {0x3A424C45, 3}, {0x9EAFFD2A, 1}, {0x4E16168C, 3}, {0x3C0849CE, 0}, {0xE434246B, 1}, {0xA90978FB, 2}
};
constexpr PropDef kFxPointLightShapeNode[] = {
    {0x9E69F4A9, 3}, {0x39DFDD9D, 0}, {0xA850262D, 1}, {0xEEA4D248, 0}, {0x5FECC762, 2}, {0x1CEC027A, 0}, {0xFFE7D17F, 0}, {0x40ECFE36, 0},
    {0x733D3780, 3}, {0x46D0D39E, 3}, {0xC92B9E01, 3}, {0xF825ECD8, 0}, {0x649CC26A, 1}, {0x9C167551, 2}, {0x39ECAC08, 2}, {0x07DEC32F, 1},
    {0x6446D9D8, 2}, {0x8CC43F28, 3}, {0x3A424C45, 3}, {0x9EAFFD2A, 1}, {0xE9DAB877, 4}, {0xE142FD96, 2}
};
constexpr PropDef kFxPointLightShapeNodeOld[] = {
    {0x9E69F4A9, 3}, {0xA850262D, 1}, {0x1CEC027A, 0}, {0xF825ECD8, 0}, {0x8CC43F28, 3}, {0x3A424C45, 3}, {0x9EAFFD2A, 1}, {0x5CB268C2, 0}
};
constexpr PropDef kFxPoolVectorNode[] = {{0x14DDEF6E, 0}, {0x3FF4A421, 0}, {0x25D54FF0, 4}};
constexpr PropDef kFxRandomGenerateVectorNode[] = {
    {0x4F41F8F8, 2}, {0xEC22E2C7, 1}, {0x05FFD4EB, 1}, {0xB1AAE4DC, 3}, {0x7EE4CB40, 3}, {0x2F55184A, 1}, {0x2551466C, 0}
};
constexpr PropDef kFxRandomLifeNode[] = {{0x9B076750, 1}, {0x8F88FA93, 1}, {0xEC22E2C7, 1}, {0x05FFD4EB, 1}};
constexpr PropDef kFxRandomVectorNode[] = {
    {0x4F41F8F8, 2}, {0x1F695AD3, 0}, {0xF62B0158, 0}, {0xEC22E2C7, 1}, {0x05FFD4EB, 1}, {0xB1AAE4DC, 3}, {0x7EE4CB40, 3}, {0x2F55184A, 1},
    {0x2551466C, 0}
};
constexpr PropDef kFxReceiveLifeNode[] = {{0x9847E8C8, 1}, {0xC6B7DC60, 1}, {0xEC22E2C7, 1}, {0x05FFD4EB, 1}, {0xE9DAB877, 4}};
constexpr PropDef kFxReceiveNumEmitNode[] = {{0xED6F8A58, 1}, {0xE9DAB877, 4}};
constexpr PropDef kFxReceiveVectorNode[] = {
    {0x9485498D, 0}, {0x3231D709, 3}, {0x4F41F8F8, 2}, {0xFA4CEAA3, 0}, {0x026AC0BF, 0}, {0x9E0C7C62, 0}, {0xF393B0E0, 0}, {0xE0691301, 0},
    {0xE9DAB877, 4}
};
constexpr PropDef kFxRopeLineShapeNode[] = {
    {0x9E69F4A9, 3}, {0xA850262D, 1}, {0x81E5CB5E, 2}, {0x0BB3ACCC, 2}, {0x1CEC027A, 0}, {0xF825ECD8, 0}, {0x8CC43F28, 3}, {0x3A424C45, 3},
    {0x9EAFFD2A, 1}, {0x12FB992D, 1}, {0xA90978FB, 2}, {0x4AF14F8F, 0}
};
constexpr PropDef kFxScrollAnimationMaterialNode[] = {
    {0x7636E1F6, 1}, {0x972FD939, 2}, {0x1118DD61, 2}, {0x1364CE7E, 2}, {0xACB0CDBB, 2}, {0x6B880E78, 2}, {0xF1A308EB, 2}, {0x99EA2F62, 2},
    {0x1F7E7CF7, 2}, {0x27F25FE0, 2}, {0xF77D9DEA, 4}, {0xFBFFDA19, 2}, {0x48428ECB, 1}, {0x0E3C1540, 2}, {0x72F33CA7, 4}, {0x495EBA72, 2},
    {0xEDA9BBFB, 2}, {0x332DD0CB, 2}
};
constexpr PropDef kFxSoundCallProgramEffectNode[] = {
    {0x37CD447C, 0}, {0x2B0AD013, 0}, {0x86B5F009, 0}, {0x4EF4B1E6, 0}, {0x10CA0B28, 0}, {0x119432AD, 0}, {0x6F15D5A0, 0}, {0xE3A9CADA, 1},
    {0xD2ECAC68, 2}, {0x17332672, 2}, {0xC23372A6, 4}, {0xF10F71BA, 4}, {0x1EB03412, 4}
};
constexpr PropDef kFxSoundCallShapeNode[] = {
    {0x9E69F4A9, 3}, {0xA850262D, 1}, {0x1CEC027A, 0}, {0xF825ECD8, 0}, {0x8CC43F28, 3}, {0x3A424C45, 3}, {0x9EAFFD2A, 1}, {0xC23372A6, 4},
    {0x2C691B64, 4}
};
constexpr PropDef kFxSpotLightShapeNode[] = {
    {0xEC9C106B, 2}, {0x9E69F4A9, 3}, {0xA850262D, 1}, {0xEEA4D248, 0}, {0x5FECC762, 2}, {0x1CEC027A, 0}, {0xFFE7D17F, 0}, {0x40ECFE36, 0},
    {0x8068AAE2, 2}, {0x733D3780, 3}, {0x46D0D39E, 3}, {0xC92B9E01, 3}, {0xF825ECD8, 0}, {0x649CC26A, 1}, {0x9C167551, 2}, {0x39ECAC08, 2},
    {0x07DEC32F, 1}, {0x6446D9D8, 2}, {0x8CC43F28, 3}, {0x3A424C45, 3}, {0x9EAFFD2A, 1}, {0x6DAEF640, 2}, {0xA07B1E26, 4}, {0xE9DAB877, 4},
    {0xC2809567, 2}, {0xE142FD96, 2}, {0xBDB0BF99, 2}, {0xDFBA45D5, 2}, {0x26C9C23D, 2}
};
constexpr PropDef kFxSpreadVectorNode[] = {
    {0xD00A46C2, 2}, {0x4F41F8F8, 2}, {0xEF65426D, 2}, {0x1EB4131F, 1}, {0x1F695AD3, 0}, {0xD8F07EBD, 0}, {0xE6B68466, 0}, {0xEC22E2C7, 1},
    {0x05FFD4EB, 1}, {0xD4D0DAAA, 2}, {0x05B462F3, 3}
};
constexpr PropDef kFxSprite2DShapeNode[] = {
    {0x9E69F4A9, 3}, {0x91365199, 1}, {0x32FF2253, 1}, {0xDE02160D, 1}, {0xA850262D, 1}, {0x81E5CB5E, 2}, {0x0BB3ACCC, 2}, {0x1CEC027A, 0},
    {0x5A76071B, 0}, {0xF825ECD8, 0}, {0x8CC43F28, 3}, {0x3A424C45, 3}, {0x9EAFFD2A, 1}, {0x62E901A7, 1}, {0x55389195, 0}, {0x72F33CA7, 4}
};
constexpr PropDef kFxSpriteRotShapeNode[] = {
    {0x9E69F4A9, 3}, {0xA850262D, 1}, {0x81E5CB5E, 2}, {0x0BB3ACCC, 2}, {0x1CEC027A, 0}, {0x43B4F24E, 0}, {0xF825ECD8, 0}, {0x8CC43F28, 3},
    {0x3A424C45, 3}, {0x9EAFFD2A, 1}, {0x94390DB1, 0}, {0xE5014C1B, 0}, {0xE434246B, 1}, {0xA90978FB, 2}
};
constexpr PropDef kFxSpriteShapeNode[] = {
    {0x9E69F4A9, 3}, {0xA850262D, 1}, {0x81E5CB5E, 2}, {0x0BB3ACCC, 2}, {0x1CEC027A, 0}, {0x43B4F24E, 0}, {0xF825ECD8, 0}, {0x8CC43F28, 3},
    {0x3A424C45, 3}, {0x9EAFFD2A, 1}, {0xE434246B, 1}, {0xA90978FB, 2}
};
constexpr PropDef kFxSubtractionVectorNode[] = {{0x6FE5383F, 2}, {0x6D3F4A3F, 0}, {0xF3E0E378, 0}, {0x970C0E28, 0}, {0x5D6AF65B, 0}};
constexpr PropDef kFxTimeCircleVectorNode[] = {{0xB39DCF4C, 2}};
constexpr PropDef kFxTimeCurveVectorNode[] = {{0x7482C030, 1}};
constexpr PropDef kFxTimeScaleVectorNode[] = {{0x06C4552D, 2}, {0x47A7A4D1, 0}, {0xC4B7E974, 0}, {0xB79D2D2F, 0}, {0x78669CAC, 0}, {0xE01E4183, 2}};
constexpr PropDef kFxTrailShapeNode[] = {
    {0x6A15D311, 2}, {0x9A1C2468, 0}, {0x452BE79C, 2}, {0x008A47F0, 2}, {0xF9F26E77, 2}, {0xEB0DEA83, 1}, {0x9E69F4A9, 3}, {0x36998A19, 2},
    {0xA850262D, 1}, {0x1CEC027A, 0}, {0x715B1280, 1}, {0x612D357A, 1}, {0xF825ECD8, 0}, {0x8CC43F28, 3}, {0x3A424C45, 3}, {0xA82D6DEC, 1},
    {0x9EAFFD2A, 1}, {0xB022DFE2, 0}, {0xA90978FB, 2}, {0xC156B9A0, 0}, {0xD8A202ED, 1}, {0x9DEA0CDB, 1}
};
constexpr PropDef kFxUVAnimeIntervalVectorNode[] = {
    {0xEBAF6352, 2}, {0x0C089291, 0}, {0x9EC5A541, 1}, {0x1D121378, 1}, {0xAEADF7F1, 0}, {0x902AA0EB, 0}, {0x0199F7F9, 0}, {0xD3C64F5A, 0},
    {0xEC22E2C7, 1}, {0x05FFD4EB, 1}, {0x2EDD498B, 0}
};
constexpr PropDef kFxUVMapRandomVectorNode[] = {{0x0A3122DF, 1}, {0x4DF2F53A, 1}, {0x0199F7F9, 0}, {0xD3C64F5A, 0}, {0xEC22E2C7, 1}, {0x05FFD4EB, 1}};
constexpr PropDef kFxUVMapVectorNode[] = {{0x9EC5A541, 1}, {0x1D121378, 1}, {0xAEADF7F1, 0}, {0x902AA0EB, 0}, {0x4296121B, 3}};
constexpr PropDef kFxUpdateSleepControlNode[] = {{0xE6335039, 2}, {0x03043315, 2}, {0xDE057317, 2}, {0xDE2DB1F0, 1}, {0xF69AD7BB, 0}};
constexpr PropDef kFxWindEmitNode[] = {{0x6ECAA185, 2}, {0xADC44F51, 2}};
constexpr PropDef kFxWorldPosDeltaVectorNode[] = {{0xECBB13BB, 2}, {0x8007000E, 2}, {0xC0883E50, 2}, {0x51F86791, 0}, {0xBF8F71BC, 2}};
constexpr PropDef kShScalableScrollAnimationMaterialNode[] = {
    {0x7636E1F6, 1}, {0x217CE1B0, 2}, {0x362A038B, 2}, {0x972FD939, 2}, {0x1118DD61, 2}, {0x1364CE7E, 2}, {0xACB0CDBB, 2}, {0x6B880E78, 2},
    {0xF1A308EB, 2}, {0x99EA2F62, 2}, {0x1F7E7CF7, 2}, {0x27F25FE0, 2}, {0xF77D9DEA, 4}, {0xFBFFDA19, 2}, {0x48428ECB, 1}, {0x0E3C1540, 2},
    {0x72F33CA7, 4}, {0x495EBA72, 2}, {0xEDA9BBFB, 2}, {0x332DD0CB, 2}
};
constexpr PropDef kTppLensFlareProgramEffectNode[] = {
    {0x2118156A, 2}, {0x84F2038B, 1}, {0xA70631A0, 6}, {0x9B1C6BA4, 4}, {0x411248AE, 2}, {0x2642E962, 2}, {0x5392278B, 2}, {0x2BF87B1B, 1},
    {0xCC495791, 3}, {0xE9DAB877, 4}, {0x32CA14EC, 0}, {0x430D5D92, 2}, {0xF856246F, 2}
};
constexpr PropDef kTppLiquidMaterial2HNMNode[] = {
    {0xADCCD482, 2}, {0x217CE1B0, 2}, {0x362A038B, 2}, {0x972FD939, 2}, {0x23E90E5F, 4}, {0x41488ACD, 2}, {0xF6B16443, 2}, {0xB65BBD54, 0},
    {0x9A230069, 2}, {0xBD8530BE, 2}, {0x3B831F9F, 2}, {0x62133294, 2}, {0x4C08ECC7, 2}, {0x50E78B53, 0}, {0x0E3C1540, 2}, {0xBB53E22D, 2},
    {0x344C4F9B, 1}, {0xD7FBBE28, 1}, {0x99C0EF09, 0}, {0xBAF73B2F, 0}, {0x72F33CA7, 4}, {0x09DC8591, 2}
};
constexpr PropDef kTppLiquidMaterial2Node[] = {
    {0xADCCD482, 2}, {0x217CE1B0, 2}, {0x362A038B, 2}, {0x972FD939, 2}, {0x23E90E5F, 4}, {0x41488ACD, 2}, {0xF6B16443, 2}, {0xB65BBD54, 0},
    {0x9A230069, 2}, {0xBD8530BE, 2}, {0x3B831F9F, 2}, {0x62133294, 2}, {0x4C08ECC7, 2}, {0x50E78B53, 0}, {0x0E3C1540, 2}, {0xBB53E22D, 2},
    {0x344C4F9B, 1}, {0xD7FBBE28, 1}, {0x99C0EF09, 0}, {0xBAF73B2F, 0}, {0x72F33CA7, 4}, {0x09DC8591, 2}
};
constexpr PropDef kTppLiquidMaterialNode[] = {
    {0x94CCCDD8, 3}, {0xE9BEF4AD, 2}, {0x522F0D83, 2}, {0xDC56DB8A, 2}, {0x6EA4260E, 2}, {0xF46F5FFF, 2}, {0x7D9E955B, 2}, {0x2599BDE5, 2},
    {0x9AC5DD02, 2}, {0x067666C3, 2}, {0xB65BBD54, 0}, {0x50C04326, 0}, {0x50E78B53, 0}, {0x0E3C1540, 2}, {0xB92B0B96, 2}, {0x86E71BA4, 2},
    {0x344C4F9B, 1}, {0x8C9F7689, 2}, {0xD7FBBE28, 1}, {0xBAF73B2F, 0}, {0x72F33CA7, 4}
};
constexpr PropDef kWindFxVectorNode[] = {{0x3D1BF456, 2}, {0x4067A508, 0}, {0xD614669B, 1}};
constexpr PropDef kWindFxWindFlowVectorNode[] = {{0x3D1BF456, 2}, {0xB5734710, 2}, {0x13226820, 0}};

constexpr ClassDef kClasses[] = {
    {0x8B05D065, "FxAddPoolVectorNode", kFxAddPoolVectorNode},
    {0xF51AC0B4, "FxApplyRandomVectorNode", kFxApplyRandomVectorNode},
    {0xFBDA7364, "FxBulletLineProgramEffectNode", kFxBulletLineProgramEffectNode},
    {0x2BB07018, "FxCameraAngleVectorNode", kFxCameraAngleVectorNode},
    {0x5443A663, "FxCameraCorrectionMaterialNode", kFxCameraCorrectionMaterialNode},
    {0xE32798AB, "FxCameraCorrectionVectorNode", kFxCameraCorrectionVectorNode},
    {0x70795051, "FxCameraFollowVectorNode", kFxCameraFollowVectorNode},
    {0x48590023, "FxCenterDistRateVectorNode", kFxCenterDistRateVectorNode},
    {0x291D1C21, "FxCenterScrollVectorNode", kFxCenterScrollVectorNode},
    {0x8146393E, "FxCheckLightInterceptionVectorNode", kFxCheckLightInterceptionVectorNode},
    {0x542562FD, "FxChildEmitVectorNode", kFxChildEmitVectorNode},
    {0xDBDA6620, "FxCloneShapeNode", kFxCloneShapeNode},
    {0xD9775FC7, "FxCollisionCheckVectorNode", kFxCollisionCheckVectorNode},
    {0x0F2A4E3C, "FxColorVectorNode", kFxColorVectorNode},
    {0xF99DF5F2, "FxCompositionVectorNode", kFxCompositionVectorNode},
    {0x8FADD256, "FxConstLifeNode", kFxConstLifeNode},
    {0x324EB321, "FxConstNumEmitNode", kFxConstNumEmitNode},
    {0x1E050127, "FxConstScaleVectorNode", kFxConstScaleVectorNode},
    {0x2D07F4DA, "FxConstVectorNode", kFxConstVectorNode},
    {0x8B6ACF34, "FxConstantMaterialNode", kFxConstantMaterialNode},
    {0xBDF6F89B, "FxDecalShapeNode", kFxDecalShapeNode},
    {0x15E2D2C8, "FxDelayNumEmitNode", kFxDelayNumEmitNode},
    {0xB4118A12, "FxDistanceEmitVectorNode", kFxDistanceEmitVectorNode},
    {0xD433376E, "FxDistanceNumEmitNode", kFxDistanceNumEmitNode},
    {0xC7D3FA9A, "FxDistortionMaterialNode", kFxDistortionMaterialNode},
    {0xE060B8C7, "FxDragTimeVectorNode", kFxDragTimeVectorNode},
    {0x774BE7BD, "FxDynamicLuminanceMaterialNode", kFxDynamicLuminanceMaterialNode},
    {0x83DC871B, "FxEffectTimeScaleVectorNode", kFxEffectTimeScaleVectorNode},
    {0xF0C46BDE, "FxFirstLoopOnlyEmitNode", {}},
    {0x8B959BCA, "FxGameTimeInterpolateVectorNode", kFxGameTimeInterpolateVectorNode},
    {0x1A770F62, "FxGraphEmitNode", kFxGraphEmitNode},
    {0x83800B1F, "FxInfinityLifeNode", {}},
    {0xCEFD036F, "FxInputFilterVectorNode", kFxInputFilterVectorNode},
    {0xE506E1D7, "FxInterpolateLineVectorNode", kFxInterpolateLineVectorNode},
    {0xC3ED2F6E, "FxIntervalProbabilityEmitNode", kFxIntervalProbabilityEmitNode},
    {0xD0FDDFE8, "FxKeyframeVectorNode", kFxKeyframeVectorNode},
    {0xCB565C60, "FxLightInfluenceMaterialNode", kFxLightInfluenceMaterialNode},
    {0xECA9424B, "FxLineShapeNode", kFxLineShapeNode},
    {0x2EBB4A00, "FxLocusBladeShapeNode", kFxLocusBladeShapeNode},
    {0x626249DE, "FxLodVectorNode", kFxLodVectorNode},
    {0xAC9A939B, "FxModelPrimitiveShapeNode", kFxModelPrimitiveShapeNode},
    {0xC2711730, "FxModelShapeNode", kFxModelShapeNode},
    {0x5B06CA22, "FxModuleGraph", kFxModuleGraph},
    {0x8B82F4F5, "FxMultipleVectorNode", kFxMultipleVectorNode},
    {0x3D852E1C, "FxMultiplyVectorNode", kFxMultiplyVectorNode},
    {0xE17CDA94, "FxNumLodEmitNode", kFxNumLodEmitNode},
    {0x880501EC, "FxOldVersionMaterialNode", kFxOldVersionMaterialNode},
    {0xD5926E22, "FxOrientationAnimeVectorNode", kFxOrientationAnimeVectorNode},
    {0xDE3A80A2, "FxOrientationVectorNode", kFxOrientationVectorNode},
    {0xFCBD414E, "FxOscillateVector2Node", kFxOscillateVector2Node},
    {0xE60DFC67, "FxOscillateVectorNode", kFxOscillateVectorNode},
    {0x431C53EA, "FxPlaneRotShapeNode", kFxPlaneRotShapeNode},
    {0x45C1F30A, "FxPlaneShapeNode", kFxPlaneShapeNode},
    {0x63830DF5, "FxPointLightShapeNode", kFxPointLightShapeNode},
    {0x027790B3, "FxPointLightShapeNodeOld", kFxPointLightShapeNodeOld},
    {0xD3F362C5, "FxPoolVectorNode", kFxPoolVectorNode},
    {0x5AB35D73, "FxRandomGenerateVectorNode", kFxRandomGenerateVectorNode},
    {0xBC5662E9, "FxRandomLifeNode", kFxRandomLifeNode},
    {0x6AB27693, "FxRandomVectorNode", kFxRandomVectorNode},
    {0xBB31D77F, "FxReceiveLifeNode", kFxReceiveLifeNode},
    {0x31A54DEF, "FxReceiveNumEmitNode", kFxReceiveNumEmitNode},
    {0x7D3E83F4, "FxReceiveVectorNode", kFxReceiveVectorNode},
    {0xC547836A, "FxRopeLineShapeNode", kFxRopeLineShapeNode},
    {0xEB8ABEE4, "FxScrollAnimationMaterialNode", kFxScrollAnimationMaterialNode},
    {0x8E522023, "FxSoundCallProgramEffectNode", kFxSoundCallProgramEffectNode},
    {0x122E3868, "FxSoundCallShapeNode", kFxSoundCallShapeNode},
    {0x955F7246, "FxSpotLightShapeNode", kFxSpotLightShapeNode},
    {0x94779FF9, "FxSpreadVectorNode", kFxSpreadVectorNode},
    {0xF845A712, "FxSprite2DShapeNode", kFxSprite2DShapeNode},
    {0xA51C93DC, "FxSpriteRotShapeNode", kFxSpriteRotShapeNode},
    {0xC31E5C16, "FxSpriteShapeNode", kFxSpriteShapeNode},
    {0x46D32803, "FxSubtractionVectorNode", kFxSubtractionVectorNode},
    {0x0B18D6A3, "FxTimeCircleVectorNode", kFxTimeCircleVectorNode},
    {0x64005093, "FxTimeCurveVectorNode", kFxTimeCurveVectorNode},
    {0xE08647E7, "FxTimeScaleVectorNode", kFxTimeScaleVectorNode},
    {0xBC411D37, "FxTrailShapeNode", kFxTrailShapeNode},
    {0xB2105308, "FxUVAnimeIntervalVectorNode", kFxUVAnimeIntervalVectorNode},
    {0x4AE19F65, "FxUVMapRandomVectorNode", kFxUVMapRandomVectorNode},
    {0x0680F7C8, "FxUVMapVectorNode", kFxUVMapVectorNode},
    {0x6A621D6B, "FxUniformAccelTimeVectorNode", {}},
    {0xF7CB5A60, "FxUniformAccelVectorNode", {}},
    {0x29C60EBC, "FxUniformVelocityTimeVectorNode", {}},
    {0x09FD2AE8, "FxUniformVelocityVectorNode", {}},
    {0x76D36F96, "FxUpdateSleepControlNode", kFxUpdateSleepControlNode},
    {0x882CF671, "FxWindEmitNode", kFxWindEmitNode},
    {0x26917BE9, "FxWorldPosDeltaVectorNode", kFxWorldPosDeltaVectorNode},
    {0x76F8F884, "ShScalableScrollAnimationMaterialNode", kShScalableScrollAnimationMaterialNode},
    {0x5C205536, "TppLensFlareProgramEffectNode", kTppLensFlareProgramEffectNode},
    {0x598FC1E5, "TppLiquidMaterial2HNMNode", kTppLiquidMaterial2HNMNode},
    {0x2E60BF85, "TppLiquidMaterial2Node", kTppLiquidMaterial2Node},
    {0x0F8A4AA5, "TppLiquidMaterialNode", kTppLiquidMaterialNode},
    {0x23C4BE1D, "WindFxVectorNode", kWindFxVectorNode},
    {0x5567C837, "WindFxWindFlowVectorNode", kWindFxWindFlowVectorNode},
};

}

const ClassDef* FindClass(uint32_t hash) {
    for (const ClassDef& c : kClasses) {
        if (c.hash == hash) {
            return &c;
        }
    }
    return nullptr;
}

std::span<const ClassDef> Classes() {
    return kClasses;
}

}
