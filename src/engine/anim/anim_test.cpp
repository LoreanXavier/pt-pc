#include "engine/anim/anim_test.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "engine/anim/demo_file.h"
#include "engine/anim/gani.h"
#include "engine/anim/help_bones.h"
#include "engine/anim/rig.h"
#include "engine/anim/skeleton.h"
#include "engine/assets/fmdl.h"
#include "engine/core/log.h"
#include "engine/core/pathcode.h"
#include "engine/core/strcode.h"
#include "engine/fs/vfs.h"

namespace pt::anim {
namespace {

class JsonDoc {
public:
    enum Type : uint8_t { Null, Bool, Number, String, Array, Object };
    struct Node {
        Type type = Null;
        uint32_t count = 0;
        uint64_t data = 0;
    };

    bool Parse(std::string text) {
        text_ = std::move(text);
        pos_ = 0;
        nodes_.clear();
        Node root;
        if (!Value(root)) {
            return false;
        }
        nodes_.push_back(root);
        return true;
    }

    uint32_t Root() const { return static_cast<uint32_t>(nodes_.size() - 1); }
    const Node& At(uint32_t i) const { return nodes_[i]; }
    std::string_view Str(const Node& n) const {
        return std::string_view(strings_).substr(static_cast<size_t>(n.data >> 32), static_cast<size_t>(n.data & 0xFFFFFFFF));
    }
    double Num(const Node& n) const {
        double d;
        std::memcpy(&d, &n.data, 8);
        return d;
    }

private:
    void Skip() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' || text_[pos_] == '\t')) {
            ++pos_;
        }
    }

    bool ParseString(Node& out) {
        ++pos_;
        const size_t start = strings_.size();
        while (pos_ < text_.size() && text_[pos_] != '"') {
            char c = text_[pos_++];
            if (c == '\\' && pos_ < text_.size()) {
                const char e = text_[pos_++];
                switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u': {
                    unsigned value = 0;
                    std::from_chars(text_.data() + pos_, text_.data() + std::min(pos_ + 4, text_.size()), value, 16);
                    pos_ += 4;
                    c = value < 0x80 ? static_cast<char>(value) : '?';
                    break;
                }
                default: c = e; break;
                }
            }
            strings_.push_back(c);
        }
        ++pos_;
        out.type = String;
        out.data = (static_cast<uint64_t>(start) << 32) | (strings_.size() - start);
        return true;
    }

    bool Value(Node& out) {
        Skip();
        if (pos_ >= text_.size()) {
            return false;
        }
        const char c = text_[pos_];
        if (c == '"') {
            return ParseString(out);
        }
        if (c == '{' || c == '[') {
            const bool object = c == '{';
            ++pos_;
            std::vector<Node> children;
            Skip();
            if (pos_ < text_.size() && text_[pos_] == (object ? '}' : ']')) {
                ++pos_;
            } else {
                while (true) {
                    Node child;
                    if (object) {
                        Skip();
                        if (pos_ >= text_.size() || text_[pos_] != '"' || !ParseString(child)) {
                            return false;
                        }
                        children.push_back(child);
                        Skip();
                        if (pos_ >= text_.size() || text_[pos_] != ':') {
                            return false;
                        }
                        ++pos_;
                    }
                    if (!Value(child)) {
                        return false;
                    }
                    children.push_back(child);
                    Skip();
                    if (pos_ < text_.size() && text_[pos_] == ',') {
                        ++pos_;
                        continue;
                    }
                    if (pos_ < text_.size() && text_[pos_] == (object ? '}' : ']')) {
                        ++pos_;
                        break;
                    }
                    return false;
                }
            }
            out.type = object ? Object : Array;
            out.count = static_cast<uint32_t>(children.size());
            out.data = nodes_.size();
            nodes_.insert(nodes_.end(), children.begin(), children.end());
            return true;
        }
        if (text_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            out.type = Null;
            return true;
        }
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            out.type = Bool;
            out.data = 1;
            return true;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            out.type = Bool;
            out.data = 0;
            return true;
        }
        double d = 0.0;
        const auto result = std::from_chars(text_.data() + pos_, text_.data() + text_.size(), d);
        if (result.ec != std::errc()) {
            return false;
        }
        pos_ = static_cast<size_t>(result.ptr - text_.data());
        out.type = Number;
        std::memcpy(&out.data, &d, 8);
        return true;
    }

    std::string text_;
    size_t pos_ = 0;
    std::string strings_;
    std::vector<Node> nodes_;
};

struct J {
    const JsonDoc* doc = nullptr;
    uint32_t index = 0;
    bool valid = false;

    const JsonDoc::Node& N() const { return doc->At(index); }
    bool Null() const { return !valid || N().type == JsonDoc::Null; }
    double Num() const { return valid && N().type == JsonDoc::Number ? doc->Num(N()) : 0.0; }
    bool Bool() const { return valid && N().type == JsonDoc::Bool && N().data != 0; }
    std::string_view Str() const { return valid && N().type == JsonDoc::String ? doc->Str(N()) : std::string_view(); }
    size_t Size() const {
        if (!valid) {
            return 0;
        }
        return N().type == JsonDoc::Array ? N().count : N().type == JsonDoc::Object ? N().count / 2 : 0;
    }
    J operator[](size_t i) const {
        if (!valid || N().type != JsonDoc::Array || i >= N().count) {
            return {};
        }
        return {doc, static_cast<uint32_t>(N().data + i), true};
    }
    J Key(size_t i) const { return {doc, static_cast<uint32_t>(N().data + 2 * i), true}; }
    J ValueAt(size_t i) const { return {doc, static_cast<uint32_t>(N().data + 2 * i + 1), true}; }
    J Get(std::string_view key) const {
        if (!valid || N().type != JsonDoc::Object) {
            return {};
        }
        for (uint32_t i = 0; i + 1 < N().count; i += 2) {
            if (doc->Str(doc->At(static_cast<uint32_t>(N().data + i))) == key) {
                return {doc, static_cast<uint32_t>(N().data + i + 1), true};
            }
        }
        return {};
    }
};

bool LoadJson(const std::filesystem::path& path, JsonDoc& doc) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return doc.Parse(std::move(text));
}

uint64_t NameHash64(std::string_view name) {
    if (name.starts_with('#')) {
        uint64_t v = 0;
        std::from_chars(name.data() + 1, name.data() + name.size(), v, 16);
        return v;
    }
    return StrCode64(name) & kStrCode64Mask;
}

uint32_t NameHash32(std::string_view name) {
    if (name.starts_with('#')) {
        uint32_t v = 0;
        std::from_chars(name.data() + 1, name.data() + name.size(), v, 16);
        return v;
    }
    return StrCode32(name);
}

struct Stats {
    uint64_t compared = 0;
    uint64_t failed = 0;
    double max_error = 0.0;
    std::string first_failure;

    void Value(double mine, double ref, double tolerance, const std::string& what) {
        ++compared;
        const double e = std::abs(mine - ref);
        max_error = std::max(max_error, e);
        if (e > tolerance + std::abs(ref) * 2.4e-7) {
            Fail(std::format("{} mine {:.6f} ref {:.6f}", what, mine, ref));
        }
    }
    void Fail(const std::string& what) {
        ++failed;
        if (first_failure.empty()) {
            first_failure = what;
        }
    }
    void Check(bool ok, const std::string& what) {
        ++compared;
        if (!ok) {
            Fail(what);
        }
    }
};

void CompareVector(Stats& s, const glm::vec4& mine, bool mine_valid, J ref, int components, double tolerance, const std::string& what) {
    if (ref.Null()) {
        s.Check(!mine_valid, what + " expected null");
        return;
    }
    if (!mine_valid) {
        s.Fail(what + " missing sample");
        return;
    }
    for (int c = 0; c < components && c < static_cast<int>(ref.Size()); ++c) {
        s.Value(mine[c], ref[static_cast<size_t>(c)].Num(), tolerance, std::format("{}[{}]", what, c));
    }
}

std::string ActorKindName(ActorKind kind) {
    switch (kind) {
    case ActorKind::Camera: return "camera";
    case ActorKind::CameraParam: return "cameraParam";
    case ActorKind::SiFrame: return "siFrame";
    case ActorKind::Locator: return "locator";
    case ActorKind::ModelRoot: return "modelRoot";
    case ActorKind::Skeleton: return "skeleton";
    case ActorKind::MotionPoints: return "motionPoints";
    case ActorKind::Timeline: return "timeline";
    default: return "other";
    }
}

struct DemoCheck {
    Stats header;
    Stats keys;
    Stats samples;
    Stats events;
};

void CompareEvents(const DemoStreamFile& file, J events, Stats& s) {
    const std::vector<const StreamEvent*> mine = file.FirstLoopEvents();
    s.Check(mine.size() == events.Size(), std::format("event count mine {} ref {}", mine.size(), events.Size()));
    for (size_t i = 0; i < std::min(mine.size(), events.Size()); ++i) {
        const StreamEvent& e = *mine[i];
        J ref = events[i];
        const std::string where = std::format("event {} ({})", i, ref.Get("type").Str());
        const std::string_view type = ref.Get("type").Str();
        const std::string_view my_type = EventTypeName(e.type);
        s.Check(my_type.empty() ? NameHash32(type) == e.type : my_type == type, where + " type");
        J sections = ref.Get("sections");
        s.Check(sections.Size() == e.sections.size(), where + " section count");
        for (size_t k = 0; k < std::min<size_t>(sections.Size(), e.sections.size()); ++k) {
            s.Check(static_cast<int32_t>(sections[k].Get("start").Num()) == e.sections[k].start, where + " section start");
            s.Check(static_cast<int32_t>(sections[k].Get("end").Num()) == e.sections[k].end, where + " section end");
            s.Check(sections[k].Get("startOutsideBlock").Bool() == e.sections[k].start_outside, where + " start flag");
            s.Check(sections[k].Get("endOutsideBlock").Bool() == e.sections[k].end_outside, where + " end flag");
        }
        if (e.type != kEventExecCommand) {
            J ints = ref.Get("ints");
            J floats = ref.Get("floats");
            J strings = ref.Get("strings");
            s.Check(ints.Size() == e.ints.size() && floats.Size() == e.floats.size() && strings.Size() == e.strings.size(), where + " array sizes");
            for (size_t k = 0; k < std::min<size_t>(ints.Size(), e.ints.size()); ++k) {
                s.Check(static_cast<uint32_t>(ints[k].Num()) == static_cast<uint32_t>(e.ints[k]), where + " int");
            }
            for (size_t k = 0; k < std::min<size_t>(floats.Size(), e.floats.size()); ++k) {
                s.Value(e.floats[k], floats[k].Num(), 1e-5, where + " float");
            }
            for (size_t k = 0; k < std::min<size_t>(strings.Size(), e.strings.size()); ++k) {
                s.Check(NameHash64(strings[k].Str()) == e.strings[k], where + std::format(" string {}", strings[k].Str()));
            }
            continue;
        }
        J functor = ref.Get("functor");
        if (!functor.Null()) {
            s.Check(NameHash64(functor.Str()) == e.functor, where + std::format(" functor {}", functor.Str()));
        }
        J length = ref.Get("length");
        s.Check(length.Null() ? e.length == -1 : static_cast<int32_t>(length.Num()) == e.length, where + " length");
        J interpolated = ref.Get("interpolated");
        s.Check(interpolated.Null() ? e.interpolated == 0 : static_cast<uint32_t>(interpolated.Get("count").Num()) == e.interpolated,
                where + " interp");
        J raw = ref.Get("rawInts");
        s.Check(raw.Size() == e.raw_int_count, where + " raw int count");
        J params = ref.Get("params");
        s.Check(params.Size() == e.params.size(), where + std::format(" param count mine {} ref {}", e.params.size(), params.Size()));
        for (size_t k = 0; k < params.Size(); ++k) {
            const std::string_view key = params.Key(k).Str();
            const uint32_t key_hash = NameHash32(key);
            const ExecParam* p = e.Param(key_hash);
            if (!p) {
                s.Fail(where + std::format(" param {} missing", key));
                continue;
            }
            J value = params.ValueAt(k).Get("value");
            const std::string_view type = params.ValueAt(k).Get("type").Str();
            if (type == "string" || type == "file") {
                s.Check(!value.Null() && NameHash64(value.Str()) == e.String(key_hash, ~0ull), where + std::format(" param {} string", key));
            } else if (type == "int" || type == "bool") {
                s.Check(!value.Null() && static_cast<uint32_t>(value.Num()) == static_cast<uint32_t>(e.Int(key_hash, -12345)),
                        where + std::format(" param {} int", key));
            } else if (value.valid && value.N().type == JsonDoc::Number) {
                s.Value(e.Float(key_hash, -12345.0f), value.Num(), 1e-5, where + std::format(" param {}", key));
            } else if (value.valid && value.N().type == JsonDoc::Array) {
                const size_t span = value.Size();
                for (size_t c = 0; c < span; ++c) {
                    const size_t idx = p->index + c;
                    s.Value(idx < e.floats.size() ? e.floats[idx] : -12345.0, value[c].Num(), 1e-5, where + std::format(" param {}[{}]", key, c));
                }
            }
        }
    }
}

DemoCheck CheckDemo(const DemoStreamFile& file, JsonDoc& doc) {
    DemoCheck check;
    J root{&doc, doc.Root(), true};
    const uint32_t length = static_cast<uint32_t>(root.Get("length").Num());
    const uint32_t frames = static_cast<uint32_t>(root.Get("sampledFrames").Num());
    check.header.Check(file.Length() == length, std::format("length mine {} ref {}", file.Length(), length));
    check.header.Check(file.Loops() == static_cast<uint32_t>(root.Get("loops").Num()), "loops");
    check.header.Check(file.StreamFrames() == static_cast<uint32_t>(root.Get("streamFrames").Num()), "stream frames");
    check.header.Check(file.Actors().size() == root.Get("actors").Size(),
                       std::format("actor count mine {} ref {}", file.Actors().size(), root.Get("actors").Size()));
    for (size_t a = 0; a < std::min(file.Actors().size(), root.Get("actors").Size()); ++a) {
        J ref = root.Get("actors")[a];
        const StreamActor& actor = file.Actors()[a];
        check.header.Check(ActorKindName(actor.kind) == ref.Get("kind").Str(), std::format("actor {} kind", a));
        check.header.Check(ref.Get("target").Null() ? actor.target.empty() : actor.target == ref.Get("target").Str(),
                           std::format("actor {} target", a));
        check.header.Check(actor.units.size() == ref.Get("units").Size(), std::format("actor {} units", a));
        J inactive = ref.Get("inactiveBlocks");
        for (size_t k = 0; k < inactive.Size(); ++k) {
            check.header.Check(!file.ActorActive(a, static_cast<uint32_t>(inactive[k][0].Num()) + 1), std::format("actor {} inactive block", a));
        }
    }
    std::map<size_t, DecodedTrack> decoded;
    auto track = [&](size_t index) -> const DecodedTrack& {
        auto it = decoded.find(index);
        if (it == decoded.end()) {
            it = decoded.emplace(index, DecodedTrack{}).first;
            file.DecodeTrack(index, frames, it->second);
        }
        return it->second;
    };
    J tracks = root.Get("tracks");
    for (size_t i = 0; i < tracks.Size(); ++i) {
        J ref = tracks[i];
        const size_t index = static_cast<size_t>(ref.Get("index").Num());
        const DecodedTrack& t = track(index);
        std::vector<Key> keys;
        for (const TrackBlock& b : t.blocks) {
            for (uint32_t k = 0; k < b.key_count; ++k) {
                const Key& key = t.keys[b.first_key + k];
                if (key.frame <= frames && (keys.empty() || keys.back().frame != key.frame)) {
                    keys.push_back(key);
                }
            }
        }
        J ref_keys = ref.Get("keys");
        check.keys.Check(keys.size() == ref_keys.Size(), std::format("track {} key count mine {} ref {}", index, keys.size(), ref_keys.Size()));
        const int components = KindComponents(t.kind);
        for (size_t k = 0; k < std::min<size_t>(keys.size(), ref_keys.Size()); ++k) {
            check.keys.Check(keys[k].frame == static_cast<uint32_t>(ref_keys[k][0].Num()), std::format("track {} key {} frame", index, k));
            CompareVector(check.keys, keys[k].value, true, ref_keys[k][1], components, 2e-6 + 1e-6, std::format("track {} key {}", index, k));
        }
    }
    auto sample = [&](int index, uint32_t f, glm::vec4& out) {
        return index >= 0 && track(static_cast<size_t>(index)).Sample(static_cast<double>(f), out);
    };
    J camera = root.Get("camera");
    const int cam = file.FindActor(ActorKind::Camera);
    const int focal_actor = file.FindActor(ActorKind::CameraParam);
    if (!camera.Null() && cam >= 0) {
        const std::vector<uint16_t>& ct = file.Actors()[static_cast<size_t>(cam)].units[0].tracks;
        const int focal = focal_actor >= 0 ? file.Actors()[static_cast<size_t>(focal_actor)].units[0].tracks[0] : -1;
        J rows = camera.Get("frames");
        for (size_t r = 0; r < rows.Size(); ++r) {
            J row = rows[r];
            const uint32_t f = static_cast<uint32_t>(row[0].Num());
            glm::vec4 rot, pos, fl;
            const bool rv = sample(ct[0], f, rot);
            const bool pv = sample(ct.size() > 1 ? ct[1] : -1, f, pos);
            const bool fv = sample(focal, f, fl);
            for (int c = 0; c < 3; ++c) {
                if (!row[1 + c].Null()) {
                    check.samples.Value(pv ? pos[c] : -1e9, row[1 + c].Num(), 2e-5, std::format("camera {} pos", f));
                }
            }
            for (int c = 0; c < 4; ++c) {
                if (!row[4 + c].Null()) {
                    check.samples.Value(rv ? rot[c] : -1e9, row[4 + c].Num(), 2e-5, std::format("camera {} rot", f));
                }
            }
            if (!row[8].Null()) {
                check.samples.Value(fv ? fl.x : -1e9, row[8].Num(), 2e-4, std::format("camera {} focal", f));
            }
        }
    }
    J transforms = root.Get("transforms");
    for (size_t a = 0; a < file.Actors().size(); ++a) {
        const StreamActor& actor = file.Actors()[a];
        if (actor.kind != ActorKind::Locator && actor.kind != ActorKind::ModelRoot && actor.kind != ActorKind::SiFrame &&
            actor.kind != ActorKind::Timeline && actor.kind != ActorKind::MotionPoints) {
            continue;
        }
        const std::string key = std::format("{}:{}", ActorKindName(actor.kind), actor.target.empty() ? std::string("None") : actor.target);
        J ref = transforms.Get(key);
        if (ref.Null()) {
            check.samples.Fail("missing transform " + key);
            continue;
        }
        std::vector<uint16_t> all;
        for (const ActorUnit& u : actor.units) {
            all.insert(all.end(), u.tracks.begin(), u.tracks.end());
        }
        J rows = ref.Get("frames");
        for (size_t r = 0; r < rows.Size(); ++r) {
            J row = rows[r];
            const uint32_t f = static_cast<uint32_t>(row[0].Num());
            for (size_t t = 0; t < all.size(); ++t) {
                glm::vec4 v;
                const bool valid = sample(all[t], f, v);
                CompareVector(check.samples, v, valid, row[1 + t], KindComponents(file.Tracks()[all[t]].kind), 2e-5,
                              std::format("{} {} track {}", key, f, t));
            }
        }
    }
    J skinned = root.Get("skinned");
    for (const StreamActor& actor : file.Actors()) {
        if (actor.kind != ActorKind::Skeleton) {
            continue;
        }
        J ref = skinned.Get(actor.target);
        if (ref.Null()) {
            continue;
        }
        J rows = ref.Get("frames");
        for (size_t r = 0; r < rows.Size(); r += 3) {
            J row = rows[r];
            for (size_t b = 0; b < actor.units.size() && b < row.Size(); ++b) {
                J bone = row[b];
                glm::vec4 q, t;
                const bool qv = sample(actor.units[b].rotation, static_cast<uint32_t>(r), q);
                const bool tv = sample(actor.units[b].translation, static_cast<uint32_t>(r), t);
                for (int c = 0; c < 4; ++c) {
                    if (!bone[static_cast<size_t>(c)].Null()) {
                        check.samples.Value(qv ? q[c] : -1e9, bone[static_cast<size_t>(c)].Num(), 2e-5,
                                            std::format("{} {} bone {} q", actor.target, r, b));
                    }
                }
                for (int c = 0; c < 3; ++c) {
                    if (!bone[static_cast<size_t>(4 + c)].Null()) {
                        check.samples.Value(tv ? t[c] : -1e9, bone[static_cast<size_t>(4 + c)].Num(), 2e-5,
                                            std::format("{} {} bone {} t", actor.target, r, b));
                    }
                }
            }
        }
    }
    CompareEvents(file, root.Get("events"), check.events);
    return check;
}

struct MotionCheck {
    Stats keys;
    Stats samples;
    Stats events;
};

MotionCheck CheckMotion(const GaniMotion& motion, JsonDoc& doc) {
    MotionCheck check;
    J root{&doc, doc.Root(), true};
    check.keys.Check(motion.frames == static_cast<uint32_t>(root.Get("frames").Num()), "frames");
    J bones = root.Get("bones");
    check.keys.Check(bones.Size() == motion.units.size(), "unit count");
    J per_frame = root.Get("perFrame");
    for (size_t u = 0; u < bones.Size(); ++u) {
        J ref = bones[u];
        const uint32_t hash = NameHash32(std::string("#") + std::string(ref.Get("hash").Str()));
        const GaniUnit* unit = motion.FindUnit(hash);
        if (!unit) {
            check.keys.Fail(std::format("unit {} missing", ref.Get("unit").Str()));
            continue;
        }
        check.keys.Check(unit->flags == static_cast<uint8_t>(ref.Get("flags").Num()), "unit flags");
        J tracks = ref.Get("tracks");
        for (size_t t = 0; t < tracks.Size() && t < unit->tracks.size(); ++t) {
            J keys = tracks[t].Get("keys");
            const GaniTrack& mine = unit->tracks[t];
            check.keys.Check(mine.keys.size() == keys.Size(),
                             std::format("{} track {} key count mine {} ref {}", ref.Get("unit").Str(), t, mine.keys.size(), keys.Size()));
            for (size_t k = 0; k < std::min<size_t>(mine.keys.size(), keys.Size()); ++k) {
                check.keys.Check(mine.keys[k].frame == static_cast<uint32_t>(keys[k][0].Num()), "key frame");
                CompareVector(check.keys, mine.keys[k].value, true, keys[k][1], KindComponents(mine.kind), 3e-6,
                              std::format("{} key {}", ref.Get("unit").Str(), k));
            }
        }
        J rows = per_frame.Get(ref.Get("unit").Str());
        for (size_t f = 0; f < rows.Size(); f += 2) {
            J row = rows[f];
            if (unit->rotation >= 0 && !row[0].Null()) {
                const glm::vec4 q = motion.Sample(*unit, unit->rotation, static_cast<double>(f));
                for (int c = 0; c < 4; ++c) {
                    check.samples.Value(q[c], row[static_cast<size_t>(c)].Num(), 2e-5, std::format("{} frame {} q", ref.Get("unit").Str(), f));
                }
            }
            if (unit->translation >= 0 && !row[4].Null()) {
                const glm::vec4 t = motion.Sample(*unit, unit->translation, static_cast<double>(f));
                for (int c = 0; c < 3; ++c) {
                    check.samples.Value(t[c], row[static_cast<size_t>(4 + c)].Num(), 2e-5, std::format("{} frame {} t", ref.Get("unit").Str(), f));
                }
            }
        }
    }
    J events = root.Get("events");
    check.events.Check(events.Size() == motion.events.size(), std::format("event count mine {} ref {}", motion.events.size(), events.Size()));
    for (size_t i = 0; i < std::min<size_t>(events.Size(), motion.events.size()); ++i) {
        const GaniEvent& e = motion.events[i];
        check.events.Check(NameHash32(events[i].Get("type").Str()) == e.event.type, "event type");
        J sections = events[i].Get("sections");
        check.events.Check(sections.Size() == e.event.sections.size(), "event sections");
        for (size_t k = 0; k < std::min<size_t>(sections.Size(), e.event.sections.size()); ++k) {
            check.events.Check(static_cast<int32_t>(sections[k].Get("start").Num()) == e.event.sections[k].start, "event start");
        }
        J ints = events[i].Get("ints");
        for (size_t k = 0; k < std::min<size_t>(ints.Size(), e.event.ints.size()); ++k) {
            check.events.Check(static_cast<uint32_t>(ints[k].Num()) == static_cast<uint32_t>(e.event.ints[k]), "event int");
        }
    }
    return check;
}

struct RigCheck {
    bool present = false;
    Stats pose;
    Stats relative;
};

RigCheck CheckRig(const GaniMotion& motion, JsonDoc& doc, Vfs& vfs) {
    RigCheck check;
    J ref = J{&doc, doc.Root(), true}.Get("rig");
    if (ref.Null()) {
        return check;
    }
    check.present = true;
    const std::string stem = std::filesystem::path(std::string(ref.Get("skeleton").Str())).stem().string();
    const std::string model = "/Assets/sh/chara/" + stem.substr(0, 3) + "/Scenes/" + stem + ".fmdl";
    auto rig_bytes = vfs.ReadFile("/Assets/sh/rig/frig/human_finger.frig");
    auto fmdl = vfs.ReadFile(model);
    Rig rig;
    Skeleton skeleton;
    RigBinding binding;
    std::string error;
    if (!rig_bytes || !fmdl || !rig.Parse(*rig_bytes, &error) || !ReadFmdlSkeleton(*fmdl, skeleton) || !binding.Bind(rig, skeleton)) {
        check.pose.Fail(std::format("rig or skeleton {} unusable {}", model, error));
        return check;
    }
    J bones = ref.Get("bones");
    check.pose.Check(bones.Size() == skeleton.Size(), std::format("bone count mine {} ref {}", skeleton.Size(), bones.Size()));
    J frames = ref.Get("frames");
    for (size_t i = 0; i < frames.Size(); ++i) {
        const double frame = frames[i].Get("frame").Num();
        if (frame >= motion.frames && motion.frames > 0) {
            continue;
        }
        RigPose pose;
        RigOutput out;
        SampleRigPose(rig, motion, frame, pose);
        EvaluateRig(rig, binding, skeleton, pose, out);
        J positions = frames[i].Get("position");
        J rotations = frames[i].Get("rotation");
        for (size_t b = 0; b < std::min<size_t>(positions.Size(), skeleton.Size()); ++b) {
            for (int c = 0; c < 3; ++c) {
                check.pose.Value(out.position[b][c], positions[b][static_cast<size_t>(c)].Num(), 5e-4,
                                 std::format("frame {} {} p{}", frame, skeleton.names[b], c));
            }
            J q = rotations[b];
            const glm::quat r(static_cast<float>(q[3].Num()), static_cast<float>(q[0].Num()), static_cast<float>(q[1].Num()),
                              static_cast<float>(q[2].Num()));
            check.pose.Value(1.0 - std::abs(glm::dot(out.rotation[b], r)), 0.0, 2e-6, std::format("frame {} {} rotation", frame, skeleton.names[b]));
        }
        RigPose relative = pose;
        RigOutput again;
        MakeRigPoseRelative(rig, binding, skeleton, out, relative);
        EvaluateRig(rig, binding, skeleton, relative, again);
        for (size_t b = 0; b < skeleton.Size(); ++b) {
            check.relative.Value(glm::length(again.position[b] - out.position[b]), 0.0, 1e-4,
                                 std::format("frame {} {} relative", frame, skeleton.names[b]));
        }
    }
    return check;
}

struct HelpCheck {
    Stats rotation;
    Stats position;
};

HelpCheck CheckHelpBones(JsonDoc& doc, Vfs& vfs) {
    HelpCheck check;
    J root{&doc, doc.Root(), true};
    const std::string frdv_path(root.Get("frdv").Str());
    const std::string fmdl_path(root.Get("fmdl").Str());
    auto frdv = vfs.ReadFile(frdv_path);
    auto fmdl = vfs.ReadFile(fmdl_path);
    HelpBones help;
    Skeleton skeleton;
    std::string error;
    if (!frdv || !fmdl || !help.Parse(*frdv, &error) || !ReadFmdlSkeleton(*fmdl, skeleton)) {
        check.rotation.Fail(std::format("{} or {} unusable {}", frdv_path, fmdl_path, error));
        return check;
    }
    check.rotation.Check(help.UnknownEntries() == 0, std::format("{} entries of unknown types", help.UnknownEntries()));
    J poses = root.Get("poses");
    for (size_t i = 0; i < poses.Size(); ++i) {
        J in_rotation = poses[i].Get("rotation");
        J in_position = poses[i].Get("position");
        J out_rotation = poses[i].Get("outRotation");
        J out_position = poses[i].Get("outPosition");
        const size_t n = std::min(skeleton.Size(), in_rotation.Size());
        std::vector<glm::quat> rotation(n);
        std::vector<glm::vec3> position(n);
        auto quat = [](J q) {
            return glm::quat(static_cast<float>(q[3].Num()), static_cast<float>(q[0].Num()), static_cast<float>(q[1].Num()),
                             static_cast<float>(q[2].Num()));
        };
        auto vec = [](J v) { return glm::vec3(static_cast<float>(v[0].Num()), static_cast<float>(v[1].Num()), static_cast<float>(v[2].Num())); };
        for (size_t b = 0; b < n; ++b) {
            rotation[b] = quat(in_rotation[b]);
            position[b] = vec(in_position[b]);
        }
        help.Evaluate(std::span<const glm::vec3>(skeleton.bind_local.data(), n), rotation, position);
        for (size_t b = 0; b < n; ++b) {
            const glm::dquat mine = glm::normalize(glm::dquat(rotation[b]));
            const glm::dquat ref = glm::normalize(glm::dquat(quat(out_rotation[b])));
            const double dot = std::min(1.0, std::abs(glm::dot(mine, ref)));
            check.rotation.Value(2.0 * std::acos(dot), 0.0, 1e-4, std::format("pose {} {} rotation", i, skeleton.names[b]));
            check.position.Value(glm::length(position[b] - vec(out_position[b])), 0.0, 1e-5,
                                 std::format("pose {} {} position", i, skeleton.names[b]));
        }
    }
    return check;
}

bool Report(const std::string& name, const std::vector<std::pair<std::string, const Stats*>>& parts) {
    bool ok = true;
    std::string line = std::format("anim test: {:<28}", name);
    for (const auto& [label, s] : parts) {
        line += std::format(" {} {}/{} max {:.2e}", label, s->compared - s->failed, s->compared, s->max_error);
        ok = ok && s->failed == 0;
    }
    LogInfo("{}{}", line, ok ? "" : "  FAIL");
    for (const auto& [label, s] : parts) {
        if (s->failed) {
            LogWarn("anim test:   {} first failure: {}", label, s->first_failure);
        }
    }
    return ok;
}

bool CheckSkeletons(Vfs& vfs) {
    bool ok = true;
    for (const char* path : {"/Assets/sh/chara/plr/Scenes/plr0_main0_def.fmdl", "/Assets/sh/chara/och/Scenes/och0_main0_def.fmdl",
                             "/Assets/sh/chara/coc/Scenes/coc0_main0_def.fmdl", "/Assets/sh/chara/bab/Scenes/bab0_main0_def.fmdl",
                             "/Assets/sh/chara/lte/Scenes/lte0_main0_def.fmdl", "/Assets/sh/chara/frz/Scenes/frz0_main0_def.fmdl",
                             "/Assets/sh/chara/pab/Scenes/pab0_main0_def.fmdl", "/Assets/sh/item/lig/Scenes/shl0_main0_def.fmdl"}) {
        auto bytes = vfs.ReadFile(path);
        if (!bytes) {
            LogWarn("anim test: {} not found", path);
            ok = false;
            continue;
        }
        Skeleton skeleton;
        FmdlModel model;
        const bool mine = ReadFmdlSkeleton(*bytes, skeleton);
        const bool theirs = LoadFmdl(*bytes, path, model);
        bool same = mine && theirs && skeleton.Size() == model.bone_names.size();
        for (size_t i = 0; same && i < skeleton.Size(); ++i) {
            same = skeleton.names[i] == model.bone_names[i] && skeleton.parents[i] == model.bone_parent[i] &&
                   glm::length(skeleton.bind_world[i] - model.bone_world[i]) < 1e-6f;
        }
        Pose pose;
        pose.Reset(skeleton.Size());
        std::vector<glm::mat4> world;
        ComputeBoneWorld(skeleton, pose, world);
        float bind_error = 0.0f;
        for (size_t i = 0; i < skeleton.Size(); ++i) {
            bind_error = std::max(bind_error, glm::length(glm::vec3(world[i][3]) - skeleton.bind_world[i]));
        }
        LogInfo("anim test: skeleton {} bones {} matches fmdl loader {} bind error {:.2e}", path, skeleton.Size(), same ? "yes" : "NO", bind_error);
        ok = ok && same && bind_error < 1e-4f;
    }
    return ok;
}

bool CheckPlayerPose(Vfs& vfs) {
    auto fmdl = vfs.ReadFile("/Assets/sh/chara/plr/Scenes/plr0_main0_def.fmdl");
    auto fsm = vfs.ReadFile("/Assets/sh/demo/demo_stream/gc_p00_010.fsm");
    if (!fmdl || !fsm) {
        return false;
    }
    Skeleton skeleton;
    ReadFmdlSkeleton(*fmdl, skeleton);
    DemoStreamFile file;
    if (!file.Parse(std::move(*fsm), nullptr)) {
        return false;
    }
    const int skel = file.FindActor(ActorKind::Skeleton, "HM_plr0_main0_def");
    if (skel < 0) {
        return false;
    }
    Pose pose;
    pose.Reset(skeleton.Size());
    int unresolved = 0;
    for (const ActorUnit& u : file.Actors()[static_cast<size_t>(skel)].units) {
        const int bone = skeleton.Find(u.hash);
        if (bone < 0) {
            ++unresolved;
            continue;
        }
        DecodedTrack t;
        glm::vec4 v;
        if (u.rotation >= 0 && file.DecodeTrack(static_cast<size_t>(u.rotation), file.Length(), t) && t.Sample(64.0, v)) {
            pose.rotation[static_cast<size_t>(bone)] = ToQuat(v);
        }
        if (u.translation >= 0 && file.DecodeTrack(static_cast<size_t>(u.translation), file.Length(), t) && t.Sample(64.0, v)) {
            pose.offset[static_cast<size_t>(bone)] = glm::vec3(v);
        }
    }
    std::vector<glm::mat4> world;
    ComputeBoneWorld(skeleton, pose, world);
    const int head = skeleton.FindName("SKL_004_HEAD");
    const int lfoot = skeleton.FindName("SKL_032_LFOOT");
    const int rfoot = skeleton.FindName("SKL_042_RFOOT");
    const float head_y = head >= 0 ? world[static_cast<size_t>(head)][3][1] : 0.0f;
    const float lfoot_y = lfoot >= 0 ? world[static_cast<size_t>(lfoot)][3][1] : 0.0f;
    const float rfoot_y = rfoot >= 0 ? world[static_cast<size_t>(rfoot)][3][1] : 0.0f;
    const bool ok = unresolved == 0 && head_y > 1.4f && head_y < 1.9f && lfoot_y < 0.25f && rfoot_y < 0.25f;
    LogInfo("anim test: gc_p00_010 player pose frame 64: {} unresolved bones, head y {:.3f}, feet y {:.3f} {:.3f} (model space) {}", unresolved,
            head_y, lfoot_y, rfoot_y, ok ? "ok" : "FAIL");
    return ok;
}

}

int RunAnimTestIfRequested(int argc, char** argv, Vfs& vfs) {
    std::filesystem::path dir;
    std::string only;
    bool requested = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--anim-test") {
            requested = true;
            dir = i + 1 < argc && !std::string_view(argv[i + 1]).starts_with("--") ? argv[++i] : "dump/demo";
        } else if (arg == "--anim-only" && i + 1 < argc) {
            only = argv[++i];
        }
    }
    if (!requested) {
        return -1;
    }
    vfs.LoadPackage("/Assets/sh/level/common/resident.fpk");
    vfs.LoadPackage("/Assets/sh/level_asset/chara/player/game_object/player2_common_motion.fpk");
    int failures = 0;
    int checked = 0;
    std::vector<std::filesystem::path> files;
    std::error_code error;
    if (!std::filesystem::is_directory(dir, error)) {
        LogError("anim test: no reference folder {} (pass it after --anim-test)", dir.string());
        return 1;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() == ".json" && (name.starts_with("gc_") || name.starts_with("motion_") || name.starts_with("helpbones_"))) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    MotionArchive gimmick;
    MotionArchive player;
    for (auto [archive, path] : {std::pair{&gimmick, "/Assets/sh/motion/mtar/gimmick/ShGimmick_layers.mtar"},
                                 std::pair{&player, "/Assets/sh/motion/mtar/player/ShPlayer_layers.mtar"}}) {
        std::string error;
        auto bytes = vfs.ReadFile(path);
        if (!bytes || !archive->Parse(std::move(*bytes), &error)) {
            LogError("anim test: {} unreadable {}", path, error);
            ++failures;
        }
    }
    for (const auto& path : files) {
        const std::string stem = path.stem().string();
        if (!only.empty() && stem.find(only) == std::string::npos) {
            continue;
        }
        JsonDoc doc;
        if (!LoadJson(path, doc)) {
            LogError("anim test: {} is not valid JSON", path.string());
            ++failures;
            continue;
        }
        ++checked;
        J root{&doc, doc.Root(), true};
        if (stem.starts_with("helpbones_")) {
            const HelpCheck h = CheckHelpBones(doc, vfs);
            failures += Report(stem, {{"rotation", &h.rotation}, {"position", &h.position}}) ? 0 : 1;
            continue;
        }
        if (stem.starts_with("motion_")) {
            const std::string_view gani_path = root.Get("path").Str();
            const uint64_t code = gani_path.empty() ? NameHash64(std::string("#") + stem.substr(7)) : PathCode64(gani_path);
            const GaniMotion* motion = gimmick.Find(code);
            if (!motion) {
                motion = player.Find(code);
            }
            if (!motion) {
                LogError("anim test: {} has no motion {:#x}", stem, code);
                ++failures;
                continue;
            }
            const MotionCheck c = CheckMotion(*motion, doc);
            failures += Report(stem, {{"keys", &c.keys}, {"samples", &c.samples}, {"events", &c.events}}) ? 0 : 1;
            const RigCheck r = CheckRig(*motion, doc, vfs);
            if (r.present) {
                failures += Report(stem + " rig", {{"pose", &r.pose}, {"relative", &r.relative}}) ? 0 : 1;
            }
            continue;
        }
        std::string asset = "/Assets/sh/demo/demo_stream/" + stem + ".fsm";
        if (stem.ends_with("_Eng")) {
            asset = "/Assets/sh/demo/demo_stream/#Eng/" + stem.substr(0, stem.size() - 4) + ".fsm";
        }
        auto bytes = vfs.ReadFile(asset);
        DemoStreamFile file;
        std::string error;
        if (!bytes || !file.Parse(std::move(*bytes), &error)) {
            LogError("anim test: {} unreadable {}", asset, error);
            ++failures;
            continue;
        }
        const DemoCheck c = CheckDemo(file, doc);
        failures += Report(stem, {{"header", &c.header}, {"keys", &c.keys}, {"samples", &c.samples}, {"events", &c.events}}) ? 0 : 1;
    }
    if (only.empty()) {
        failures += CheckSkeletons(vfs) ? 0 : 1;
        failures += CheckPlayerPose(vfs) ? 0 : 1;
    }
    LogInfo("anim test: {} files checked, {} failed", checked, failures);
    return failures == 0 ? 0 : 2;
}

}
