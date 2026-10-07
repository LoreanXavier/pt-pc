import argparse
import json
import struct
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pathcode import read_pathid_list

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_FPK_ROOT = REPO_ROOT / "dump" / "fpk"
DEFAULT_PATHIDS = REPO_ROOT / "game" / "CUSA01127" / "pathid_list_ps4.bin"

HEADER_SIZE = 15
TYPE_BOOL, TYPE_UINT32, TYPE_FLOAT, TYPE_VECTOR4, TYPE_STRING, TYPE_STRCODE, TYPE_PATHCODE = range(7)
TYPE_NAMES = ["bool", "uint32", "float", "Vector4", "String", "StrCode64", "PathCode64"]
FIXED = {TYPE_BOOL: ("<?", 1), TYPE_UINT32: ("<I", 4), TYPE_FLOAT: ("<f", 4), TYPE_VECTOR4: ("<4f", 16), TYPE_STRCODE: ("<Q", 8),
         TYPE_PATHCODE: ("<Q", 8)}

CLASSES = {
    0x8B05D065: ("FxAddPoolVectorNode", [(0xDD140AE7, 2), (0xD791673F, 2), (0x53C923A7, 2), (0x430E9E8E, 1), (0x25D54FF0, 4)]),
    0xF51AC0B4: ("FxApplyRandomVectorNode", [(0xEC22E2C7, 1), (0x05FFD4EB, 1)]),
    0xFBDA7364: ("FxBulletLineProgramEffectNode", [(0x3D1BF456, 2), (0x9F412E57, 2), (0x6A5FA950, 2), (0xD310A646, 2), (0xB4F75F03, 2),
        (0xAB20DE36, 2), (0x31985701, 2), (0x094FB2B9, 2), (0x0EB57E93, 2), (0x2C62EB6A, 2), (0x61A6E2F0, 2), (0xD43DE798, 3), (0x7636E1F6, 1),
        (0x217CE1B0, 2), (0x362A038B, 2), (0xE6832126, 2), (0x4111B159, 2), (0x28AC37D4, 2), (0xFA572C45, 2), (0xEEE69CCA, 0), (0xBE3BD61E, 0),
        (0x7CA4AE7B, 0), (0x39441A80, 0), (0x612D357A, 1), (0x33FE189E, 1), (0x8941E3B3, 1), (0x5FEFA472, 1), (0x3A9AC68E, 2), (0x302CE9CF, 0),
        (0xECCE68E7, 1), (0x1B98FED9, 1), (0xC8BC49E6, 3), (0x8AB1CA2B, 2), (0x38A73E2B, 2), (0x1118DD61, 2), (0x21738C2A, 4), (0xE5ACF62A, 2),
        (0xF1DDC1EE, 2), (0x9BF77157, 2), (0xF8CE4CAA, 2), (0xFC08FF2F, 2), (0xE2937ED7, 2), (0xAB96FA51, 2), (0x4067A508, 0), (0xB65BBD54, 0),
        (0xEC22E2C7, 1), (0x05FFD4EB, 1), (0xE9DAB877, 4), (0x48428ECB, 1), (0xCF9D35A1, 1), (0x6739DFC3, 2), (0x283A241E, 2), (0xAD7105FB, 2),
        (0x328263C6, 0), (0xC5BFC21D, 2), (0xD704532B, 2), (0x77DEDF8D, 1), (0x781AAC2C, 1), (0xAE8F8BA3, 2), (0x7127EB8E, 2), (0xC00B4A79, 2),
        (0xA90978FB, 2), (0x5E28DEE9, 0), (0x72F33CA7, 4), (0xCFBDAC7B, 1), (0xA74BE2E8, 2), (0x1B7F9286, 2), (0xF4DA0FEC, 2)]),
    0x2BB07018: ("FxCameraAngleVectorNode", [(0x506F061C, 1), (0xEECAF4DF, 0)]),
    0x5443A663: ("FxCameraCorrectionMaterialNode", [(0x7636E1F6, 1), (0x2435A351, 2), (0xBDEBC3F4, 2), (0x3C826A5C, 2), (0x25EF42E3, 2),
        (0x03ADA714, 2), (0x779A8842, 2), (0x75546309, 2), (0x97E06A7B, 2), (0xFB8F3EFF, 2), (0x2296332E, 2), (0xDE6A379F, 2), (0x25C95427, 2),
        (0x68E43C74, 2), (0x6AA1ACFC, 2), (0xFEB64AD5, 2), (0x02ED8BF5, 2), (0x1C9DB76F, 2), (0x8FAFF419, 2), (0x9DE24629, 2), (0x164A2569, 2),
        (0x972FD939, 2), (0x1118DD61, 2), (0x7BF7756D, 2), (0x48428ECB, 1), (0x0E3C1540, 2), (0x72F33CA7, 4)]),
    0xE32798AB: ("FxCameraCorrectionVectorNode", [(0x3BF5868A, 2), (0xE210591C, 1), (0xBA7C713E, 2), (0x22D34390, 0), (0xB4A484CB, 2),
        (0x56C2B781, 2), (0x8C3C9820, 2), (0x78176DC8, 2), (0x3E251573, 2)]),
    0x70795051: ("FxCameraFollowVectorNode", [(0x73D22AD2, 3), (0x1ACEBD89, 0), (0x849D3E0C, 0)]),
    0x48590023: ("FxCenterDistRateVectorNode", [(0xC73B3191, 0), (0xABD319EB, 0), (0xE4D092DD, 0), (0x2F51BCE2, 0), (0x6FC9EBDD, 3),
        (0x7E85E8D8, 3), (0x4819755F, 3), (0x3CEF4C99, 3), (0x38B1A286, 4)]),
    0x291D1C21: ("FxCenterScrollVectorNode", [(0xAA90B6B2, 0), (0x40EB719E, 4), (0x91E731AD, 3)]),
    0x8146393E: ("FxCheckLightInterceptionVectorNode", [(0x4C8B8306, 0), (0x25A4D811, 1), (0x527D0675, 2), (0x96CE333D, 0), (0x654A17E9, 1)]),
    0x542562FD: ("FxChildEmitVectorNode", [(0xE4B7906D, 4), (0xF68E107E, 2), (0x4DC95B4E, 4), (0x6F37DB13, 4)]),
    0xDBDA6620: ("FxCloneShapeNode", [(0x9E69F4A9, 3), (0xA850262D, 1), (0x1CEC027A, 0), (0xB0A863CB, 0), (0xF825ECD8, 0), (0x8CC43F28, 3),
        (0x3A424C45, 3), (0x5C1B5DF3, 6), (0x9EAFFD2A, 1)]),
    0xD9775FC7: ("FxCollisionCheckVectorNode", [(0x9BFBFF91, 4), (0xBEDD7B1A, 1), (0x4DC95B4E, 4), (0xB45EFCBE, 2), (0x6F37DB13, 4)]),
    0x0F2A4E3C: ("FxColorVectorNode", [(0x78F4ACF2, 3)]),
    0xF99DF5F2: ("FxCompositionVectorNode", [(0x6FE5383F, 2), (0x6D3F4A3F, 0), (0xF3E0E378, 0), (0x970C0E28, 0), (0x5D6AF65B, 0)]),
    0x8FADD256: ("FxConstLifeNode", [(0x33FE189E, 1)]),
    0x324EB321: ("FxConstNumEmitNode", [(0xB07EC7A5, 1)]),
    0x1E050127: ("FxConstScaleVectorNode", [(0x47A7A4D1, 0), (0xC4B7E974, 0), (0xB79D2D2F, 0), (0x78669CAC, 0), (0xC0883E50, 2)]),
    0x2D07F4DA: ("FxConstVectorNode", [(0x4F41F8F8, 2), (0x1F695AD3, 0), (0x48ADB96A, 3), (0x2F55184A, 1)]),
    0x8B6ACF34: ("FxConstantMaterialNode", [(0x7636E1F6, 1), (0x217CE1B0, 2), (0x362A038B, 2), (0x1C9DB76F, 2), (0x8FAFF419, 2), (0x9DE24629, 2),
        (0x164A2569, 2), (0x972FD939, 2), (0x1118DD61, 2), (0xB65BBD54, 0), (0x1364CE7E, 2), (0xACB0CDBB, 2), (0x6B880E78, 2), (0xF1A308EB, 2),
        (0x99EA2F62, 2), (0x1F7E7CF7, 2), (0x27F25FE0, 2), (0xF77D9DEA, 4), (0xFBFFDA19, 2), (0x48428ECB, 1), (0x0E3C1540, 2), (0xBB53E22D, 2),
        (0x344C4F9B, 1), (0xD7FBBE28, 1), (0x99C0EF09, 0), (0xBAF73B2F, 0), (0x72F33CA7, 4)]),
    0xBDF6F89B: ("FxDecalShapeNode", [(0x9E69F4A9, 3), (0x36998A19, 2), (0xA850262D, 1), (0x800A313F, 0), (0x5AFF77F2, 0), (0x4C8B8306, 0),
        (0x1CEC027A, 0), (0x999DC0F3, 4), (0xF825ECD8, 0), (0x8CC43F28, 3), (0x3A424C45, 3), (0x888E606F, 1), (0x5C1B5DF3, 6), (0xD0343D4B, 2),
        (0x2D058C6C, 4), (0x9EAFFD2A, 1), (0x88F0F0A9, 1), (0xA35A373D, 2), (0x44178224, 2), (0xC31A37A7, 0), (0xA28A6C30, 3), (0x48428ECB, 1),
        (0xA90978FB, 2), (0xC6CC8CEF, 4), (0x72F33CA7, 4), (0x7F916B75, 0)]),
    0x15E2D2C8: ("FxDelayNumEmitNode", [(0x48503BF9, 1), (0x4CD9C31F, 1), (0x33FE189E, 1), (0x0A539A6A, 1), (0xEC22E2C7, 1), (0x05FFD4EB, 1)]),
    0xB4118A12: ("FxDistanceEmitVectorNode", [(0x9AA63A20, 2)]),
    0xD433376E: ("FxDistanceNumEmitNode", [(0x9AA63A20, 2), (0x996A1E16, 1)]),
    0xC7D3FA9A: ("FxDistortionMaterialNode", [(0x28AC37D4, 2), (0xFA572C45, 2), (0x50E78B53, 0), (0x0E3C1540, 2), (0x72F33CA7, 4)]),
    0xE060B8C7: ("FxDragTimeVectorNode", [(0xD2B8C5F0, 2), (0x0DE810D9, 1), (0xC0883E50, 2)]),
    0x774BE7BD: ("FxDynamicLuminanceMaterialNode", [(0x7636E1F6, 1), (0x217CE1B0, 2), (0x362A038B, 2), (0x972FD939, 2), (0x2BA9021F, 0),
        (0xAAAD4DE6, 4), (0xF8CE4CAA, 2), (0xFC08FF2F, 2), (0xE2937ED7, 2), (0xAB96FA51, 2), (0xB65BBD54, 0), (0x48428ECB, 1), (0x0E3C1540, 2),
        (0xBB53E22D, 2), (0x344C4F9B, 1), (0xD7FBBE28, 1), (0x99C0EF09, 0), (0xBAF73B2F, 0), (0x72F33CA7, 4)]),
    0x83DC871B: ("FxEffectTimeScaleVectorNode", [(0x06C4552D, 2), (0xC7ABD8B2, 1), (0xE01E4183, 2)]),
    0xF0C46BDE: ("FxFirstLoopOnlyEmitNode", []),
    0x8B959BCA: ("FxGameTimeInterpolateVectorNode", [(0x759B9632, 2), (0xCA216724, 2), (0x4C4B29BF, 0)]),
    0x1A770F62: ("FxGraphEmitNode", [(0xA4842DF8, 2), (0x69639E1F, 2), (0x9C1E8FBF, 2), (0x20ABC542, 2), (0x2DA33043, 1), (0x2A0357DD, 0),
        (0x3DBF79B0, 2), (0xF6B4B0EF, 2)]),
    0x83800B1F: ("FxInfinityLifeNode", []),
    0xCEFD036F: ("FxInputFilterVectorNode", [(0xE3B399C2, 1), (0xF7CBCFD2, 0)]),
    0xE506E1D7: ("FxInterpolateLineVectorNode", [(0x5F70F6A6, 3), (0x9F381F3B, 3), (0xE9DAB877, 4)]),
    0xC3ED2F6E: ("FxIntervalProbabilityEmitNode", [(0x48503BF9, 1), (0x4CD9C31F, 1), (0x3C8D5204, 2), (0xD620F511, 0), (0x9761B6D5, 1),
        (0x33FE189E, 1), (0x309EF6C9, 1), (0x6AC9578A, 1), (0x95FB860B, 1), (0x6DDAE7F8, 2), (0xEC22E2C7, 1), (0x05FFD4EB, 1), (0xE9DAB877, 4)]),
    0xD0FDDFE8: ("FxKeyframeVectorNode", [(0x261502A4, 1), (0x4DBFDB86, 1), (0x9B835C1F, 1), (0x4078CFD1, 1), (0x96237796, 1), (0x93E612A1, 1),
        (0xC90B7E24, 1), (0xFB83BB36, 2), (0xC0508FF5, 2), (0xF5E50B70, 2), (0xD2D302AC, 2), (0xEB6517B9, 2), (0x90F78320, 2), (0xE7F1341A, 2),
        (0x355BBCF5, 2)]),
    0xCB565C60: ("FxLightInfluenceMaterialNode", [(0xADCCD482, 2), (0x217CE1B0, 2), (0x362A038B, 2), (0x972FD939, 2), (0x41488ACD, 2),
        (0xB65BBD54, 0), (0x9A230069, 2), (0xAD94236D, 0), (0x48428ECB, 1), (0x50E78B53, 0), (0x0E3C1540, 2), (0x40BE742E, 0), (0xBB53E22D, 2),
        (0x344C4F9B, 1), (0xD7FBBE28, 1), (0x99C0EF09, 0), (0xBAF73B2F, 0), (0x72F33CA7, 4)]),
    0xECA9424B: ("FxLineShapeNode", [(0x9E69F4A9, 3), (0xA850262D, 1), (0x1CEC027A, 0), (0x43B4F24E, 0), (0xF825ECD8, 0), (0x8CC43F28, 3),
        (0x3A424C45, 3), (0x9EAFFD2A, 1), (0xE434246B, 1), (0xA90978FB, 2), (0x1AFFA980, 2)]),
    0x2EBB4A00: ("FxLocusBladeShapeNode", [(0x9E69F4A9, 3), (0xA850262D, 1), (0x78F4ACF2, 3), (0x180E7B16, 2), (0x1CEC027A, 0), (0xF825ECD8, 0),
        (0x8CC43F28, 3), (0x3A424C45, 3), (0x8A0C7B2B, 4), (0x9EAFFD2A, 1), (0x4BAF9414, 2), (0x0B7CC884, 2)]),
    0x626249DE: ("FxLodVectorNode", [(0xDE057317, 2), (0x29584B20, 2), (0x907C9DEC, 2), (0x106ECD47, 2), (0x010EA99A, 2)]),
    0xAC9A939B: ("FxModelPrimitiveShapeNode", [(0x9E69F4A9, 3), (0xA850262D, 1), (0x0F2216D0, 0), (0x1CEC027A, 0), (0xC80F79DB, 0),
        (0x7241DEDA, 0), (0x4A226D21, 0), (0xF825ECD8, 0), (0x8CC43F28, 3), (0x3A424C45, 3), (0x5C1B5DF3, 6), (0x9EAFFD2A, 1), (0x0E49C84B, 0),
        (0xA90978FB, 2)]),
    0xC2711730: ("FxModelShapeNode", [(0x9E69F4A9, 3), (0xA850262D, 1), (0x1CEC027A, 0), (0xF825ECD8, 0), (0x9C167551, 2), (0x39ECAC08, 2),
        (0x8CC43F28, 3), (0x3A424C45, 3), (0x5C1B5DF3, 6), (0x9EAFFD2A, 1)]),
    0x5B06CA22: ("FxModuleGraph", [(0xCDC62989, 1), (0xD865337D, 3), (0x4BA61406, 3), (0xA850262D, 1), (0xDFC9ED5B, 0), (0x9BA4FB3B, 5),
        (0x42FAFE2B, 1), (0x8DB85414, 1), (0x4114D9A8, 1), (0x380ADBEB, 1), (0x49780D5C, 1)]),
    0x8B82F4F5: ("FxMultipleVectorNode", [(0xC44EE238, 3), (0x1F695AD3, 0), (0xE9DAB877, 4), (0x2F55184A, 1)]),
    0x3D852E1C: ("FxMultiplyVectorNode", [(0x6FE5383F, 2), (0x6D3F4A3F, 0), (0xF3E0E378, 0), (0x970C0E28, 0), (0x5D6AF65B, 0)]),
    0xE17CDA94: ("FxNumLodEmitNode", [(0xBE7AE6F0, 0), (0xCAB8CE1D, 2), (0xBC2A4902, 2), (0xDE057317, 2), (0xFB5568AF, 2), (0x17332672, 2),
        (0x1F27A68D, 1)]),
    0x880501EC: ("FxOldVersionMaterialNode", [(0xDE02160D, 1), (0x82839428, 3), (0x1F14DF64, 2), (0xF88C633C, 2), (0x2642E962, 2),
        (0xB13FD162, 3), (0x48428ECB, 1), (0x50E78B53, 0), (0x0E3C1540, 2), (0x72F33CA7, 4)]),
    0xD5926E22: ("FxOrientationAnimeVectorNode", [(0x6662D7FA, 1), (0x8FD2A63F, 1), (0xF0351193, 3), (0xAA60D112, 3), (0xCF8619C3, 0),
        (0x3F606248, 1), (0xF88410C1, 3), (0xA4607F13, 3)]),
    0xDE3A80A2: ("FxOrientationVectorNode", [(0xD7EEB2EE, 2), (0xEA72A4BE, 2), (0xFF8073A8, 2), (0xD9563544, 1)]),
    0xFCBD414E: ("FxOscillateVector2Node", [(0x35971A79, 0)]),
    0xE60DFC67: ("FxOscillateVectorNode", [(0x3096A361, 3), (0xDB2E8027, 2), (0xEC22E2C7, 1), (0x05FFD4EB, 1), (0xA2EAD6CA, 0)]),
    0x431C53EA: ("FxPlaneRotShapeNode", [(0x9E69F4A9, 3), (0x78F17705, 1), (0xAD7CD8F6, 4), (0x51D15822, 3), (0x36998A19, 2), (0xA850262D, 1),
        (0x81E5CB5E, 2), (0x0BB3ACCC, 2), (0x0F2216D0, 0), (0x1CEC027A, 0), (0xF825ECD8, 0), (0x8CC43F28, 3), (0x3A424C45, 3), (0x9EAFFD2A, 1),
        (0x3C0849CE, 0), (0xC633B47F, 1), (0xE434246B, 1), (0xA90978FB, 2)]),
    0x45C1F30A: ("FxPlaneShapeNode", [(0x9E69F4A9, 3), (0x78F17705, 1), (0x36998A19, 2), (0xA850262D, 1), (0x81E5CB5E, 2), (0x0BB3ACCC, 2),
        (0x0F2216D0, 0), (0x1CEC027A, 0), (0xF825ECD8, 0), (0x8CC43F28, 3), (0x3A424C45, 3), (0x9EAFFD2A, 1), (0x4E16168C, 3), (0x3C0849CE, 0),
        (0xE434246B, 1), (0xA90978FB, 2)]),
    0x63830DF5: ("FxPointLightShapeNode", [(0x9E69F4A9, 3), (0x39DFDD9D, 0), (0xA850262D, 1), (0xEEA4D248, 0), (0x5FECC762, 2), (0x1CEC027A, 0),
        (0xFFE7D17F, 0), (0x40ECFE36, 0), (0x733D3780, 3), (0x46D0D39E, 3), (0xC92B9E01, 3), (0xF825ECD8, 0), (0x649CC26A, 1), (0x9C167551, 2),
        (0x39ECAC08, 2), (0x07DEC32F, 1), (0x6446D9D8, 2), (0x8CC43F28, 3), (0x3A424C45, 3), (0x9EAFFD2A, 1), (0xE9DAB877, 4), (0xE142FD96, 2)]),
    0x027790B3: ("FxPointLightShapeNodeOld", [(0x9E69F4A9, 3), (0xA850262D, 1), (0x1CEC027A, 0), (0xF825ECD8, 0), (0x8CC43F28, 3),
        (0x3A424C45, 3), (0x9EAFFD2A, 1), (0x5CB268C2, 0)]),
    0xD3F362C5: ("FxPoolVectorNode", [(0x14DDEF6E, 0), (0x3FF4A421, 0), (0x25D54FF0, 4)]),
    0x5AB35D73: ("FxRandomGenerateVectorNode", [(0x4F41F8F8, 2), (0xEC22E2C7, 1), (0x05FFD4EB, 1), (0xB1AAE4DC, 3), (0x7EE4CB40, 3),
        (0x2F55184A, 1), (0x2551466C, 0)]),
    0xBC5662E9: ("FxRandomLifeNode", [(0x9B076750, 1), (0x8F88FA93, 1), (0xEC22E2C7, 1), (0x05FFD4EB, 1)]),
    0x6AB27693: ("FxRandomVectorNode", [(0x4F41F8F8, 2), (0x1F695AD3, 0), (0xF62B0158, 0), (0xEC22E2C7, 1), (0x05FFD4EB, 1), (0xB1AAE4DC, 3),
        (0x7EE4CB40, 3), (0x2F55184A, 1), (0x2551466C, 0)]),
    0xBB31D77F: ("FxReceiveLifeNode", [(0x9847E8C8, 1), (0xC6B7DC60, 1), (0xEC22E2C7, 1), (0x05FFD4EB, 1), (0xE9DAB877, 4)]),
    0x31A54DEF: ("FxReceiveNumEmitNode", [(0xED6F8A58, 1), (0xE9DAB877, 4)]),
    0x7D3E83F4: ("FxReceiveVectorNode", [(0x9485498D, 0), (0x3231D709, 3), (0x4F41F8F8, 2), (0xFA4CEAA3, 0), (0x026AC0BF, 0), (0x9E0C7C62, 0),
        (0xF393B0E0, 0), (0xE0691301, 0), (0xE9DAB877, 4)]),
    0xC547836A: ("FxRopeLineShapeNode", [(0x9E69F4A9, 3), (0xA850262D, 1), (0x81E5CB5E, 2), (0x0BB3ACCC, 2), (0x1CEC027A, 0), (0xF825ECD8, 0),
        (0x8CC43F28, 3), (0x3A424C45, 3), (0x9EAFFD2A, 1), (0x12FB992D, 1), (0xA90978FB, 2), (0x4AF14F8F, 0)]),
    0xEB8ABEE4: ("FxScrollAnimationMaterialNode", [(0x7636E1F6, 1), (0x972FD939, 2), (0x1118DD61, 2), (0x1364CE7E, 2), (0xACB0CDBB, 2),
        (0x6B880E78, 2), (0xF1A308EB, 2), (0x99EA2F62, 2), (0x1F7E7CF7, 2), (0x27F25FE0, 2), (0xF77D9DEA, 4), (0xFBFFDA19, 2), (0x48428ECB, 1),
        (0x0E3C1540, 2), (0x72F33CA7, 4), (0x495EBA72, 2), (0xEDA9BBFB, 2), (0x332DD0CB, 2)]),
    0x8E522023: ("FxSoundCallProgramEffectNode", [(0x37CD447C, 0), (0x2B0AD013, 0), (0x86B5F009, 0), (0x4EF4B1E6, 0), (0x10CA0B28, 0),
        (0x119432AD, 0), (0x6F15D5A0, 0), (0xE3A9CADA, 1), (0xD2ECAC68, 2), (0x17332672, 2), (0xC23372A6, 4), (0xF10F71BA, 4), (0x1EB03412, 4)]),
    0x122E3868: ("FxSoundCallShapeNode", [(0x9E69F4A9, 3), (0xA850262D, 1), (0x1CEC027A, 0), (0xF825ECD8, 0), (0x8CC43F28, 3), (0x3A424C45, 3),
        (0x9EAFFD2A, 1), (0xC23372A6, 4), (0x2C691B64, 4)]),
    0x955F7246: ("FxSpotLightShapeNode", [(0xEC9C106B, 2), (0x9E69F4A9, 3), (0xA850262D, 1), (0xEEA4D248, 0), (0x5FECC762, 2), (0x1CEC027A, 0),
        (0xFFE7D17F, 0), (0x40ECFE36, 0), (0x8068AAE2, 2), (0x733D3780, 3), (0x46D0D39E, 3), (0xC92B9E01, 3), (0xF825ECD8, 0), (0x649CC26A, 1),
        (0x9C167551, 2), (0x39ECAC08, 2), (0x07DEC32F, 1), (0x6446D9D8, 2), (0x8CC43F28, 3), (0x3A424C45, 3), (0x9EAFFD2A, 1), (0x6DAEF640, 2),
        (0xA07B1E26, 4), (0xE9DAB877, 4), (0xC2809567, 2), (0xE142FD96, 2), (0xBDB0BF99, 2), (0xDFBA45D5, 2), (0x26C9C23D, 2)]),
    0x94779FF9: ("FxSpreadVectorNode", [(0xD00A46C2, 2), (0x4F41F8F8, 2), (0xEF65426D, 2), (0x1EB4131F, 1), (0x1F695AD3, 0), (0xD8F07EBD, 0),
        (0xE6B68466, 0), (0xEC22E2C7, 1), (0x05FFD4EB, 1), (0xD4D0DAAA, 2), (0x05B462F3, 3)]),
    0xF845A712: ("FxSprite2DShapeNode", [(0x9E69F4A9, 3), (0x91365199, 1), (0x32FF2253, 1), (0xDE02160D, 1), (0xA850262D, 1), (0x81E5CB5E, 2),
        (0x0BB3ACCC, 2), (0x1CEC027A, 0), (0x5A76071B, 0), (0xF825ECD8, 0), (0x8CC43F28, 3), (0x3A424C45, 3), (0x9EAFFD2A, 1), (0x62E901A7, 1),
        (0x55389195, 0), (0x72F33CA7, 4)]),
    0xA51C93DC: ("FxSpriteRotShapeNode", [(0x9E69F4A9, 3), (0xA850262D, 1), (0x81E5CB5E, 2), (0x0BB3ACCC, 2), (0x1CEC027A, 0), (0x43B4F24E, 0),
        (0xF825ECD8, 0), (0x8CC43F28, 3), (0x3A424C45, 3), (0x9EAFFD2A, 1), (0x94390DB1, 0), (0xE5014C1B, 0), (0xE434246B, 1), (0xA90978FB, 2)]),
    0xC31E5C16: ("FxSpriteShapeNode", [(0x9E69F4A9, 3), (0xA850262D, 1), (0x81E5CB5E, 2), (0x0BB3ACCC, 2), (0x1CEC027A, 0), (0x43B4F24E, 0),
        (0xF825ECD8, 0), (0x8CC43F28, 3), (0x3A424C45, 3), (0x9EAFFD2A, 1), (0xE434246B, 1), (0xA90978FB, 2)]),
    0x46D32803: ("FxSubtractionVectorNode", [(0x6FE5383F, 2), (0x6D3F4A3F, 0), (0xF3E0E378, 0), (0x970C0E28, 0), (0x5D6AF65B, 0)]),
    0x0B18D6A3: ("FxTimeCircleVectorNode", [(0xB39DCF4C, 2)]),
    0x64005093: ("FxTimeCurveVectorNode", [(0x7482C030, 1)]),
    0xE08647E7: ("FxTimeScaleVectorNode", [(0x06C4552D, 2), (0x47A7A4D1, 0), (0xC4B7E974, 0), (0xB79D2D2F, 0), (0x78669CAC, 0), (0xE01E4183, 2)]),
    0xBC411D37: ("FxTrailShapeNode", [(0x6A15D311, 2), (0x9A1C2468, 0), (0x452BE79C, 2), (0x008A47F0, 2), (0xF9F26E77, 2), (0xEB0DEA83, 1),
        (0x9E69F4A9, 3), (0x36998A19, 2), (0xA850262D, 1), (0x1CEC027A, 0), (0x715B1280, 1), (0x612D357A, 1), (0xF825ECD8, 0), (0x8CC43F28, 3),
        (0x3A424C45, 3), (0xA82D6DEC, 1), (0x9EAFFD2A, 1), (0xB022DFE2, 0), (0xA90978FB, 2), (0xC156B9A0, 0), (0xD8A202ED, 1), (0x9DEA0CDB, 1)]),
    0xB2105308: ("FxUVAnimeIntervalVectorNode", [(0xEBAF6352, 2), (0x0C089291, 0), (0x9EC5A541, 1), (0x1D121378, 1), (0xAEADF7F1, 0),
        (0x902AA0EB, 0), (0x0199F7F9, 0), (0xD3C64F5A, 0), (0xEC22E2C7, 1), (0x05FFD4EB, 1), (0x2EDD498B, 0)]),
    0x4AE19F65: ("FxUVMapRandomVectorNode", [(0x0A3122DF, 1), (0x4DF2F53A, 1), (0x0199F7F9, 0), (0xD3C64F5A, 0), (0xEC22E2C7, 1), (0x05FFD4EB, 1)]),
    0x0680F7C8: ("FxUVMapVectorNode", [(0x9EC5A541, 1), (0x1D121378, 1), (0xAEADF7F1, 0), (0x902AA0EB, 0), (0x4296121B, 3)]),
    0x6A621D6B: ("FxUniformAccelTimeVectorNode", []),
    0xF7CB5A60: ("FxUniformAccelVectorNode", []),
    0x29C60EBC: ("FxUniformVelocityTimeVectorNode", []),
    0x09FD2AE8: ("FxUniformVelocityVectorNode", []),
    0x76D36F96: ("FxUpdateSleepControlNode", [(0xE6335039, 2), (0x03043315, 2), (0xDE057317, 2), (0xDE2DB1F0, 1), (0xF69AD7BB, 0)]),
    0x882CF671: ("FxWindEmitNode", [(0x6ECAA185, 2), (0xADC44F51, 2)]),
    0x26917BE9: ("FxWorldPosDeltaVectorNode", [(0xECBB13BB, 2), (0x8007000E, 2), (0xC0883E50, 2), (0x51F86791, 0), (0xBF8F71BC, 2)]),
    0x76F8F884: ("ShScalableScrollAnimationMaterialNode", [(0x7636E1F6, 1), (0x217CE1B0, 2), (0x362A038B, 2), (0x972FD939, 2), (0x1118DD61, 2),
        (0x1364CE7E, 2), (0xACB0CDBB, 2), (0x6B880E78, 2), (0xF1A308EB, 2), (0x99EA2F62, 2), (0x1F7E7CF7, 2), (0x27F25FE0, 2), (0xF77D9DEA, 4),
        (0xFBFFDA19, 2), (0x48428ECB, 1), (0x0E3C1540, 2), (0x72F33CA7, 4), (0x495EBA72, 2), (0xEDA9BBFB, 2), (0x332DD0CB, 2)]),
    0x5C205536: ("TppLensFlareProgramEffectNode", [(0x2118156A, 2), (0x84F2038B, 1), (0xA70631A0, 6), (0x9B1C6BA4, 4), (0x411248AE, 2),
        (0x2642E962, 2), (0x5392278B, 2), (0x2BF87B1B, 1), (0xCC495791, 3), (0xE9DAB877, 4), (0x32CA14EC, 0), (0x430D5D92, 2), (0xF856246F, 2)]),
    0x598FC1E5: ("TppLiquidMaterial2HNMNode", [(0xADCCD482, 2), (0x217CE1B0, 2), (0x362A038B, 2), (0x972FD939, 2), (0x23E90E5F, 4),
        (0x41488ACD, 2), (0xF6B16443, 2), (0xB65BBD54, 0), (0x9A230069, 2), (0xBD8530BE, 2), (0x3B831F9F, 2), (0x62133294, 2), (0x4C08ECC7, 2),
        (0x50E78B53, 0), (0x0E3C1540, 2), (0xBB53E22D, 2), (0x344C4F9B, 1), (0xD7FBBE28, 1), (0x99C0EF09, 0), (0xBAF73B2F, 0), (0x72F33CA7, 4),
        (0x09DC8591, 2)]),
    0x2E60BF85: ("TppLiquidMaterial2Node", [(0xADCCD482, 2), (0x217CE1B0, 2), (0x362A038B, 2), (0x972FD939, 2), (0x23E90E5F, 4), (0x41488ACD, 2),
        (0xF6B16443, 2), (0xB65BBD54, 0), (0x9A230069, 2), (0xBD8530BE, 2), (0x3B831F9F, 2), (0x62133294, 2), (0x4C08ECC7, 2), (0x50E78B53, 0),
        (0x0E3C1540, 2), (0xBB53E22D, 2), (0x344C4F9B, 1), (0xD7FBBE28, 1), (0x99C0EF09, 0), (0xBAF73B2F, 0), (0x72F33CA7, 4), (0x09DC8591, 2)]),
    0x0F8A4AA5: ("TppLiquidMaterialNode", [(0x94CCCDD8, 3), (0xE9BEF4AD, 2), (0x522F0D83, 2), (0xDC56DB8A, 2), (0x6EA4260E, 2), (0xF46F5FFF, 2),
        (0x7D9E955B, 2), (0x2599BDE5, 2), (0x9AC5DD02, 2), (0x067666C3, 2), (0xB65BBD54, 0), (0x50C04326, 0), (0x50E78B53, 0), (0x0E3C1540, 2),
        (0xB92B0B96, 2), (0x86E71BA4, 2), (0x344C4F9B, 1), (0x8C9F7689, 2), (0xD7FBBE28, 1), (0xBAF73B2F, 0), (0x72F33CA7, 4)]),
    0x23C4BE1D: ("WindFxVectorNode", [(0x3D1BF456, 2), (0x4067A508, 0), (0xD614669B, 1)]),
    0x5567C837: ("WindFxWindFlowVectorNode", [(0x3D1BF456, 2), (0xB5734710, 2), (0x13226820, 0)]),
}

NAMES = {
    0x0199F7F9: "randomFlipU",
    0x05B462F3: "spreadRot",
    0x05FFD4EB: "randomGatherType",
    0x067666C3: "distortionRate",
    0x06C4552D: "endScale",
    0x07DEC32F: "lodRadiusLevel",
    0x09DC8591: "transparency",
    0x0A3122DF: "randomDivisionHeightGrid",
    0x0A539A6A: "num",
    0x0BB3ACCC: "centerV",
    0x0C089291: "clamp",
    0x0DE810D9: "method",
    0x0E3C1540: "softBlendFactor",
    0x0F2216D0: "cullFace",
    0x1118DD61: "luminance",
    0x12FB992D: "statisticsWords",
    0x14DDEF6E: "addPool",
    0x17332672: "lodDistance",
    0x180E7B16: "distortion",
    0x1AFFA980: "width",
    0x1B98FED9: "lineType",
    0x1CEC027A: "enable",
    0x1EB03412: "soundStop",
    0x1EB4131F: "forceType",
    0x1F27A68D: "lodType",
    0x1F695AD3: "global",
    0x2118156A: "baseDistance",
    0x217CE1B0: "cameraFadeInFar",
    0x2551466C: "xySquere",
    0x25A4D811: "fadeFrame",
    0x25D54FF0: "vectorName",
    0x261502A4: "keyframeMethod",
    0x26C9C23D: "viewBias",
    0x2B0AD013: "enableLod",
    0x2BF87B1B: "numFlare",
    0x2EDD498B: "randomStart",
    0x2F55184A: "vectorType",
    0x3096A361: "amplitude",
    0x309EF6C9: "lifeRandomRangeFrame",
    0x3231D709: "defaultVector",
    0x33FE189E: "lifeFrame",
    0x344C4F9B: "textureAnimeBlendHeight",
    0x355BBCF5: "zValues",
    0x35971A79: "periodicity",
    0x362A038B: "cameraFadeInNear",
    0x36998A19: "baseSizeScale",
    0x380ADBEB: "playMode",
    0x39ECAC08: "lodNearSize",
    0x3A424C45: "manualBoundingBoxSize",
    0x3C0849CE: "rotGlobal",
    0x3C8D5204: "fadeOutPosition",
    0x3CEF4C99: "nearScale",
    0x3D1BF456: "airResistanceRate",
    0x3DBF79B0: "times",
    0x3F606248: "rotateType",
    0x3FF4A421: "cameraCoordinate",
    0x4078CFD1: "numY",
    0x40BE742E: "textureAnimeBlend",
    0x40EB719E: "poolName",
    0x40ECFE36: "hasSpecular",
    0x411248AE: "limitDistance",
    0x4114D9A8: "fadeOutStartFrame",
    0x41488ACD: "directionalLightRate",
    0x42FAFE2B: "executionPriorityType",
    0x430E9E8E: "outputType",
    0x452BE79C: "alphaPosition",
    0x46D0D39E: "lightAreaScale",
    0x47A7A4D1: "maskW",
    0x4819755F: "nearDist",
    0x48428ECB: "shaderType",
    0x48503BF9: "delayFrame",
    0x48ADB96A: "vector",
    0x49780D5C: "updateType",
    0x4A226D21: "invertFace",
    0x4AF14F8F: "spline",
    0x4BA61406: "boundingBoxSize",
    0x4BAF9414: "swelling",
    0x4C08ECC7: "roughness",
    0x4C4B29BF: "reverse",
    0x4C8B8306: "debugDraw",
    0x4CD9C31F: "delayFrameRandomRange",
    0x4DBFDB86: "numW",
    0x4DF2F53A: "randomDivisionWidthGrid",
    0x4E16168C: "planeRot",
    0x4F41F8F8: "force",
    0x50C04326: "shadowed",
    0x50E78B53: "softBlend",
    0x51D15822: "baseRot",
    0x527D0675: "lightSize",
    0x5392278B: "lux",
    0x5C1B5DF3: "modelFile",
    0x5CB268C2: "shadowEnable",
    0x5D6AF65B: "secondMaskZ",
    0x5F70F6A6: "beginPosition",
    0x5FECC762: "dimmer",
    0x62E901A7: "priority",
    0x649CC26A: "lodFadeType",
    0x6AC9578A: "numMax",
    0x6D3F4A3F: "secondMaskW",
    0x6DAEF640: "outerRange",
    0x6DDAE7F8: "probability",
    0x6EA4260E: "alphaScale",
    0x6ECAA185: "sensitivity",
    0x6FC9EBDD: "farDist",
    0x6FE5383F: "maskValue",
    0x72F33CA7: "textureFile",
    0x7636E1F6: "blendType",
    0x78669CAC: "maskZ",
    0x78F17705: "axisFix",
    0x78F4ACF2: "color",
    0x7D9E955B: "diffuseMin",
    0x7E85E8D8: "farScale",
    0x7EE4CB40: "randomMin",
    0x8068AAE2: "innerRange",
    0x81E5CB5E: "centerU",
    0x84F2038B: "drawPriority",
    0x86E71BA4: "speculerSharpness",
    0x88F0F0A9: "projectionType",
    0x8941E3B3: "lineDetail",
    0x8A0C7B2B: "moduleName",
    0x8AB1CA2B: "lineWidth",
    0x8CC43F28: "manualBoundingBoxOffset",
    0x8DB85414: "fadeInEndFrame",
    0x8FD2A63F: "animationType",
    0x902AA0EB: "flipV",
    0x90F78320: "yValues",
    0x91E731AD: "range",
    0x95FB860B: "numMin",
    0x96237796: "numZ",
    0x970C0E28: "secondMaskY",
    0x972FD939: "cameraZOffset",
    0x9761B6D5: "intervalFrame",
    0x99C0EF09: "textureAnimeClamp",
    0x9A1C2468: "alphaInverse",
    0x9A230069: "pointLightRate",
    0x9AA63A20: "correctDistance",
    0x9B1C6BA4: "lensFlareName",
    0x9B835C1F: "numX",
    0x9BA4FB3B: "effectName",
    0x9C167551: "lodFarSize",
    0x9E69F4A9: "autoBoundingBoxMargin",
    0x9EAFFD2A: "numSimulatedMaxParticle",
    0x9F381F3B: "endPosition",
    0xA2EAD6CA: "randomize",
    0xA4607F13: "startRot",
    0xA850262D: "boundingBoxType",
    0xA90978FB: "sortOffset",
    0xAA60D112: "endRot",
    0xAD7CD8F6: "axisFixParticleDirectionPoolName",
    0xAD94236D: "receiveShadowMap",
    0xADCCD482: "ambientRate",
    0xAEADF7F1: "flipU",
    0xB022DFE2: "shapeDrag",
    0xB07EC7A5: "count",
    0xB13FD162: "materialParameters",
    0xB1AAE4DC: "randomMax",
    0xB45EFCBE: "power",
    0xB5734710: "curveFactor",
    0xB65BBD54: "opaque",
    0xB79D2D2F: "maskY",
    0xB92B0B96: "speculerPower",
    0xBAF73B2F: "textureAnimeRandomStart",
    0xBB53E22D: "textureAnimeBlendFrame",
    0xBDB0BF99: "shadowPenumbraAngleScale",
    0xBE7AE6F0: "inverse",
    0xBEDD7B1A: "outMode",
    0xC0508FF5: "wValues",
    0xC0883E50: "scale",
    0xC23372A6: "signalName",
    0xC2809567: "shadowAttenuationExponent",
    0xC31A37A7: "screenSpace",
    0xC44EE238: "defaultVectors",
    0xC4B7E974: "maskX",
    0xC633B47F: "rotateOrderType",
    0xC90B7E24: "timeRatio",
    0xC92B9E01: "lightAreaTranslation",
    0xCC495791: "offsets",
    0xCDC62989: "allFrame",
    0xCF9D35A1: "shapeType",
    0xD00A46C2: "elevation",
    0xD0343D4B: "nearClipScale",
    0xD2B8C5F0: "drag",
    0xD2D302AC: "xValues",
    0xD3C64F5A: "randomFlipV",
    0xD43DE798: "baseColor",
    0xD4D0DAAA: "rangeAngle",
    0xD620F511: "fadeOutReverse",
    0xD7EEB2EE: "angleX",
    0xD7FBBE28: "textureAnimeBlendWidth",
    0xD865337D: "boundingBoxOffsetPos",
    0xDB2E8027: "frequency",
    0xDC56DB8A: "alphaOffset",
    0xDE02160D: "blendMode",
    0xDE2DB1F0: "sleepType",
    0xDFBA45D5: "shadowUmbraAngleScale",
    0xDFC9ED5B: "debugInfo",
    0xE01E4183: "startScale",
    0xE142FD96: "shadowBias",
    0xE210591C: "correctionType",
    0xE2937ED7: "minExposure",
    0xE3B399C2: "filterType",
    0xE434246B: "sortMode",
    0xE6335039: "cameraDistance",
    0xE7F1341A: "zTimes",
    0xE9DAB877: "receiveName",
    0xEA72A4BE: "angleY",
    0xEB0DEA83: "attenuationMode",
    0xEB6517B9: "yTimes",
    0xEBAF6352: "animationFrame",
    0xEC22E2C7: "randomGatherSeedValue",
    0xEC9C106B: "attenuationExponent",
    0xECBB13BB: "delayTime",
    0xEEA4D248: "castShadow",
    0xEECAF4DF: "localCoordinate",
    0xF0351193: "endAngle",
    0xF10F71BA: "soundEvent",
    0xF3E0E378: "secondMaskX",
    0xF46F5FFF: "diffuseMax",
    0xF5E50B70: "xTimes",
    0xF62B0158: "globalEvaluateRealTimeRootRotate",
    0xF6B4B0EF: "values",
    0xF825ECD8: "localSpace",
    0xF88410C1: "startAngle",
    0xF8CE4CAA: "maxExposure",
    0xFB83BB36: "wTimes",
    0xFF8073A8: "angleZ",
    0xFFE7D17F: "enableLightArea",
}


def property_name(code):
    return NAMES.get(code) or "h%08X" % code


class VfxError(Exception):
    pass


def read_value(data, offset, kind):
    if kind == TYPE_STRING:
        length = struct.unpack_from("<H", data, offset)[0]
        text = data[offset + 2:offset + 2 + length].decode("latin-1")
        if data[offset + 2 + length] != 0:
            raise VfxError("string at 0x%X is not terminated" % offset)
        return text, offset + 3 + length
    fmt, size = FIXED[kind]
    value = struct.unpack_from(fmt, data, offset)
    return (list(value) if len(value) > 1 else value[0]), offset + size


def parse(data):
    if len(data) < 0x10 or data[:3] != b"vfx":
        raise VfxError("not a vfx file")
    version, node_count, edge_count = struct.unpack_from("<HHH", data, 3)
    offset = HEADER_SIZE
    nodes = []
    for index in range(node_count):
        code = struct.unpack_from("<Q", data, offset)[0]
        entry = CLASSES.get(code & 0xFFFFFFFF)
        if entry is None:
            raise VfxError("node %d at 0x%X: unknown class %012X" % (index, offset, code))
        name, props = entry
        node = {"index": index, "class": name, "offset": offset, "properties": {}}
        offset += 8
        for prop_code, kind in props:
            count = data[offset]
            offset += 1
            values = []
            for _ in range(count):
                value, offset = read_value(data, offset, kind)
                values.append(value)
            node["properties"][property_name(prop_code)] = {"type": TYPE_NAMES[kind], "values": values}
        nodes.append(node)
    wide = node_count >= 0xFF
    edges = []
    for _ in range(edge_count):
        if wide:
            src, dst = struct.unpack_from("<HH", data, offset)
            offset += 4
        else:
            src, dst = data[offset], data[offset + 1]
            offset += 2
        src_type, src_port, dst_type, dst_port = data[offset:offset + 4]
        offset += 4
        edges.append({"from": src, "to": dst, "fromType": src_type, "fromPort": src_port, "toType": dst_type, "toPort": dst_port})
    if offset != len(data):
        raise VfxError("%d trailing bytes after the edges" % (len(data) - offset))
    return {"version": version, "nodes": nodes, "edges": edges}


def resolve(doc, pathids):
    for node in doc["nodes"]:
        for prop in node["properties"].values():
            if prop["type"] == "StrCode64":
                prop["values"] = ["0x%012X" % v for v in prop["values"]]
            elif prop["type"] == "PathCode64":
                prop["values"] = [pathids.get(v, "0x%016X" % v) for v in prop["values"]]
    return doc


def load_pathids(path):
    if path and Path(path).exists():
        return read_pathid_list(Path(path))
    return {}


def short(value):
    if isinstance(value, float):
        return round(value, 6)
    if isinstance(value, list):
        return [short(v) for v in value]
    return value


def graph_text(doc, name):
    lines = ["%s (version %d, %d nodes, %d edges)" % (name, doc["version"], len(doc["nodes"]), len(doc["edges"]))]
    inputs = defaultdict(list)
    for e in doc["edges"]:
        inputs[e["to"]].append(e)
    for node in doc["nodes"]:
        lines.append("  [%d] %s" % (node["index"], node["class"]))
        for key, prop in node["properties"].items():
            values = prop["values"]
            if values:
                lines.append("      %s = %s" % (key, short(values[0] if len(values) == 1 else values)))
        for e in sorted(inputs[node["index"]], key=lambda e: e["toPort"]):
            source = doc["nodes"][e["from"]]["class"]
            lines.append("      <- in %d.%d from [%d] %s out %d.%d" % (e["toType"], e["toPort"], e["from"], source, e["fromType"],
                                                                        e["fromPort"]))
    return "\n".join(lines)


def collect(inputs):
    files = []
    for item in inputs:
        p = Path(item)
        files += sorted(p.rglob("*.vfx")) if p.is_dir() else [p]
    return files


def command_dump(args):
    pathids = load_pathids(args.pathids)
    out = Path(args.out) if args.out else None
    failed = 0
    written = set()
    for path in collect(args.inputs or [DEFAULT_FPK_ROOT]):
        try:
            doc = resolve(parse(path.read_bytes()), pathids)
        except VfxError as e:
            print("%s: %s" % (path, e))
            failed += 1
            continue
        if args.text:
            print(graph_text(doc, path.name))
        elif out is None:
            print(json.dumps(doc, indent=1))
        elif path.name not in written:
            written.add(path.name)
            target = out / path.parent.name / (path.stem + ".json")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(json.dumps(doc, indent=1), encoding="utf-8")
    if out is not None:
        print("%d files written to %s, %d failed" % (len(written), out, failed))
    return 1 if failed else 0


def command_summary(args):
    usage = defaultdict(set)
    files = {}
    for path in collect(args.inputs or [DEFAULT_FPK_ROOT]):
        files.setdefault(path.name, path)
    for name, path in sorted(files.items()):
        for node in parse(path.read_bytes())["nodes"]:
            usage[node["class"]].add(path.stem)
    print("%d unique files" % len(files))
    for cls in sorted(usage, key=lambda c: (-len(usage[c]), c)):
        visual = sorted(n for n in usage[cls] if not n.startswith("fxsd_"))
        print("%-38s %3d  %s" % (cls, len(usage[cls]), " ".join(visual)))
    return 0


def command_schema(args):
    for code, (name, props) in sorted(CLASSES.items(), key=lambda kv: kv[1][0]):
        if args.classes and name not in args.classes:
            continue
        print("%s (%08X)" % (name, code))
        for prop_code, kind in props:
            print("    %-36s %-10s %08X" % (property_name(prop_code), TYPE_NAMES[kind], prop_code))
    return 0


def main():
    ap = argparse.ArgumentParser(description="Fox Engine .vfx (effect node graph) reader for P.T.")
    sub = ap.add_subparsers(dest="command", required=True)
    dump = sub.add_parser("dump", help="parse .vfx files and print JSON, write it per file with --out, or print a text graph")
    dump.add_argument("inputs", nargs="*", help=".vfx files or folders (default: dump/fpk)")
    dump.add_argument("--out", help="output folder, one JSON per unique file name")
    dump.add_argument("--text", action="store_true", help="print a readable node graph instead of JSON")
    dump.add_argument("--pathids", default=str(DEFAULT_PATHIDS), help="pathid_list_ps4.bin to name PathCode64 values")
    dump.set_defaults(handler=command_dump)
    summary = sub.add_parser("summary", help="list node classes and the files that use them")
    summary.add_argument("inputs", nargs="*")
    summary.set_defaults(handler=command_summary)
    schema = sub.add_parser("schema", help="print the node class schema")
    schema.add_argument("classes", nargs="*")
    schema.set_defaults(handler=command_schema)
    args = ap.parse_args()
    sys.exit(args.handler(args))


if __name__ == "__main__":
    main()
