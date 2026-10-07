import argparse
import json
import struct
from collections import Counter
from pathlib import Path

SUPPORTED_VERSION = 88

HIRC_TYPE_NAMES = {
    0x01: "State",
    0x02: "Sound",
    0x03: "Action",
    0x04: "Event",
    0x05: "RanSeqCntr",
    0x06: "SwitchCntr",
    0x07: "ActorMixer",
    0x08: "Bus",
    0x09: "LayerCntr",
    0x0A: "MusicSegment",
    0x0B: "MusicTrack",
    0x0C: "MusicSwitchCntr",
    0x0D: "MusicRanSeqCntr",
    0x0E: "Attenuation",
    0x0F: "DialogueEvent",
    0x10: "FeedbackBus",
    0x11: "FeedbackNode",
    0x12: "FxShareSet",
    0x13: "FxCustom",
    0x14: "AuxBus",
}

PROP_NAMES = {
    0x00: "Volume", 0x01: "LFE", 0x02: "Pitch", 0x03: "LPF", 0x04: "BusVolume", 0x05: "Priority",
    0x06: "PriorityDistanceOffset", 0x07: "Loop", 0x08: "FeedbackVolume", 0x09: "FeedbackLPF",
    0x0A: "MuteRatio", 0x0B: "PAN_LR", 0x0C: "PAN_FR", 0x0D: "CenterPCT", 0x0E: "DelayTime",
    0x0F: "TransitionTime", 0x10: "Probability", 0x11: "DialogueMode", 0x12: "UserAuxSendVolume0",
    0x13: "UserAuxSendVolume1", 0x14: "UserAuxSendVolume2", 0x15: "UserAuxSendVolume3",
    0x16: "GameAuxSendVolume", 0x17: "OutputBusVolume", 0x18: "OutputBusLPF", 0x19: "InitialDelay",
    0x1A: "HDRBusThreshold", 0x1B: "HDRBusRatio", 0x1C: "HDRBusReleaseTime", 0x1D: "HDRBusGameParam",
    0x1E: "HDRBusGameParamMin", 0x1F: "HDRBusGameParamMax", 0x20: "HDRActiveRange", 0x21: "MakeUpGain",
    0x22: "LoopStart", 0x23: "LoopEnd", 0x24: "TrimInTime", 0x25: "TrimOutTime", 0x26: "FadeInTime",
    0x27: "FadeOutTime", 0x28: "FadeInCurve", 0x29: "FadeOutCurve", 0x2A: "LoopCrossfadeDuration",
    0x2B: "CrossfadeUpCurve", 0x2C: "CrossfadeDownCurve",
}

INTEGER_PROPS = {"Loop", "TransitionTime", "DelayTime", "DialogueMode", "FadeInCurve", "FadeOutCurve",
                 "CrossfadeUpCurve", "CrossfadeDownCurve", "HDRBusGameParam"}

RTPC_PARAMETER_NAMES = {
    0x00: "Volume", 0x01: "LFE", 0x02: "Pitch", 0x03: "LPF", 0x04: "BusVolume",
    0x05: "PlayMechanismSpecialTransitionsValue", 0x06: "InitialDelay", 0x08: "Priority",
    0x09: "MaxNumInstances", 0x0A: "PositioningType", 0x0B: "Positioning_Divergence_Center_PCT",
    0x0C: "Positioning_Cone_Attenuation_ON_OFF", 0x0D: "Positioning_Cone_Attenuation",
    0x0E: "Positioning_Cone_LPF", 0x0F: "UserAuxSendVolume0", 0x10: "UserAuxSendVolume1",
    0x11: "UserAuxSendVolume2", 0x12: "UserAuxSendVolume3", 0x13: "GameAuxSendVolume",
    0x14: "Position_PAN_X_2D", 0x15: "Position_PAN_Y_2D", 0x16: "OutputBusVolume", 0x17: "OutputBusLPF",
    0x18: "BypassFX0", 0x19: "BypassFX1", 0x1A: "BypassFX2", 0x1B: "BypassFX3", 0x1C: "BypassAllFX",
    0x1D: "FeedbackVolume", 0x1E: "FeedbackLowpass", 0x1F: "FeedbackPitch", 0x20: "HDRBusThreshold",
    0x21: "HDRBusReleaseTime", 0x22: "HDRBusRatio", 0x23: "HDRActiveRange", 0x24: "MakeUpGain",
    0x25: "Position_PAN_X_3D", 0x26: "Position_PAN_Y_3D",
}

CURVE_SCALING_NAMES = {0: "None", 2: "dB", 3: "Log", 4: "dBToLin"}

CURVE_INTERPOLATION_NAMES = {
    0: "Log3", 1: "Sine", 2: "Log1", 3: "InvSCurve", 4: "Linear", 5: "SCurve", 6: "Exp1",
    7: "SineRecip", 8: "Exp3", 9: "Constant",
}

ACTION_TYPE_NAMES = {
    0x1204: "SetState", 0x1A02: "BypassFX_M", 0x1A03: "BypassFX_O", 0x1B02: "ResetBypassFX_M",
    0x1B03: "ResetBypassFX_O", 0x1B04: "ResetBypassFX_ALL", 0x1B05: "ResetBypassFX_ALL_O",
    0x1B08: "ResetBypassFX_AE", 0x1B09: "ResetBypassFX_AE_O", 0x1901: "SetSwitch", 0x1002: "UseState_E",
    0x1102: "UnuseState_E", 0x0403: "Play", 0x0503: "PlayAndContinue", 0x0102: "Stop_E", 0x0103: "Stop_E_O",
    0x0104: "Stop_ALL", 0x0105: "Stop_ALL_O", 0x0108: "Stop_AE", 0x0109: "Stop_AE_O", 0x0202: "Pause_E",
    0x0203: "Pause_E_O", 0x0204: "Pause_ALL", 0x0205: "Pause_ALL_O", 0x0208: "Pause_AE", 0x0209: "Pause_AE_O",
    0x0302: "Resume_E", 0x0303: "Resume_E_O", 0x0304: "Resume_ALL", 0x0305: "Resume_ALL_O",
    0x0308: "Resume_AE", 0x0309: "Resume_AE_O", 0x1C02: "Break_E", 0x1C03: "Break_E_O", 0x0602: "Mute_M",
    0x0603: "Mute_O", 0x0702: "Unmute_M", 0x0703: "Unmute_O", 0x0704: "Unmute_ALL", 0x0705: "Unmute_ALL_O",
    0x0708: "Unmute_AE", 0x0709: "Unmute_AE_O", 0x0A02: "SetVolume_M", 0x0A03: "SetVolume_O",
    0x0B02: "ResetVolume_M", 0x0B03: "ResetVolume_O", 0x0B04: "ResetVolume_ALL", 0x0B05: "ResetVolume_ALL_O",
    0x0B08: "ResetVolume_AE", 0x0B09: "ResetVolume_AE_O", 0x0802: "SetPitch_M", 0x0803: "SetPitch_O",
    0x0902: "ResetPitch_M", 0x0903: "ResetPitch_O", 0x0904: "ResetPitch_ALL", 0x0905: "ResetPitch_ALL_O",
    0x0908: "ResetPitch_AE", 0x0909: "ResetPitch_AE_O", 0x0E02: "SetLPF_M", 0x0E03: "SetLPF_O",
    0x0F02: "ResetLPF_M", 0x0F03: "ResetLPF_O", 0x0F04: "ResetLPF_ALL", 0x0F05: "ResetLPF_ALL_O",
    0x0F08: "ResetLPF_AE", 0x0F09: "ResetLPF_AE_O", 0x0C02: "SetBusVolume_M", 0x0C03: "SetBusVolume_O",
    0x0D02: "ResetBusVolume_M", 0x0D03: "ResetBusVolume_O", 0x0D04: "ResetBusVolume_ALL",
    0x0D08: "ResetBusVolume_AE", 0x1511: "StopEvent", 0x1611: "PauseEvent", 0x1711: "ResumeEvent",
    0x1820: "Duck", 0x1D00: "Trigger", 0x1D01: "Trigger_O", 0x1E02: "Seek_E", 0x1E03: "Seek_E_O",
    0x1E04: "Seek_ALL", 0x1E05: "Seek_ALL_O", 0x1E08: "Seek_AE", 0x1E09: "Seek_AE_O",
    0x1302: "SetGameParameter", 0x1303: "SetGameParameter_O", 0x1402: "ResetGameParameter",
    0x1403: "ResetGameParameter_O",
}

ACTION_CLASS_BY_CATEGORY = {
    0x01: "Stop", 0x02: "Pause", 0x03: "Resume", 0x04: "Play", 0x05: "Play", 0x06: "SetValue", 0x07: "SetValue",
    0x08: "SetAkProp", 0x09: "SetAkProp", 0x0A: "SetAkProp", 0x0B: "SetAkProp", 0x0C: "SetAkProp",
    0x0D: "SetAkProp", 0x0E: "SetAkProp", 0x0F: "SetAkProp", 0x10: "NoParams", 0x11: "NoParams",
    0x12: "SetState", 0x13: "SetGameParameter", 0x14: "SetGameParameter", 0x15: "NoParams", 0x16: "NoParams",
    0x17: "NoParams", 0x19: "SetSwitch", 0x1A: "BypassFX", 0x1B: "BypassFX", 0x1C: "NoParams",
    0x1D: "NoParams", 0x1E: "Seek", 0x20: "SetAkProp", 0x30: "SetAkProp",
}

SOURCE_TYPE_NAMES = {0: "Data", 1: "Streaming", 2: "PrefetchStreaming"}

CODEC_NAMES = {
    0x00010001: "PCM", 0x00020001: "ADPCM", 0x00030001: "XMA", 0x00040001: "Vorbis", 0x00050001: "WiiADPCM",
    0x00080001: "ExternalSource", 0x00090001: "xWMA", 0x000A0001: "AAC", 0x000C0001: "ATRAC9",
}

PLUGIN_NAMES = {
    0x00640002: "Wwise Sine", 0x00650002: "Wwise Silence", 0x00660002: "Wwise Tone Generator",
    0x00690003: "Wwise Parametric EQ", 0x006A0003: "Wwise Delay", 0x006C0003: "Wwise Compressor",
    0x006D0003: "Wwise Expander", 0x006E0003: "Wwise Peak Limiter", 0x00730003: "Wwise Matrix Reverb",
    0x00760003: "Wwise RoomVerb", 0x007D0003: "Wwise Flanger", 0x007E0003: "Wwise Guitar Distortion",
    0x007F0003: "Wwise Convolution Reverb", 0x00810003: "Wwise Meter", 0x00820003: "Wwise Time Stretch",
    0x00830003: "Wwise Tremolo", 0x00870003: "Wwise Stereo Delay", 0x00880003: "Wwise Pitch Shifter",
    0x008A0003: "Wwise Harmonizer", 0x008B0003: "Wwise Gain", 0x01950005: "Wwise Motion Generator",
}

ATTENUATION_CURVE_SLOTS = ["VolumeDry", "VolumeAuxGameDef", "VolumeAuxUserDef", "LowPassFilter", "Spread"]

POSITIONING_3D_TYPE_NAMES = {0: "UserDefined", 1: "GameDefined"}

PATH_MODE_NAMES = {0: "StepSequence", 1: "StepRandom", 2: "ContinuousSequence", 3: "ContinuousRandom",
                   4: "StepSequencePickNewPath", 5: "StepRandomPickNewPath"}

CONTAINER_MODE_NAMES = {0: "Random", 1: "Sequence"}
RANDOM_MODE_NAMES = {0: "Normal", 1: "Shuffle"}
TRANSITION_MODE_NAMES = {0: "Disabled", 1: "CrossFadeAmp", 2: "CrossFadePower", 3: "Delay", 4: "SampleAccurate",
                         5: "TriggerRate"}
GROUP_TYPE_NAMES = {0: "Switch", 1: "State"}
MUSIC_TRACK_TYPE_NAMES = {0: "Normal", 1: "Random", 2: "Sequence"}
MUSIC_PLAYLIST_TYPE_NAMES = {0: "ContinuousSequence", 1: "StepSequence", 2: "ContinuousRandom", 3: "StepRandom",
                             0xFFFFFFFF: "None"}
SYNC_TYPE_NAMES = {0: "Immediate", 1: "NextGrid", 2: "NextBar", 3: "NextBeat", 4: "NextMarker",
                   5: "NextUserMarker", 6: "EntryMarker", 7: "ExitMarker", 8: "ExitNever", 9: "LastExitPosition"}
VALUE_MEANING_NAMES = {0: "Default", 1: "Independent", 2: "Offset"}
VIRTUAL_QUEUE_NAMES = {0: "FromBeginning", 1: "FromElapsedTime", 2: "Resume"}
BELOW_THRESHOLD_NAMES = {0: "ContinueToPlay", 1: "KillVoice", 2: "SetAsVirtualVoice", 3: "KillIfOneShotElseVirtual"}


class BankError(Exception):
    pass


class Reader:
    def __init__(self, data: bytes, pos: int = 0, end: int = None):
        self.data = data
        self.pos = pos
        self.end = len(data) if end is None else end

    def take(self, fmt: str):
        size = struct.calcsize(fmt)
        if self.pos + size > self.end:
            raise BankError("read past end at 0x%X" % self.pos)
        value = struct.unpack_from("<" + fmt, self.data, self.pos)
        self.pos += size
        return value[0] if len(value) == 1 else value

    def u8(self):
        return self.take("B")

    def s8(self):
        return self.take("b")

    def u16(self):
        return self.take("H")

    def u32(self):
        return self.take("I")

    def s32(self):
        return self.take("i")

    def f32(self):
        return round(self.take("f"), 6)

    def f64(self):
        return self.take("d")

    def raw(self, size: int) -> bytes:
        if self.pos + size > self.end:
            raise BankError("read past end at 0x%X" % self.pos)
        blob = self.data[self.pos:self.pos + size]
        self.pos += size
        return blob

    def remaining(self) -> int:
        return self.end - self.pos


def fnv1_32(name: str) -> int:
    value = 2166136261
    for byte in name.lower().encode("utf-8"):
        value = (value * 16777619) & 0xFFFFFFFF
        value ^= byte
    return value


def plugin_type(plugin_id: int) -> int:
    return plugin_id & 0xF


def plugin_name(plugin_id: int) -> str:
    return CODEC_NAMES.get(plugin_id) or PLUGIN_NAMES.get(plugin_id) or "0x%08X" % plugin_id


def union_value(prop_name: str, raw: int):
    unknown_small_int = prop_name.startswith("Prop_") and raw <= 0x10000000
    if prop_name in INTEGER_PROPS or unknown_small_int:
        return struct.unpack("<i", struct.pack("<I", raw))[0]
    return round(struct.unpack("<f", struct.pack("<I", raw))[0], 6)


def read_prop_bundle(r: Reader):
    count = r.u8()
    ids = [r.u8() for _ in range(count)]
    props = {}
    for prop_id in ids:
        name = PROP_NAMES.get(prop_id, "Prop_0x%02X" % prop_id)
        props[name] = union_value(name, r.u32())
    return props


def read_ranged_prop_bundle(r: Reader):
    count = r.u8()
    ids = [r.u8() for _ in range(count)]
    props = {}
    for prop_id in ids:
        name = PROP_NAMES.get(prop_id, "Prop_0x%02X" % prop_id)
        props[name] = [union_value(name, r.u32()), union_value(name, r.u32())]
    return props


def read_graph_points(r: Reader, count: int):
    points = []
    for _ in range(count):
        x, y, interpolation = r.f32(), r.f32(), r.u32()
        points.append([x, y, CURVE_INTERPOLATION_NAMES.get(interpolation, interpolation)])
    return points


def read_rtpc_list(r: Reader):
    rtpcs = []
    for _ in range(r.u16()):
        rtpc_id = r.u32()
        param_id = r.u32()
        curve_id = r.u32()
        scaling = r.u8()
        size = r.u16()
        rtpcs.append({
            "rtpc_id": rtpc_id,
            "param": RTPC_PARAMETER_NAMES.get(param_id, "Param_0x%02X" % param_id),
            "curve_id": curve_id,
            "scaling": CURVE_SCALING_NAMES.get(scaling, scaling),
            "points": read_graph_points(r, size),
        })
    return rtpcs


ROOMVERB_LAYOUT = [(name, "f") for name in (
    "decay_time", "hf_damping", "diffusion", "stereo_width", "filter1_gain", "filter1_freq", "filter1_q",
    "filter2_gain", "filter2_freq", "filter2_q", "filter3_gain", "filter3_freq", "filter3_q", "front_level",
    "rear_level", "center_level", "lfe_level", "dry_level", "er_level", "reverb_level")] + [
    ("enable_early_reflections", "B"), ("er_pattern", "I"), ("reverb_delay", "f"), ("room_size", "f"),
    ("er_front_back_delay", "f"), ("density", "f"), ("room_shape", "f"), ("reverb_units", "I"),
    ("enable_tone_controls", "B"), ("filter1_position", "I"), ("filter1_curve", "I"), ("filter2_position", "I"),
    ("filter2_curve", "I"), ("filter3_position", "I"), ("filter3_curve", "I"), ("input_center_level", "f"),
    ("input_lfe_level", "f")] + [(name, "f") for name in (
    "density_delay_min", "density_delay_max", "density_delay_random_percent", "room_shape_min", "room_shape_max",
    "diffusion_delay_scale_percent", "diffusion_delay_max", "diffusion_delay_random_percent", "dc_filter_cut_freq",
    "reverb_unit_input_delay", "reverb_unit_input_delay_random_percent")]

PEAK_LIMITER_LAYOUT = [("threshold", "f"), ("ratio", "f"), ("lookahead", "f"), ("release", "f"),
                       ("output_level", "f"), ("process_lfe", "B"), ("channel_link", "B")]

STEREO_DELAY_LAYOUT = [
    ("left_input_type", "I"), ("left_delay_time", "f"), ("left_feedback", "f"), ("left_cross_feed", "f"),
    ("right_input_type", "I"), ("right_delay_time", "f"), ("right_feedback", "f"), ("right_cross_feed", "f"),
    ("filter_type", "I"), ("filter_gain", "f"), ("filter_frequency", "f"), ("filter_q", "f"), ("dry_level", "f"),
    ("wet_level", "f"), ("front_rear_balance", "f"), ("enable_feedback", "B"), ("enable_cross_feed", "B")]

MATRIX_REVERB_LAYOUT = [("reverb_time", "f"), ("hf_ratio", "f"), ("delay_count", "I"), ("dry_level", "f"),
                        ("wet_level", "f"), ("pre_delay", "f"), ("process_lfe", "B"), ("delay_lengths_mode", "I")]

PLUGIN_PARAM_LAYOUTS = {
    0x00760003: ROOMVERB_LAYOUT,
    0x006E0003: PEAK_LIMITER_LAYOUT,
    0x00870003: STEREO_DELAY_LAYOUT,
    0x00730003: MATRIX_REVERB_LAYOUT,
}


def read_layout(blob: bytes, layout):
    r = Reader(blob)
    params = {}
    for name, fmt in layout:
        params[name] = r.f32() if fmt == "f" else r.take(fmt)
    return params, r


def read_plugin_params(r: Reader, plugin_id: int, size: int):
    blob = r.raw(size)
    layout = PLUGIN_PARAM_LAYOUTS.get(plugin_id)
    if layout:
        try:
            params, rest = read_layout(blob, layout)
            if plugin_id == 0x00730003 and params["delay_lengths_mode"] == 1:
                params["delay_times"] = [rest.f32() for _ in range(params["delay_count"])]
            if rest.remaining() == 0:
                return params
        except BankError:
            pass
        return {"raw": blob.hex()}
    if plugin_id == 0x00650002 and size >= 12:
        duration, minus, plus = struct.unpack_from("<fff", blob)
        return {"duration": round(duration, 6), "random_minus": round(minus, 6), "random_plus": round(plus, 6)}
    if plugin_id == 0x00660002 and size >= 0x41:
        names = ["gain", "start_freq", "stop_freq", "start_freq_rand_min", "start_freq_rand_max"]
        values = struct.unpack_from("<5f", blob)
        params = {k: round(v, 6) for k, v in zip(names, values)}
        params["freq_sweep"] = blob[20]
        sweep, stop_min, stop_max, gen_type, gen_mode = struct.unpack_from("<IffII", blob, 21)
        params.update({"sweep": sweep, "stop_freq_rand_min": round(stop_min, 6),
                       "stop_freq_rand_max": round(stop_max, 6), "wave": gen_type, "mode": gen_mode})
        envelope = struct.unpack_from("<6f", blob, 41)
        for key, value in zip(["fixed_duration", "attack", "decay", "sustain_duration", "sustain_level", "release"], envelope):
            params[key] = round(value, 6)
        if size >= 0x45:
            params["channel_mask"] = struct.unpack_from("<I", blob, 65)[0]
        return params
    return {"raw": blob.hex()}


def read_source(r: Reader):
    plugin_id = r.u32()
    stream_type = r.u32()
    source = {
        "plugin_id": plugin_id,
        "plugin": plugin_name(plugin_id),
        "stream_type": SOURCE_TYPE_NAMES.get(stream_type, stream_type),
        "source_id": r.u32(),
        "file_id": r.u32(),
    }
    if stream_type != 1:
        source["file_offset"] = r.u32()
        source["in_memory_size"] = r.u32()
    bits = r.u8()
    source["language_specific"] = bool(bits & 1)
    source["has_source"] = bool(bits & 2)
    source["externally_supplied"] = bool(bits & 4)
    if plugin_type(plugin_id) in (2, 5):
        size = r.u32()
        if size:
            source["params"] = read_plugin_params(r, plugin_id, size)
    return source


def read_fx_chunks(r: Reader):
    count = r.u8()
    fx = {"count": count}
    if count:
        fx["bypass_bits"] = r.u8()
        fx["slots"] = []
        for _ in range(count):
            fx["slots"].append({"index": r.u8(), "fx_id": r.u32(), "share_set": r.u8(), "rendered": r.u8()})
    return fx


def read_positioning(r: Reader):
    bits = r.u8()
    positioning = {"override_parent": bool(bits & 1), "listener_relative_routing": bool(bits & 2)}
    if not bits & 1:
        return positioning
    has_2d = r.u8()
    has_3d = r.u8()
    positioning["has_2d"] = bool(has_2d)
    positioning["has_3d"] = bool(has_3d)
    if has_2d:
        positioning["enable_panner"] = bool(r.u8())
    if has_3d:
        position_type = r.u32()
        positioning["type_3d"] = POSITIONING_3D_TYPE_NAMES.get(position_type & 3, position_type)
        positioning["attenuation_id"] = r.u32()
        positioning["spatialized"] = bool(r.u8())
        if (position_type & 3) == 1:
            positioning["dynamic"] = bool(r.u8())
        else:
            path_mode = r.u32()
            positioning["path_mode"] = PATH_MODE_NAMES.get(path_mode, path_mode)
            positioning["path_looping"] = bool(r.u8())
            positioning["path_transition_time"] = r.s32()
            positioning["path_follow_orientation"] = bool(r.u8())
            positioning["path_vertices"] = [[r.f32(), r.f32(), r.f32(), r.s32()] for _ in range(r.u32())]
            item_count = r.u32()
            positioning["path_items"] = [[r.u32(), r.u32()] for _ in range(item_count)]
            positioning["path_ranges"] = [[r.f32(), r.f32()] for _ in range(item_count)]
    return positioning


def read_aux_params(r: Reader):
    aux = {
        "override_game_aux": bool(r.u8()),
        "use_game_aux": bool(r.u8()),
        "override_user_aux": bool(r.u8()),
    }
    if r.u8():
        aux["user_aux_buses"] = [r.u32() for _ in range(4)]
    return aux


def read_adv_settings(r: Reader):
    return {
        "virtual_queue": VIRTUAL_QUEUE_NAMES.get(r.u8()),
        "kill_newest": bool(r.u8()),
        "use_virtual_behavior": bool(r.u8()),
        "max_instances": r.u16(),
        "global_limit": bool(r.u8()),
        "below_threshold": BELOW_THRESHOLD_NAMES.get(r.u8()),
        "max_instances_override_parent": bool(r.u8()),
        "virtual_voice_override_parent": bool(r.u8()),
        "override_hdr_envelope": bool(r.u8()),
        "override_analysis": bool(r.u8()),
        "normalize_loudness": bool(r.u8()),
        "enable_envelope": bool(r.u8()),
    }


def read_state_chunk(r: Reader):
    groups = []
    for _ in range(r.u32()):
        group_id = r.u32()
        sync = r.u8()
        states = [{"state_id": r.u32(), "state_instance_id": r.u32()} for _ in range(r.u16())]
        groups.append({"state_group_id": group_id, "sync": SYNC_TYPE_NAMES.get(sync, sync), "states": states})
    return groups


def read_node_base(r: Reader, feedback: bool):
    override_fx = r.u8()
    fx = read_fx_chunks(r)
    fx["override_parent"] = bool(override_fx)
    node = {"fx": fx}
    node["override_bus_id"] = r.u32()
    node["parent_id"] = r.u32()
    node["priority_override_parent"] = bool(r.u8())
    node["priority_apply_distance_factor"] = bool(r.u8())
    node["props"] = read_prop_bundle(r)
    node["ranged_props"] = read_ranged_prop_bundle(r)
    node["positioning"] = read_positioning(r)
    node["aux"] = read_aux_params(r)
    node["advanced"] = read_adv_settings(r)
    node["states"] = read_state_chunk(r)
    node["rtpcs"] = read_rtpc_list(r)
    if feedback:
        node["feedback_bus_id"] = r.u32()
    return node


def read_children(r: Reader):
    return [r.u32() for _ in range(r.u32())]


def read_except_list(r: Reader):
    return [{"id": r.u32(), "is_bus": bool(r.u8())} for _ in range(r.u32())]


def parse_state(r, obj, feedback):
    count = r.u8()
    ids = [r.u8() for _ in range(count)]
    obj["props"] = {RTPC_PARAMETER_NAMES.get(i, "Param_0x%02X" % i): r.f32() for i in ids}


def parse_sound(r, obj, feedback):
    obj["source"] = read_source(r)
    obj["node"] = read_node_base(r, feedback)


def parse_action(r, obj, feedback):
    action_type = r.u16()
    obj["action_type"] = action_type
    obj["action"] = ACTION_TYPE_NAMES.get(action_type, "0x%04X" % action_type)
    obj["target_id"] = r.u32()
    obj["target_is_bus"] = bool(r.u8() & 1)
    obj["props"] = read_prop_bundle(r)
    obj["ranged_props"] = read_ranged_prop_bundle(r)
    kind = ACTION_CLASS_BY_CATEGORY.get(action_type >> 8)
    if kind is None:
        raise BankError("unknown action type 0x%04X" % action_type)
    if kind in ("Stop", "Pause", "Resume"):
        obj["fade_curve"] = CURVE_INTERPOLATION_NAMES.get(r.u8() & 0x1F)
        if kind == "Pause":
            bits = r.u8()
            obj["include_pending_resume"] = bool(bits & 1)
        elif kind == "Resume":
            bits = r.u8()
            obj["master_resume"] = bool(bits & 1)
        obj["exceptions"] = read_except_list(r)
    elif kind == "Play":
        obj["fade_curve"] = CURVE_INTERPOLATION_NAMES.get(r.u8() & 0x1F)
        obj["bank_id"] = r.u32()
    elif kind == "SetValue":
        obj["fade_curve"] = CURVE_INTERPOLATION_NAMES.get(r.u8() & 0x1F)
        obj["exceptions"] = read_except_list(r)
    elif kind in ("SetAkProp", "SetGameParameter"):
        obj["fade_curve"] = CURVE_INTERPOLATION_NAMES.get(r.u8() & 0x1F)
        meaning = r.u8()
        obj["value_meaning"] = VALUE_MEANING_NAMES.get(meaning, meaning)
        obj["value"] = [r.f32(), r.f32(), r.f32()]
        obj["exceptions"] = read_except_list(r)
    elif kind == "SetState":
        obj["state_group_id"] = r.u32()
        obj["state_id"] = r.u32()
    elif kind == "SetSwitch":
        obj["switch_group_id"] = r.u32()
        obj["switch_id"] = r.u32()
    elif kind == "BypassFX":
        obj["bypass"] = bool(r.u8())
        obj["target_mask"] = r.u8()
        obj["exceptions"] = read_except_list(r)
    elif kind == "Seek":
        obj["seek_relative_to_duration"] = bool(r.u8())
        obj["seek_value"] = [r.f32(), r.f32(), r.f32()]
        obj["snap_to_marker"] = bool(r.u8())
        obj["exceptions"] = read_except_list(r)


def parse_event(r, obj, feedback):
    obj["action_ids"] = [r.u32() for _ in range(r.u32())]


def parse_ranseq(r, obj, feedback):
    obj["node"] = read_node_base(r, feedback)
    obj["loop_count"] = r.u16()
    obj["loop_mod"] = [r.u16(), r.u16()]
    obj["transition_time"] = [r.f32(), r.f32(), r.f32()]
    obj["avoid_repeat_count"] = r.u16()
    obj["transition_mode"] = TRANSITION_MODE_NAMES.get(r.u8())
    obj["random_mode"] = RANDOM_MODE_NAMES.get(r.u8())
    obj["mode"] = CONTAINER_MODE_NAMES.get(r.u8())
    obj["using_weight"] = bool(r.u8())
    obj["reset_playlist_each_play"] = bool(r.u8())
    obj["restart_backward"] = bool(r.u8())
    obj["continuous"] = bool(r.u8())
    obj["global"] = bool(r.u8())
    obj["children"] = read_children(r)
    obj["playlist"] = [{"id": r.u32(), "weight": r.s32()} for _ in range(r.u16())]


def parse_switch(r, obj, feedback):
    obj["node"] = read_node_base(r, feedback)
    obj["group_type"] = GROUP_TYPE_NAMES.get(r.u32())
    obj["group_id"] = r.u32()
    obj["default_switch"] = r.u32()
    obj["continuous_validation"] = bool(r.u8())
    obj["children"] = read_children(r)
    obj["switches"] = []
    for _ in range(r.u32()):
        switch_id = r.u32()
        obj["switches"].append({"switch_id": switch_id, "nodes": [r.u32() for _ in range(r.u32())]})
    obj["switch_params"] = []
    for _ in range(r.u32()):
        obj["switch_params"].append({
            "node_id": r.u32(), "first_only": bool(r.u8()), "continue_playback": bool(r.u8()),
            "on_switch_mode": {0: "PlayToEnd", 1: "Stop"}.get(r.u32()), "fade_out": r.s32(), "fade_in": r.s32(),
        })


def parse_actor_mixer(r, obj, feedback):
    obj["node"] = read_node_base(r, feedback)
    obj["children"] = read_children(r)


def parse_bus(r, obj, feedback):
    obj["parent_bus_id"] = r.u32()
    obj["props"] = read_prop_bundle(r)
    obj["positioning_enabled"] = bool(r.u8())
    obj["positioning_enable_panner"] = bool(r.u8())
    obj["kill_newest"] = bool(r.u8())
    obj["use_virtual_behavior"] = bool(r.u8())
    obj["max_instances"] = r.u16()
    obj["max_instances_override_parent"] = bool(r.u8())
    obj["channel_config"] = r.u16()
    r.u8()
    r.u8()
    obj["hdr_bus"] = bool(r.u8())
    obj["hdr_release_exponential"] = bool(r.u8())
    obj["recovery_time"] = r.s32()
    obj["max_duck_volume"] = r.f32()
    obj["ducks"] = []
    for _ in range(r.u32()):
        obj["ducks"].append({
            "bus_id": r.u32(), "volume": r.f32(), "fade_out": r.s32(), "fade_in": r.s32(),
            "curve": CURVE_INTERPOLATION_NAMES.get(r.u8()), "target_prop": PROP_NAMES.get(r.u8()),
        })
    obj["fx"] = read_fx_chunks(r)
    obj["rtpcs"] = read_rtpc_list(r)
    obj["states"] = read_state_chunk(r)
    if feedback:
        obj["feedback_bus_id"] = r.u32()


def parse_layer(r, obj, feedback):
    obj["node"] = read_node_base(r, feedback)
    obj["children"] = read_children(r)
    obj["layers"] = []
    for _ in range(r.u32()):
        layer = {"layer_id": r.u32(), "rtpcs": read_rtpc_list(r), "crossfade_rtpc_id": r.u32(), "associations": []}
        for _ in range(r.u32()):
            child_id = r.u32()
            layer["associations"].append({"child_id": child_id, "curve": read_graph_points(r, r.u32())})
        obj["layers"].append(layer)


def read_music_node(r: Reader, obj, feedback):
    obj["node"] = read_node_base(r, feedback)
    obj["children"] = read_children(r)
    obj["meter"] = {"grid_period": r.f64(), "grid_offset": r.f64(), "tempo": r.f32(), "beats_per_bar": r.u8(),
                    "beat_value": r.u8()}
    obj["meter_override"] = bool(r.u8())
    obj["stingers"] = []
    for _ in range(r.u32()):
        obj["stingers"].append({"trigger_id": r.u32(), "segment_id": r.u32(), "sync": SYNC_TYPE_NAMES.get(r.u32()),
                                "cue_filter": r.u32(), "dont_repeat_time": r.s32(), "lookahead": r.u32()})


def parse_music_segment(r, obj, feedback):
    read_music_node(r, obj, feedback)
    obj["duration_ms"] = r.f64()
    obj["markers"] = []
    for _ in range(r.u32()):
        marker = {"id": r.u32(), "position_ms": r.f64()}
        name_size = r.u32()
        if name_size:
            marker["name"] = r.raw(name_size).decode("utf-8", "replace")
        obj["markers"].append(marker)


def parse_music_track(r, obj, feedback):
    obj["sources"] = [read_source(r) for _ in range(r.u32())]
    obj["playlist"] = []
    count = r.u32()
    if count:
        for _ in range(count):
            obj["playlist"].append({"track": r.u32(), "source_id": r.u32(), "play_at_ms": r.f64(),
                                    "begin_trim_ms": r.f64(), "end_trim_ms": r.f64(), "source_duration_ms": r.f64()})
        obj["sub_tracks"] = r.u32()
    obj["clip_automation"] = []
    for _ in range(r.u32()):
        clip = r.u32()
        kind = {0: "Volume", 1: "LPF", 2: "FadeIn", 3: "FadeOut"}.get(r.u32())
        obj["clip_automation"].append({"clip": clip, "type": kind, "points": read_graph_points(r, r.u32())})
    obj["node"] = read_node_base(r, feedback)
    obj["track_type"] = MUSIC_TRACK_TYPE_NAMES.get(r.u32())
    obj["lookahead_ms"] = r.s32()


def read_music_fade(r: Reader):
    return {"time": r.s32(), "curve": CURVE_INTERPOLATION_NAMES.get(r.u32()), "offset": r.s32()}


def read_music_trans_node(r: Reader, obj, feedback):
    read_music_node(r, obj, feedback)
    obj["transition_rules"] = []
    for _ in range(r.u32()):
        rule = {"sources": [r.u32() for _ in range(r.u32())]}
        rule["destinations"] = [r.u32() for _ in range(r.u32())]
        rule["source_rule"] = {"fade": read_music_fade(r), "sync": SYNC_TYPE_NAMES.get(r.u32()),
                               "cue_filter": r.u32(), "play_post_exit": bool(r.u8())}
        rule["destination_rule"] = {"fade": read_music_fade(r), "cue_filter": r.u32(), "jump_to_id": r.u32(),
                                    "entry_type": r.u16(), "play_pre_entry": bool(r.u8()),
                                    "match_source_cue_name": bool(r.u8())}
        if r.u8():
            rule["transition_object"] = {"segment_id": r.u32(), "fade_in": read_music_fade(r),
                                         "fade_out": read_music_fade(r), "play_pre_entry": bool(r.u8()),
                                         "play_post_exit": bool(r.u8())}
        obj["transition_rules"].append(rule)


def read_music_playlist_node(r: Reader):
    node = {"segment_id": r.u32(), "item_id": r.u32()}
    child_count = r.u32()
    node["type"] = MUSIC_PLAYLIST_TYPE_NAMES.get(r.u32())
    node["loop"] = struct.unpack("<h", struct.pack("<H", r.u16()))[0]
    node["weight"] = r.u32()
    node["avoid_repeat_count"] = r.u16()
    node["using_weight"] = bool(r.u8())
    node["shuffle"] = bool(r.u8())
    node["children"] = [read_music_playlist_node(r) for _ in range(child_count)]
    return node


def parse_music_ranseq(r, obj, feedback):
    read_music_trans_node(r, obj, feedback)
    obj["playlist_item_count"] = r.u32()
    obj["playlist"] = read_music_playlist_node(r)


def parse_attenuation(r, obj, feedback):
    cone = r.u8()
    obj["cone_enabled"] = bool(cone & 1)
    if cone & 1:
        obj["cone"] = {"inside_degrees": r.f32(), "outside_degrees": r.f32(), "outside_volume": r.f32(),
                       "low_pass": r.f32()}
    curve_to_use = [r.s8() for _ in ATTENUATION_CURVE_SLOTS]
    curves = []
    for _ in range(r.u8()):
        scaling = r.u8()
        curves.append({"scaling": CURVE_SCALING_NAMES.get(scaling, scaling), "points": read_graph_points(r, r.u16())})
    obj["curves"] = {slot: (curves[index] if 0 <= index < len(curves) else None)
                     for slot, index in zip(ATTENUATION_CURVE_SLOTS, curve_to_use)}
    obj["curve_to_use"] = curve_to_use
    obj["rtpcs"] = read_rtpc_list(r)
    dry = obj["curves"].get("VolumeDry")
    if dry and dry["points"]:
        obj["max_distance"] = dry["points"][-1][0]


def read_decision_tree(r: Reader, size: int, depth: int):
    raw = r.raw(size)
    node_count = size // 12
    nodes = [struct.unpack_from("<IIHH", raw, i * 12) for i in range(node_count)]

    def build(index, level):
        key, payload, weight, probability = nodes[index]
        node = {"key": key, "weight": weight, "probability": probability}
        if level == depth:
            node["audio_node_id"] = payload
        else:
            first, count = payload & 0xFFFF, payload >> 16
            node["children"] = [build(first + i, level + 1) for i in range(count)]
        return node

    return build(0, 0) if nodes else None


def parse_dialogue_event(r, obj, feedback):
    obj["probability"] = r.u8()
    depth = r.u32()
    groups = [r.u32() for _ in range(depth)]
    types = [GROUP_TYPE_NAMES.get(r.u8()) for _ in range(depth)]
    obj["arguments"] = [{"group_id": g, "group_type": t} for g, t in zip(groups, types)]
    size = r.u32()
    obj["mode"] = {0: "BestMatch", 1: "Weighted"}.get(r.u8())
    obj["tree"] = read_decision_tree(r, size, depth)


def parse_feedback_node(r, obj, feedback):
    obj["sources"] = []
    for _ in range(r.u32()):
        company, device, volume_offset = r.u16(), r.u16(), r.f32()
        source = read_source(r)
        source.update({"company_id": company, "device_id": device, "volume_offset": volume_offset})
        obj["sources"].append(source)
    obj["node"] = read_node_base(r, feedback)


def parse_fx(r, obj, feedback):
    plugin_id = r.u32()
    obj["plugin_id"] = plugin_id
    obj["plugin"] = plugin_name(plugin_id)
    size = r.u32()
    obj["params"] = read_plugin_params(r, plugin_id, size) if size else {}
    obj["media"] = [{"index": r.u8(), "source_id": r.u32()} for _ in range(r.u8())]
    obj["rtpcs"] = read_rtpc_list(r)


HIRC_PARSERS = {
    0x01: parse_state, 0x02: parse_sound, 0x03: parse_action, 0x04: parse_event, 0x05: parse_ranseq,
    0x06: parse_switch, 0x07: parse_actor_mixer, 0x08: parse_bus, 0x09: parse_layer, 0x0A: parse_music_segment,
    0x0B: parse_music_track, 0x0D: parse_music_ranseq, 0x0E: parse_attenuation, 0x0F: parse_dialogue_event,
    0x10: parse_bus, 0x11: parse_feedback_node, 0x12: parse_fx, 0x13: parse_fx, 0x14: parse_bus,
}


def parse_hirc(data: bytes, start: int, end: int, feedback: bool):
    r = Reader(data, start, end)
    objects = []
    for _ in range(r.u32()):
        hirc_type = r.u8()
        size = r.u32()
        body_start = r.pos
        body = Reader(data, body_start, body_start + size)
        obj = {"type": HIRC_TYPE_NAMES.get(hirc_type, "Type_0x%02X" % hirc_type), "id": body.u32()}
        parser = HIRC_PARSERS.get(hirc_type)
        if parser is None:
            obj["raw"] = body.raw(body.remaining()).hex()
        else:
            parser(body, obj, feedback)
            if body.remaining():
                raise BankError("%s %d: %d bytes left unparsed" % (obj["type"], obj["id"], body.remaining()))
        objects.append(obj)
        r.pos = body_start + size
    return objects


def parse_global_settings(data: bytes, start: int, end: int):
    r = Reader(data, start, end)
    settings = {"volume_threshold": r.f32(), "max_voices": r.u16(), "state_groups": [], "switch_groups": [],
                "game_parameters": []}
    for _ in range(r.u32()):
        group = {"id": r.u32(), "default_transition_time": r.u32()}
        group["transitions"] = [{"from": r.u32(), "to": r.u32(), "time": r.u32()} for _ in range(r.u32())]
        settings["state_groups"].append(group)
    for _ in range(r.u32()):
        group_id, rtpc_id = r.u32(), r.u32()
        settings["switch_groups"].append({"id": group_id, "rtpc_id": rtpc_id, "points": read_graph_points(r, r.u32())})
    for _ in range(r.u32()):
        settings["game_parameters"].append({"id": r.u32(), "default": r.f32()})
    return settings


def parse_env_settings(data: bytes, start: int, end: int):
    r = Reader(data, start, end)
    curves = {}
    for x_name in ("Obstruction", "Occlusion"):
        for y_name in ("Volume", "LPF"):
            enabled = bool(r.u8())
            scaling = r.u8()
            curves["%s%s" % (x_name, y_name)] = {"enabled": enabled,
                                                  "scaling": CURVE_SCALING_NAMES.get(scaling, scaling),
                                                  "points": read_graph_points(r, r.u16())}
    return curves


def parse_string_mappings(data: bytes, start: int, end: int):
    r = Reader(data, start, end)
    r.u32()
    names = {}
    for _ in range(r.u32()):
        bank_id = r.u32()
        names[bank_id] = r.raw(r.u8()).decode("utf-8", "replace")
    return names


class Bank:
    def __init__(self, data: bytes, name: str = ""):
        self.data = data
        self.name = name
        self.chunks = {}
        pos = 0
        while pos + 8 <= len(data):
            tag, size = struct.unpack_from("<4sI", data, pos)
            self.chunks[tag.decode("ascii", "replace")] = (pos + 8, size)
            pos += 8 + size
        if "BKHD" not in self.chunks:
            raise BankError("no BKHD chunk")
        header_start, header_size = self.chunks["BKHD"]
        version, bank_id, language_id, feedback, project_id = struct.unpack_from("<IIIII", data, header_start)
        if version != SUPPORTED_VERSION:
            raise BankError("bank version %d is not supported (only %d)" % (version, SUPPORTED_VERSION))
        self.version = version
        self.bank_id = bank_id
        self.language_id = language_id
        self.feedback = bool(feedback & 1)
        self.project_id = project_id
        self.media = {}
        if "DIDX" in self.chunks and "DATA" in self.chunks:
            index_start, index_size = self.chunks["DIDX"]
            data_start, data_size = self.chunks["DATA"]
            for i in range(index_size // 12):
                media_id, offset, size = struct.unpack_from("<III", data, index_start + i * 12)
                if offset + size > data_size:
                    raise BankError("media %d runs past DATA" % media_id)
                self.media[media_id] = (data_start + offset, size)
        self.objects = []
        if "HIRC" in self.chunks:
            start, size = self.chunks["HIRC"]
            self.objects = parse_hirc(data, start, start + size, self.feedback)
        self.global_settings = None
        if "STMG" in self.chunks:
            start, size = self.chunks["STMG"]
            self.global_settings = parse_global_settings(data, start, start + size)
        self.env_settings = None
        if "ENVS" in self.chunks:
            start, size = self.chunks["ENVS"]
            self.env_settings = parse_env_settings(data, start, start + size)
        self.bank_names = {}
        if "STID" in self.chunks:
            start, size = self.chunks["STID"]
            self.bank_names = parse_string_mappings(data, start, start + size)

    def media_bytes(self, media_id: int) -> bytes:
        offset, size = self.media[media_id]
        return self.data[offset:offset + size]

    def to_json(self):
        return {
            "name": self.name,
            "version": self.version,
            "bank_id": self.bank_id,
            "language_id": self.language_id,
            "feedback": self.feedback,
            "project_id": self.project_id,
            "chunks": {tag: {"offset": start - 8, "size": size} for tag, (start, size) in self.chunks.items()},
            "media": {str(k): {"offset": v[0], "size": v[1]} for k, v in self.media.items()},
            "bank_names": {str(k): v for k, v in self.bank_names.items()},
            "global_settings": self.global_settings,
            "env_settings": self.env_settings,
            "objects": self.objects,
        }


def load_bank(path: Path) -> Bank:
    data = path.read_bytes()
    # a Fox sound bank package (.sbp, docs/formats/audio.md) wraps one bank: take it out so the game's own files work here
    if data[:4] == b"SBPL":
        from sbp import read_sbp
        banks = [e for e in read_sbp(data) if e["kind"] == "bnk"]
        if not banks:
            raise BankError("%s holds no bnk entry" % path.name)
        data = data[banks[0]["offset"]:banks[0]["offset"] + banks[0]["size"]]
    return Bank(data, path.stem)


WEM_CODEC_TAGS = {0xFFFF: "Wwise Vorbis", 0xFFFE: "PCM", 0x0001: "PCM", 0x0002: "Wwise IMA ADPCM", 0x0165: "XMA",
                  0x0166: "XMA2", 0xFFF0: "DSP", 0xA106: "AAC", 0x3039: "Opus"}


def riff_chunks(blob: bytes):
    pos = 12
    while pos + 8 <= len(blob):
        tag, size = struct.unpack_from("<4sI", blob, pos)
        yield tag, pos + 8, size
        pos += 8 + size + (size & 1)


def wem_format(blob: bytes):
    if blob[:4] != b"RIFF":
        return None
    for tag, start, size in riff_chunks(blob):
        if tag == b"fmt ":
            codec, channels, rate = struct.unpack_from("<HHI", blob, start)
            return {"codec_tag": codec, "codec_tag_name": WEM_CODEC_TAGS.get(codec, "0x%04X" % codec),
                    "channels": channels, "sample_rate": rate}
    return None


def wem_markers(blob: bytes):
    positions = {}
    labels = {}
    for tag, start, size in riff_chunks(blob):
        if tag == b"cue ":
            for index in range(struct.unpack_from("<I", blob, start)[0]):
                cue_id, _, _, _, _, sample = struct.unpack_from("<II4sIII", blob, start + 4 + index * 24)
                positions[cue_id] = sample
        elif tag == b"LIST" and blob[start:start + 4] == b"adtl":
            sub = start + 4
            while sub + 8 <= start + size:
                sub_tag, sub_size = struct.unpack_from("<4sI", blob, sub)
                if sub_tag == b"labl":
                    cue_id = struct.unpack_from("<I", blob, sub + 8)[0]
                    labels[cue_id] = blob[sub + 12:sub + 8 + sub_size].split(b"\0")[0].decode("latin-1")
                sub += 8 + sub_size + (sub_size & 1)
    return [{"cue_id": cue_id, "sample": positions.get(cue_id), "label": labels.get(cue_id)}
            for cue_id in sorted(set(positions) | set(labels))]


def cmd_info(args):
    for path in args.banks:
        bank = load_bank(path)
        print("%s: version %d, id %08X, language %d, feedback %d, project %d" % (
            path.name, bank.version, bank.bank_id, bank.language_id, bank.feedback, bank.project_id))
        print("  chunks: %s" % ", ".join("%s(%d)" % (t, s) for t, (_, s) in bank.chunks.items()))
        print("  media: %d" % len(bank.media))
        counts = Counter(obj["type"] for obj in bank.objects)
        print("  hirc: %d objects: %s" % (len(bank.objects), ", ".join("%s %d" % kv for kv in sorted(counts.items()))))


def cmd_json(args):
    bank = load_bank(args.bank)
    text = json.dumps(bank.to_json(), indent=1)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding="utf-8")
    else:
        print(text)


def cmd_extract(args):
    for path in args.banks:
        bank = load_bank(path)
        target = args.out / path.stem
        target.mkdir(parents=True, exist_ok=True)
        for media_id in bank.media:
            (target / ("%d.wem" % media_id)).write_bytes(bank.media_bytes(media_id))
        print("%s: %d media written to %s" % (path.name, len(bank.media), target))


def main():
    ap = argparse.ArgumentParser(description="Wwise sound bank (.bnk) parser for bank version 88 (Wwise 2013).")
    sub = ap.add_subparsers(dest="command", required=True)
    p = sub.add_parser("info", help="print header, chunks and HIRC object counts")
    p.add_argument("banks", type=Path, nargs="+")
    p.set_defaults(func=cmd_info)
    p = sub.add_parser("json", help="dump the parsed bank as JSON")
    p.add_argument("bank", type=Path)
    p.add_argument("--out", type=Path)
    p.set_defaults(func=cmd_json)
    p = sub.add_parser("extract", help="write embedded media as <id>.wem into OUT/<bank>/")
    p.add_argument("banks", type=Path, nargs="+")
    p.add_argument("--out", type=Path, required=True)
    p.set_defaults(func=cmd_extract)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
