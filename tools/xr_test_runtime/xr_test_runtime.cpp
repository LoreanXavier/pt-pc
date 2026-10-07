#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_reflection.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define PT_XR_EXPORT extern "C" __declspec(dllexport)
#else
#define PT_XR_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

std::mutex g_mutex;
FILE* g_log = nullptr;
int g_violations = 0;

void Log(const char* format, ...) {
    if (!g_log) {
        const char* path = std::getenv("PT_XRTEST_LOG");
        g_log = path && *path ? std::fopen(path, "a") : nullptr;
        if (!g_log) g_log = stderr;
    }
    va_list args;
    va_start(args, format);
    std::fprintf(g_log, "xrtest: ");
    std::vfprintf(g_log, format, args);
    std::fprintf(g_log, "\n");
    std::fflush(g_log);
    va_end(args);
}

XrResult Violation(XrResult result, const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    ++g_violations;
    Log("VIOLATION %s", text);
    return result;
}

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};
struct Vec3 {
    float x = 0, y = 0, z = 0;
};
struct Pose {
    Quat q;
    Vec3 p;
};

Quat Mul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Quat Conj(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }
Vec3 Rotate(const Quat& q, const Vec3& v) {
    const Quat r = Mul(Mul(q, Quat{v.x, v.y, v.z, 0}), Conj(q));
    return {r.x, r.y, r.z};
}
Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Pose Compose(const Pose& a, const Pose& b) { return {Mul(a.q, b.q), Add(a.p, Rotate(a.q, b.p))}; }
Pose Inverse(const Pose& a) {
    const Quat qi = Conj(a.q);
    const Vec3 pi = Rotate(qi, a.p);
    return {qi, {-pi.x, -pi.y, -pi.z}};
}
Quat AxisAngle(float x, float y, float z, float radians) {
    const float s = std::sin(radians * 0.5f);
    return {x * s, y * s, z * s, std::cos(radians * 0.5f)};
}
Quat FromYawPitchRoll(float yaw_deg, float pitch_deg, float roll_deg) {
    const float k = 3.14159265358979f / 180.0f;
    return Mul(Mul(AxisAngle(0, 1, 0, yaw_deg * k), AxisAngle(1, 0, 0, pitch_deg * k)), AxisAngle(0, 0, 1, roll_deg * k));
}
Pose FromXr(const XrPosef& p) { return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}, {p.position.x, p.position.y, p.position.z}}; }
XrPosef ToXr(const Pose& p) {
    XrPosef out;
    out.orientation = {p.q.x, p.q.y, p.q.z, p.q.w};
    out.position = {p.p.x, p.p.y, p.p.z};
    return out;
}
bool Normalized(const XrQuaternionf& q) {
    const float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    return std::abs(n - 1.0f) < 1.0e-3f;
}

struct Keyframe {
    int frame = 0;
    float v[6] = {};
};

struct InputRange {
    int from = 0;
    int to = 0;
    std::string path;
    float x = 0;
    float y = 0;
};

struct Script {
    std::vector<Keyframe> head;
    std::vector<Keyframe> hands[2];
    std::vector<InputRange> inputs;
    std::multimap<int, std::string> events;
    std::set<int> dumps;
    int dump_every = 0;
};

std::vector<std::string> Split(const std::string& text, const char* separators) {
    std::vector<std::string> out;
    std::string current;
    for (char c : text) {
        if (std::strchr(separators, c)) {
            if (!current.empty()) out.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

Script LoadScript() {
    Script s;
    std::string text;
    if (const char* inline_text = std::getenv("PT_XRTEST_SCRIPT")) text = inline_text;
    if (const char* file = std::getenv("PT_XRTEST_SCRIPT_FILE")) {
        if (FILE* f = std::fopen(file, "rb")) {
            char buffer[4096];
            size_t n;
            while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) text.append(buffer, n);
            std::fclose(f);
        } else {
            Log("cannot read PT_XRTEST_SCRIPT_FILE %s", file);
        }
    }
    for (const std::string& entry : Split(text, ";\n\r")) {
        const std::vector<std::string> w = Split(entry, " \t");
        if (w.empty()) continue;
        auto num = [&](size_t i) { return i < w.size() ? static_cast<float>(std::atof(w[i].c_str())) : 0.0f; };
        if (w[0] == "head" && w.size() >= 5) {
            Keyframe k;
            k.frame = std::atoi(w[1].c_str());
            for (int i = 0; i < 6; ++i) k.v[i] = num(2 + i);
            s.head.push_back(k);
        } else if (w[0] == "hand" && w.size() >= 9) {
            Keyframe k;
            k.frame = std::atoi(w[2].c_str());
            for (int i = 0; i < 6; ++i) k.v[i] = num(3 + i);
            s.hands[w[1] == "right" ? 1 : 0].push_back(k);
        } else if (w[0] == "input" && w.size() >= 5) {
            InputRange r;
            r.from = std::atoi(w[1].c_str());
            r.to = std::atoi(w[2].c_str());
            r.path = w[3];
            r.x = num(4);
            r.y = num(5);
            s.inputs.push_back(r);
        } else if (w[0] == "event" && w.size() >= 3) {
            s.events.emplace(std::atoi(w[1].c_str()), w[2]);
        } else if (w[0] == "dump" && w.size() >= 2) {
            s.dumps.insert(std::atoi(w[1].c_str()));
        } else if (w[0] == "dumpevery" && w.size() >= 2) {
            s.dump_every = std::atoi(w[1].c_str());
        } else {
            Log("script entry not understood: %s", entry.c_str());
        }
    }
    auto by_frame = [](const Keyframe& a, const Keyframe& b) { return a.frame < b.frame; };
    std::sort(s.head.begin(), s.head.end(), by_frame);
    std::sort(s.hands[0].begin(), s.hands[0].end(), by_frame);
    std::sort(s.hands[1].begin(), s.hands[1].end(), by_frame);
    return s;
}

void Sample(const std::vector<Keyframe>& keys, int frame, float out[6]) {
    if (keys.empty()) {
        std::fill(out, out + 6, 0.0f);
        return;
    }
    if (frame <= keys.front().frame) {
        std::copy(keys.front().v, keys.front().v + 6, out);
        return;
    }
    for (size_t i = 1; i < keys.size(); ++i) {
        if (frame <= keys[i].frame) {
            const float t = static_cast<float>(frame - keys[i - 1].frame) / static_cast<float>(std::max(1, keys[i].frame - keys[i - 1].frame));
            for (int j = 0; j < 6; ++j) out[j] = keys[i - 1].v[j] + (keys[i].v[j] - keys[i - 1].v[j]) * t;
            return;
        }
    }
    std::copy(keys.back().v, keys.back().v + 6, out);
}

constexpr uint64_t kMagicInstance = 0x7074785249ull, kMagicSession = 0x7074785253ull, kMagicSpace = 0x7074785250ull,
                   kMagicSwapchain = 0x7074785357ull, kMagicActionSet = 0x7074784153ull, kMagicAction = 0x7074784143ull;

struct Instance;
struct Session;
struct Action;

struct Object {
    uint64_t magic = 0;
};

struct VkFunctions {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkDestroyBuffer DestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkUnmapMemory UnmapMemory = nullptr;
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
};

struct SwapchainImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct Swapchain : Object {
    Session* session = nullptr;
    XrSwapchainCreateInfo info{};
    std::vector<SwapchainImage> images;
    std::deque<uint32_t> acquired;
    bool waited = false;
    uint32_t next = 0;
    int last_released = -1;
    bool released_since_end = false;
    uint64_t released_count = 0;
};

struct Space : Object {
    Session* session = nullptr;
    bool action = false;
    XrReferenceSpaceType type = XR_REFERENCE_SPACE_TYPE_LOCAL;
    Action* action_ptr = nullptr;
    XrPath subaction = XR_NULL_PATH;
    Pose offset;
};

struct ActionSet : Object {
    Instance* instance = nullptr;
    std::string name;
    bool attached = false;
    std::vector<Action*> actions;
};

struct Action : Object {
    ActionSet* set = nullptr;
    std::string name;
    XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::vector<XrPath> subactions;
    std::map<XrPath, std::vector<XrPath>> bindings;
    struct State {
        float x = 0, y = 0;
        bool active = false;
        bool changed = false;
        XrTime change_time = 0;
    };
    State state[3];
};

struct Session : Object {
    Instance* instance = nullptr;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    bool exit_requested = false;
    VkInstance vk_instance = VK_NULL_HANDLE;
    VkPhysicalDevice vk_physical = VK_NULL_HANDLE;
    VkDevice vk_device = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    uint32_t queue_index = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkFunctions vk;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    std::set<Swapchain*> swapchains;
    std::set<Space*> spaces;
    int64_t waited = 0;
    int64_t begun = 0;
    int64_t ended = 0;
    bool frame_begun = false;
    XrTime last_predicted = 0;
    std::set<XrTime> predicted;
    int frames_without_layers = 0;
    std::vector<ActionSet*> attached;
    XrPath profile = XR_NULL_PATH;
    bool focused = false;
    int64_t projection_layers = 0;
    int64_t quad_layers = 0;
};

struct Instance : Object {
    XrInstanceCreateInfo info{};
    std::string app_name;
    XrVersion api_version = 0;
    std::set<std::string> extensions;
    std::map<std::string, XrPath> paths;
    std::vector<std::string> path_names{""};
    std::deque<std::vector<uint8_t>> events;
    std::set<Session*> sessions;
    std::set<ActionSet*> action_sets;
    std::map<XrPath, std::set<XrPath>> suggested;
    bool system_got = false;
    bool requirements_checked = false;
    VkInstance created_vk_instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr vk_gipa = nullptr;
    Script script;
    uint32_t view_w = 1024, view_h = 1104;
    float ipd = 0.064f;
    std::vector<int64_t> formats;
    std::string out_dir;
};

constexpr XrSystemId kSystemId = 0x5054;
constexpr XrDuration kPeriod = 11111111;
constexpr XrTime kStartTime = 1000000000;

Instance* g_instance = nullptr;

template <typename T>
T* Get(uint64_t handle, uint64_t magic) {
    if (handle == 0) return nullptr;
    T* object = reinterpret_cast<T*>(handle);
    return object->magic == magic ? object : nullptr;
}
Instance* GetInstance(XrInstance h) { return h && g_instance && reinterpret_cast<Instance*>(h) == g_instance ? g_instance : nullptr; }
Session* GetSession(XrSession h) { return Get<Session>(reinterpret_cast<uint64_t>(h), kMagicSession); }
Space* GetSpace(XrSpace h) { return Get<Space>(reinterpret_cast<uint64_t>(h), kMagicSpace); }
Swapchain* GetSwapchain(XrSwapchain h) { return Get<Swapchain>(reinterpret_cast<uint64_t>(h), kMagicSwapchain); }
ActionSet* GetActionSet(XrActionSet h) { return Get<ActionSet>(reinterpret_cast<uint64_t>(h), kMagicActionSet); }
Action* GetAction(XrAction h) { return Get<Action>(reinterpret_cast<uint64_t>(h), kMagicAction); }

XrPath PathOf(Instance* inst, const std::string& name) {
    auto it = inst->paths.find(name);
    if (it != inst->paths.end()) return it->second;
    const XrPath id = inst->path_names.size();
    inst->path_names.push_back(name);
    inst->paths[name] = id;
    return id;
}
std::string NameOf(Instance* inst, XrPath path) { return path < inst->path_names.size() ? inst->path_names[path] : std::string("<invalid>"); }

void PushEvent(Instance* inst, const void* data, size_t size) {
    std::vector<uint8_t> bytes(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    inst->events.push_back(std::move(bytes));
}

const char* StateName(XrSessionState s) {
    switch (s) {
#define PT_STATE_NAME(name, value) \
    case name: return #name;
        XR_LIST_ENUM_XrSessionState(PT_STATE_NAME)
#undef PT_STATE_NAME
    default: return "?";
    }
}

void SetState(Session* s, XrSessionState state) {
    if (s->state == state) return;
    s->state = state;
    XrEventDataSessionStateChanged e{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
    e.session = reinterpret_cast<XrSession>(s);
    e.state = state;
    e.time = s->last_predicted ? s->last_predicted : kStartTime;
    PushEvent(s->instance, &e, sizeof(e));
    Log("session state %s", StateName(state));
}

int CurrentFrame(Session* s) { return static_cast<int>(std::max<int64_t>(0, s->waited - 1)); }

struct Profile {
    const char* path;
    std::vector<const char*> both;
    std::vector<const char*> left;
    std::vector<const char*> right;
};

const std::vector<Profile>& Profiles() {
    static const std::vector<Profile> profiles = {
        {"/interaction_profiles/khr/simple_controller",
         {"/input/select/click", "/input/menu/click", "/input/grip/pose", "/input/aim/pose", "/output/haptic"},
         {},
         {}},
        {"/interaction_profiles/oculus/touch_controller",
         {"/input/squeeze/value", "/input/trigger/value", "/input/trigger/touch", "/input/thumbstick", "/input/thumbstick/x",
          "/input/thumbstick/y", "/input/thumbstick/click", "/input/thumbstick/touch", "/input/thumbrest/touch", "/input/grip/pose",
          "/input/aim/pose", "/output/haptic"},
         {"/input/x/click", "/input/x/touch", "/input/y/click", "/input/y/touch", "/input/menu/click"},
         {"/input/a/click", "/input/a/touch", "/input/b/click", "/input/b/touch", "/input/system/click"}},
        {"/interaction_profiles/valve/index_controller",
         {"/input/system/click", "/input/system/touch", "/input/a/click", "/input/a/touch", "/input/b/click", "/input/b/touch",
          "/input/squeeze/value", "/input/squeeze/force", "/input/trigger/click", "/input/trigger/value", "/input/trigger/touch",
          "/input/thumbstick", "/input/thumbstick/x", "/input/thumbstick/y", "/input/thumbstick/click", "/input/thumbstick/touch",
          "/input/trackpad", "/input/trackpad/x", "/input/trackpad/y", "/input/trackpad/force", "/input/trackpad/touch",
          "/input/grip/pose", "/input/aim/pose", "/output/haptic"},
         {},
         {}},
        {"/interaction_profiles/htc/vive_controller",
         {"/input/system/click", "/input/squeeze/click", "/input/menu/click", "/input/trigger/click", "/input/trigger/value",
          "/input/trackpad", "/input/trackpad/x", "/input/trackpad/y", "/input/trackpad/click", "/input/trackpad/touch",
          "/input/grip/pose", "/input/aim/pose", "/output/haptic"},
         {},
         {}},
        {"/interaction_profiles/microsoft/motion_controller",
         {"/input/menu/click", "/input/squeeze/click", "/input/trigger/value", "/input/thumbstick", "/input/thumbstick/x",
          "/input/thumbstick/y", "/input/thumbstick/click", "/input/trackpad", "/input/trackpad/x", "/input/trackpad/y",
          "/input/trackpad/click", "/input/trackpad/touch", "/input/grip/pose", "/input/aim/pose", "/output/haptic"},
         {},
         {}},
    };
    return profiles;
}

const Profile* FindProfile(const std::string& path) {
    for (const Profile& p : Profiles())
        if (path == p.path) return &p;
    return nullptr;
}

bool ValidBindingPath(const Profile& profile, const std::string& binding) {
    for (int side = 0; side < 2; ++side) {
        const std::string prefix = side == 0 ? "/user/hand/left" : "/user/hand/right";
        if (binding.rfind(prefix, 0) != 0) continue;
        const std::string component = binding.substr(prefix.size());
        for (const char* c : profile.both)
            if (component == c) return true;
        for (const char* c : side == 0 ? profile.left : profile.right)
            if (component == c) return true;
        for (const char* c : profile.both) {
            const std::string full = c;
            if (full.rfind(component + "/", 0) == 0) return true;
        }
        for (const char* c : side == 0 ? profile.left : profile.right) {
            const std::string full = c;
            if (full.rfind(component + "/", 0) == 0) return true;
        }
    }
    return false;
}

void InputValue(Instance* inst, int frame, const std::string& path, float& x, float& y) {
    x = y = 0.0f;
    std::string wanted = path;
    auto replace = [&](const char* from, const char* to) {
        const size_t at = wanted.find(from);
        if (at != std::string::npos) wanted.replace(at, std::strlen(from), to);
    };
    replace("/input/select/click", "/input/trigger/value");
    for (const InputRange& r : inst->script.inputs) {
        if (frame < r.from || frame >= r.to) continue;
        std::string p = r.path;
        if (p == wanted || (wanted.size() > p.size() && wanted.rfind(p + "/", 0) == 0)) {
            const std::string tail = wanted.size() > p.size() ? wanted.substr(p.size() + 1) : std::string();
            if (tail == "y") {
                x = r.y;
            } else if (tail == "x" || tail == "value" || tail == "click" || tail == "touch" || tail.empty()) {
                x = r.x;
                y = r.y;
            }
        } else if (p.size() > wanted.size() && p.rfind(wanted + "/", 0) == 0) {
            x = r.x;
        }
    }
}

Pose HeadPose(Instance* inst, int frame) {
    float v[6];
    Sample(inst->script.head, frame, v);
    return {FromYawPitchRoll(v[0], v[1], v[2]), {v[3], v[4], v[5]}};
}

Pose HandPose(Instance* inst, int side, int frame) {
    if (inst->script.hands[side].empty()) {
        const Pose head = HeadPose(inst, frame);
        return {Quat{}, {head.p.x + (side == 0 ? -0.2f : 0.2f), head.p.y - 0.5f, head.p.z - 0.3f}};
    }
    float v[6];
    Sample(inst->script.hands[side], frame, v);
    return {FromYawPitchRoll(v[3], v[4], v[5]), {v[0], v[1], v[2]}};
}

bool SpaceInLocal(Space* space, int frame, Pose& out) {
    Instance* inst = space->session->instance;
    Pose base;
    if (space->action) {
        if (!space->action_ptr || !space->action_ptr->set->attached) return false;
        Session* s = space->session;
        if (s->profile == XR_NULL_PATH) return false;
        auto it = space->action_ptr->bindings.find(s->profile);
        if (it == space->action_ptr->bindings.end() || it->second.empty()) return false;
        int side = -1;
        const std::string sub = NameOf(inst, space->subaction);
        for (XrPath b : it->second) {
            const std::string name = NameOf(inst, b);
            const int bound_side = name.rfind("/user/hand/left", 0) == 0 ? 0 : name.rfind("/user/hand/right", 0) == 0 ? 1 : -1;
            if (space->subaction == XR_NULL_PATH || (sub == "/user/hand/left" && bound_side == 0) || (sub == "/user/hand/right" && bound_side == 1)) {
                side = bound_side;
                break;
            }
        }
        if (side < 0) return false;
        base = HandPose(inst, side, frame);
    } else {
        switch (space->type) {
        case XR_REFERENCE_SPACE_TYPE_VIEW: base = HeadPose(inst, frame); break;
        case XR_REFERENCE_SPACE_TYPE_STAGE: base = {Quat{}, {0.0f, -1.6f, 0.0f}}; break;
        default: base = {}; break;
        }
    }
    out = Compose(base, space->offset);
    return true;
}

template <typename T>
void LoadInstanceFn(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, T& fn, const char* name) {
    fn = reinterpret_cast<T>(gipa(instance, name));
}

bool LoadDeviceFunctions(Session* s, PFN_vkGetInstanceProcAddr gipa) {
    VkFunctions& f = s->vk;
    f.gipa = gipa;
    LoadInstanceFn(gipa, s->vk_instance, f.gdpa, "vkGetDeviceProcAddr");
    LoadInstanceFn(gipa, s->vk_instance, f.GetPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
    if (!f.gdpa) return false;
#define PT_DEVICE_FN(name) f.name = reinterpret_cast<PFN_vk##name>(f.gdpa(s->vk_device, "vk" #name))
    PT_DEVICE_FN(CreateImage);
    PT_DEVICE_FN(DestroyImage);
    PT_DEVICE_FN(GetImageMemoryRequirements);
    PT_DEVICE_FN(AllocateMemory);
    PT_DEVICE_FN(FreeMemory);
    PT_DEVICE_FN(BindImageMemory);
    PT_DEVICE_FN(CreateBuffer);
    PT_DEVICE_FN(DestroyBuffer);
    PT_DEVICE_FN(GetBufferMemoryRequirements);
    PT_DEVICE_FN(BindBufferMemory);
    PT_DEVICE_FN(MapMemory);
    PT_DEVICE_FN(UnmapMemory);
    PT_DEVICE_FN(CreateCommandPool);
    PT_DEVICE_FN(DestroyCommandPool);
    PT_DEVICE_FN(AllocateCommandBuffers);
    PT_DEVICE_FN(BeginCommandBuffer);
    PT_DEVICE_FN(EndCommandBuffer);
    PT_DEVICE_FN(CmdPipelineBarrier);
    PT_DEVICE_FN(CmdCopyImageToBuffer);
    PT_DEVICE_FN(QueueSubmit);
    PT_DEVICE_FN(QueueWaitIdle);
    PT_DEVICE_FN(GetDeviceQueue);
    PT_DEVICE_FN(ResetCommandBuffer);
    PT_DEVICE_FN(DeviceWaitIdle);
#undef PT_DEVICE_FN
    return f.CreateImage && f.QueueSubmit && f.CmdCopyImageToBuffer;
}

uint32_t MemoryType(Session* s, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props{};
    s->vk.GetPhysicalDeviceMemoryProperties(s->vk_physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    return UINT32_MAX;
}

template <typename F>
bool RunCommands(Session* s, F&& record) {
    if (!s->pool) {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = s->queue_family;
        if (s->vk.CreateCommandPool(s->vk_device, &pool, nullptr, &s->pool) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = s->pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        if (s->vk.AllocateCommandBuffers(s->vk_device, &alloc, &s->cmd) != VK_SUCCESS) return false;
    }
    s->vk.ResetCommandBuffer(s->cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    s->vk.BeginCommandBuffer(s->cmd, &begin);
    record(s->cmd);
    s->vk.EndCommandBuffer(s->cmd);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &s->cmd;
    if (s->vk.QueueSubmit(s->queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) return false;
    return s->vk.QueueWaitIdle(s->queue) == VK_SUCCESS;
}

VkImageAspectFlags AspectOf(int64_t format) {
    switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

float HalfToFloat(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16;
    uint32_t exponent = (h >> 10) & 0x1Fu;
    uint32_t mantissa = h & 0x3FFu;
    uint32_t value;
    if (exponent == 0) {
        if (mantissa == 0) {
            value = sign;
        } else {
            exponent = 127 - 15 + 1;
            while (!(mantissa & 0x400u)) {
                mantissa <<= 1;
                --exponent;
            }
            value = sign | (exponent << 23) | ((mantissa & 0x3FFu) << 13);
        }
    } else if (exponent == 31) {
        value = sign | 0x7F800000u | (mantissa << 13);
    } else {
        value = sign | ((exponent + 112) << 23) | (mantissa << 13);
    }
    float f;
    std::memcpy(&f, &value, 4);
    return f;
}

bool DumpImage(Session* s, Swapchain* sc, uint32_t index, uint32_t layer, const std::string& path, const XrRect2Di* rect) {
    const uint32_t w = sc->info.width;
    const uint32_t h = sc->info.height;
    const int64_t format = sc->info.format;
    const bool half = format == VK_FORMAT_R16G16B16A16_SFLOAT;
    const VkDeviceSize texel = half ? 8 : 4;
    if (AspectOf(format) != VK_IMAGE_ASPECT_COLOR_BIT) return false;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = VkDeviceSize(w) * h * texel;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (s->vk.CreateBuffer(s->vk_device, &buffer_info, nullptr, &buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    s->vk.GetBufferMemoryRequirements(s->vk_device, buffer, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = MemoryType(s, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (alloc.memoryTypeIndex == UINT32_MAX || s->vk.AllocateMemory(s->vk_device, &alloc, nullptr, &memory) != VK_SUCCESS) {
        s->vk.DestroyBuffer(s->vk_device, buffer, nullptr);
        return false;
    }
    s->vk.BindBufferMemory(s->vk_device, buffer, memory, 0);
    const VkImage image = sc->images[index].image;
    const bool ok = RunCommands(s, [&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
        s->vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        region.imageExtent = {w, h, 1};
        s->vk.CmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        s->vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    });
    bool written = false;
    void* mapped = nullptr;
    if (ok && s->vk.MapMemory(s->vk_device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
        const bool bgra = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
        const bool linear = format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM || half;
        const int x0 = rect ? std::clamp(rect->offset.x, 0, static_cast<int>(w)) : 0;
        const int y0 = rect ? std::clamp(rect->offset.y, 0, static_cast<int>(h)) : 0;
        const int rw = rect ? std::clamp(rect->extent.width, 0, static_cast<int>(w) - x0) : static_cast<int>(w);
        const int rh = rect ? std::clamp(rect->extent.height, 0, static_cast<int>(h) - y0) : static_cast<int>(h);
        std::vector<uint8_t> rgba(size_t(std::max(rw, 1)) * std::max(rh, 1) * 4);
        auto encode = [](float v) {
            v = std::clamp(v, 0.0f, 1.0f);
            v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
            return static_cast<uint8_t>(std::lround(v * 255.0f));
        };
        for (int y = 0; y < rh; ++y) {
            for (int x = 0; x < rw; ++x) {
                const size_t src = (size_t(y0 + y) * w + (x0 + x));
                uint8_t* dst = &rgba[(size_t(y) * rw + x) * 4];
                if (half) {
                    const uint16_t* p = static_cast<const uint16_t*>(mapped) + src * 4;
                    for (int c = 0; c < 3; ++c) dst[c] = encode(HalfToFloat(p[c]));
                    dst[3] = static_cast<uint8_t>(std::lround(std::clamp(HalfToFloat(p[3]), 0.0f, 1.0f) * 255.0f));
                } else {
                    const uint8_t* p = static_cast<const uint8_t*>(mapped) + src * 4;
                    const uint8_t r = bgra ? p[2] : p[0];
                    const uint8_t b = bgra ? p[0] : p[2];
                    dst[0] = linear ? encode(r / 255.0f) : r;
                    dst[1] = linear ? encode(p[1] / 255.0f) : p[1];
                    dst[2] = linear ? encode(b / 255.0f) : b;
                    dst[3] = p[3];
                }
            }
        }
        written = stbi_write_png(path.c_str(), rw, rh, 4, rgba.data(), rw * 4) != 0;
        s->vk.UnmapMemory(s->vk_device, memory);
    }
    s->vk.DestroyBuffer(s->vk_device, buffer, nullptr);
    s->vk.FreeMemory(s->vk_device, memory, nullptr);
    Log("dump %s %s", path.c_str(), written ? "written" : "FAILED");
    return written;
}

std::string FormatName(int64_t f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
    case VK_FORMAT_D32_SFLOAT: return "D32_SFLOAT";
    default: return std::to_string(f);
    }
}

#define PT_CHECK_STRUCT(ptr, xrtype)                                                                                     \
    do {                                                                                                                 \
        if (!(ptr)) return Violation(XR_ERROR_VALIDATION_FAILURE, "%s: null " #ptr, __func__);                            \
        if ((ptr)->type != (xrtype)) return Violation(XR_ERROR_VALIDATION_FAILURE, "%s: " #ptr "->type is %d, not " #xrtype, __func__, (ptr)->type); \
    } while (0)

XRAPI_ATTR XrResult XRAPI_CALL GetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);

const std::vector<std::pair<const char*, uint32_t>>& Extensions() {
    static const std::vector<std::pair<const char*, uint32_t>> list = {{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, 2}};
    return list;
}

XRAPI_ATTR XrResult XRAPI_CALL EnumerateInstanceExtensionProperties(const char* layer, uint32_t capacity, uint32_t* count, XrExtensionProperties* props) {
    if (layer && *layer) return XR_ERROR_API_LAYER_NOT_PRESENT;
    if (!count) return Violation(XR_ERROR_VALIDATION_FAILURE, "xrEnumerateInstanceExtensionProperties: null count");
    *count = static_cast<uint32_t>(Extensions().size());
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t i = 0; i < *count; ++i) {
        if (props[i].type != XR_TYPE_EXTENSION_PROPERTIES) Violation(XR_ERROR_VALIDATION_FAILURE, "extension properties [%u] type", i);
        std::snprintf(props[i].extensionName, sizeof(props[i].extensionName), "%s", Extensions()[i].first);
        props[i].extensionVersion = Extensions()[i].second;
    }
    return XR_SUCCESS;
}

int64_t ParseFormat(const std::string& name) {
    if (name == "rgba_srgb") return VK_FORMAT_R8G8B8A8_SRGB;
    if (name == "bgra_srgb") return VK_FORMAT_B8G8R8A8_SRGB;
    if (name == "rgba_unorm") return VK_FORMAT_R8G8B8A8_UNORM;
    if (name == "bgra_unorm") return VK_FORMAT_B8G8R8A8_UNORM;
    if (name == "rgba16f") return VK_FORMAT_R16G16B16A16_SFLOAT;
    return 0;
}

XRAPI_ATTR XrResult XRAPI_CALL CreateInstance(const XrInstanceCreateInfo* info, XrInstance* out) {
    std::lock_guard lock(g_mutex);
    PT_CHECK_STRUCT(info, XR_TYPE_INSTANCE_CREATE_INFO);
    if (!out) return Violation(XR_ERROR_VALIDATION_FAILURE, "xrCreateInstance: null instance");
    if (g_instance) return XR_ERROR_LIMIT_REACHED;
    const XrVersion api = info->applicationInfo.apiVersion;
    if (XR_VERSION_MAJOR(api) != 1 || XR_VERSION_MINOR(api) > 1) return XR_ERROR_API_VERSION_UNSUPPORTED;
    if (!info->applicationInfo.applicationName[0]) return Violation(XR_ERROR_NAME_INVALID, "xrCreateInstance: empty application name");
    auto inst = std::make_unique<Instance>();
    inst->magic = kMagicInstance;
    inst->app_name = info->applicationInfo.applicationName;
    inst->api_version = api;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        const std::string name = info->enabledExtensionNames[i];
        if (std::none_of(Extensions().begin(), Extensions().end(), [&](const auto& e) { return name == e.first; })) {
            Log("xrCreateInstance: extension %s not supported", name.c_str());
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
        inst->extensions.insert(name);
    }
    inst->script = LoadScript();
    if (const char* view = std::getenv("PT_XRTEST_VIEW")) {
        unsigned w = 0, h = 0;
        if (std::sscanf(view, "%ux%u", &w, &h) == 2 && w >= 16 && h >= 16 && w <= 4096 && h <= 4096) {
            inst->view_w = w;
            inst->view_h = h;
        }
    }
    if (const char* ipd = std::getenv("PT_XRTEST_IPD")) inst->ipd = static_cast<float>(std::atof(ipd));
    if (const char* formats = std::getenv("PT_XRTEST_FORMATS")) {
        for (const std::string& f : Split(formats, ", "))
            if (const int64_t v = ParseFormat(f)) inst->formats.push_back(v);
    }
    if (inst->formats.empty()) {
        inst->formats = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
                         VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT};
    }
    if (const char* out_dir = std::getenv("PT_XRTEST_OUT")) {
        inst->out_dir = out_dir;
        std::error_code ec;
        std::filesystem::create_directories(inst->out_dir, ec);
    }
    Log("instance created for '%s' (api %u.%u), view %ux%u, ipd %.4f, %zu script entries", inst->app_name.c_str(), XR_VERSION_MAJOR(api),
        XR_VERSION_MINOR(api), inst->view_w, inst->view_h, inst->ipd,
        inst->script.head.size() + inst->script.inputs.size() + inst->script.events.size() + inst->script.dumps.size());
    g_instance = inst.release();
    *out = reinterpret_cast<XrInstance>(g_instance);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL DestroyInstance(XrInstance h) {
    std::lock_guard lock(g_mutex);
    Instance* inst = GetInstance(h);
    if (!inst) return Violation(XR_ERROR_HANDLE_INVALID, "xrDestroyInstance: invalid instance");
    if (!inst->sessions.empty()) Violation(XR_SUCCESS, "xrDestroyInstance with %zu sessions left (destroyed with it)", inst->sessions.size());
    for (Session* s : inst->sessions) {
        Log("summary: frames waited %lld begun %lld ended %lld, projection layers %lld, quad layers %lld", static_cast<long long>(s->waited),
            static_cast<long long>(s->begun), static_cast<long long>(s->ended), static_cast<long long>(s->projection_layers),
            static_cast<long long>(s->quad_layers));
    }
    for (ActionSet* set : inst->action_sets) {
        for (Action* a : set->actions) delete a;
        delete set;
    }
    Log("instance destroyed, %d violations", g_violations);
    delete inst;
    g_instance = nullptr;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL GetInstanceProperties(XrInstance h, XrInstanceProperties* props) {
    if (!GetInstance(h)) return Violation(XR_ERROR_HANDLE_INVALID, "xrGetInstanceProperties: invalid instance");
    PT_CHECK_STRUCT(props, XR_TYPE_INSTANCE_PROPERTIES);
    props->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    std::snprintf(props->runtimeName, sizeof(props->runtimeName), "pt-port headless test runtime");
    return XR_SUCCESS;
}

void ScriptEvents(Session* s, int frame);

XRAPI_ATTR XrResult XRAPI_CALL PollEvent(XrInstance h, XrEventDataBuffer* buffer) {
    std::lock_guard lock(g_mutex);
    Instance* inst = GetInstance(h);
    if (!inst) return Violation(XR_ERROR_HANDLE_INVALID, "xrPollEvent: invalid instance");
    PT_CHECK_STRUCT(buffer, XR_TYPE_EVENT_DATA_BUFFER);
    if (inst->events.empty()) return XR_EVENT_UNAVAILABLE;
    const std::vector<uint8_t> e = std::move(inst->events.front());
    inst->events.pop_front();
    std::memcpy(buffer, e.data(), std::min(e.size(), sizeof(XrEventDataBuffer)));
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL ResultToString(XrInstance h, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE]) {
    if (!GetInstance(h)) return XR_ERROR_HANDLE_INVALID;
    switch (value) {
#define PT_RESULT_NAME(name, v) \
    case name: std::snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "%s", #name); return XR_SUCCESS;
        XR_LIST_ENUM_XrResult(PT_RESULT_NAME)
#undef PT_RESULT_NAME
    default: std::snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "%s_%d", value < 0 ? "XR_UNKNOWN_FAILURE" : "XR_UNKNOWN_SUCCESS", static_cast<int>(value));
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL StructureTypeToString(XrInstance h, XrStructureType value, char buffer[XR_MAX_STRUCTURE_NAME_SIZE]) {
    if (!GetInstance(h)) return XR_ERROR_HANDLE_INVALID;
    switch (value) {
#define PT_TYPE_NAME(name, v) \
    case name: std::snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "%s", #name); return XR_SUCCESS;
        XR_LIST_ENUM_XrStructureType(PT_TYPE_NAME)
#undef PT_TYPE_NAME
    default: std::snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "XR_UNKNOWN_STRUCTURE_TYPE_%d", static_cast<int>(value));
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL GetSystem(XrInstance h, const XrSystemGetInfo* info, XrSystemId* id) {
    Instance* inst = GetInstance(h);
    if (!inst) return Violation(XR_ERROR_HANDLE_INVALID, "xrGetSystem: invalid instance");
    PT_CHECK_STRUCT(info, XR_TYPE_SYSTEM_GET_INFO);
    if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
        return info->formFactor == XR_FORM_FACTOR_HANDHELD_DISPLAY ? XR_ERROR_FORM_FACTOR_UNSUPPORTED : XR_ERROR_VALIDATION_FAILURE;
    }
    inst->system_got = true;
    *id = kSystemId;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL GetSystemProperties(XrInstance h, XrSystemId id, XrSystemProperties* props) {
    if (!GetInstance(h)) return Violation(XR_ERROR_HANDLE_INVALID, "xrGetSystemProperties: invalid instance");
    if (id != kSystemId) return Violation(XR_ERROR_SYSTEM_INVALID, "xrGetSystemProperties: system id");
    PT_CHECK_STRUCT(props, XR_TYPE_SYSTEM_PROPERTIES);
    props->systemId = kSystemId;
    props->vendorId = 0;
    std::snprintf(props->systemName, sizeof(props->systemName), "pt-port simulated headset");
    props->graphicsProperties = {4096, 4096, 16};
    props->trackingProperties = {XR_TRUE, XR_TRUE};
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL EnumerateEnvironmentBlendModes(XrInstance h, XrSystemId id, XrViewConfigurationType type, uint32_t capacity,
                                                              uint32_t* count, XrEnvironmentBlendMode* modes) {
    if (!GetInstance(h)) return Violation(XR_ERROR_HANDLE_INVALID, "xrEnumerateEnvironmentBlendModes: invalid instance");
    if (id != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    *count = 1;
    if (capacity == 0) return XR_SUCCESS;
    modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL EnumerateViewConfigurations(XrInstance h, XrSystemId id, uint32_t capacity, uint32_t* count,
                                                           XrViewConfigurationType* types) {
    if (!GetInstance(h)) return Violation(XR_ERROR_HANDLE_INVALID, "xrEnumerateViewConfigurations: invalid instance");
    if (id != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    *count = 1;
    if (capacity == 0) return XR_SUCCESS;
    types[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL GetViewConfigurationProperties(XrInstance h, XrSystemId id, XrViewConfigurationType type,
                                                              XrViewConfigurationProperties* props) {
    if (!GetInstance(h)) return XR_ERROR_HANDLE_INVALID;
    if (id != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    PT_CHECK_STRUCT(props, XR_TYPE_VIEW_CONFIGURATION_PROPERTIES);
    props->viewConfigurationType = type;
    props->fovMutable = XR_FALSE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL EnumerateViewConfigurationViews(XrInstance h, XrSystemId id, XrViewConfigurationType type, uint32_t capacity,
                                                               uint32_t* count, XrViewConfigurationView* views) {
    Instance* inst = GetInstance(h);
    if (!inst) return Violation(XR_ERROR_HANDLE_INVALID, "xrEnumerateViewConfigurationViews: invalid instance");
    if (id != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    *count = 2;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    for (int i = 0; i < 2; ++i) {
        if (views[i].type != XR_TYPE_VIEW_CONFIGURATION_VIEW) Violation(XR_ERROR_VALIDATION_FAILURE, "view configuration view [%d] type", i);
        views[i].recommendedImageRectWidth = inst->view_w;
        views[i].recommendedImageRectHeight = inst->view_h;
        views[i].maxImageRectWidth = 4096;
        views[i].maxImageRectHeight = 4096;
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount = 4;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL GetVulkanGraphicsRequirements2(XrInstance h, XrSystemId id, XrGraphicsRequirementsVulkanKHR* req) {
    Instance* inst = GetInstance(h);
    if (!inst) return Violation(XR_ERROR_HANDLE_INVALID, "xrGetVulkanGraphicsRequirements2KHR: invalid instance");
    if (!inst->extensions.count(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) return Violation(XR_ERROR_FUNCTION_UNSUPPORTED, "vulkan_enable2 not enabled");
    if (id != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    PT_CHECK_STRUCT(req, XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR);
    req->minApiVersionSupported = XR_MAKE_VERSION(1, 1, 0);
    req->maxApiVersionSupported = XR_MAKE_VERSION(1, 4, 0);
    inst->requirements_checked = true;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL CreateVulkanInstance(XrInstance h, const XrVulkanInstanceCreateInfoKHR* info, VkInstance* out, VkResult* vk_result) {
    Instance* inst = GetInstance(h);
    if (!inst) return Violation(XR_ERROR_HANDLE_INVALID, "xrCreateVulkanInstanceKHR: invalid instance");
    PT_CHECK_STRUCT(info, XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR);
    if (info->systemId != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (!inst->requirements_checked) Violation(XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING, "xrCreateVulkanInstanceKHR before xrGetVulkanGraphicsRequirements2KHR");
    if (!info->pfnGetInstanceProcAddr || !info->vulkanCreateInfo) return Violation(XR_ERROR_VALIDATION_FAILURE, "xrCreateVulkanInstanceKHR: null pointers");
    auto create = reinterpret_cast<PFN_vkCreateInstance>(info->pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) return XR_ERROR_RUNTIME_FAILURE;
    *vk_result = create(info->vulkanCreateInfo, info->vulkanAllocator, out);
    if (*vk_result == VK_SUCCESS) {
        inst->created_vk_instance = *out;
        inst->vk_gipa = info->pfnGetInstanceProcAddr;
    }
    Log("vkCreateInstance through the runtime: %d (%u extensions, %u layers)", *vk_result, info->vulkanCreateInfo->enabledExtensionCount,
        info->vulkanCreateInfo->enabledLayerCount);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL GetVulkanGraphicsDevice2(XrInstance h, const XrVulkanGraphicsDeviceGetInfoKHR* info, VkPhysicalDevice* out) {
    Instance* inst = GetInstance(h);
    if (!inst) return Violation(XR_ERROR_HANDLE_INVALID, "xrGetVulkanGraphicsDevice2KHR: invalid instance");
    PT_CHECK_STRUCT(info, XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR);
    if (info->systemId != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (!inst->vk_gipa || info->vulkanInstance != inst->created_vk_instance) {
        Violation(XR_ERROR_VALIDATION_FAILURE, "xrGetVulkanGraphicsDevice2KHR: VkInstance not created with xrCreateVulkanInstanceKHR");
        if (!inst->vk_gipa) return XR_ERROR_RUNTIME_FAILURE;
    }
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(inst->vk_gipa(info->vulkanInstance, "vkEnumeratePhysicalDevices"));
    auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(inst->vk_gipa(info->vulkanInstance, "vkGetPhysicalDeviceProperties"));
    uint32_t count = 0;
    enumerate(info->vulkanInstance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    enumerate(info->vulkanInstance, &count, devices.data());
    VkPhysicalDevice chosen = devices.empty() ? VK_NULL_HANDLE : devices[0];
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties p{};
        properties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            chosen = d;
            break;
        }
    }
    if (const char* gpu = std::getenv("PT_XRTEST_GPU")) {
        const uint32_t i = static_cast<uint32_t>(std::atoi(gpu));
        if (i < count) chosen = devices[i];
    }
    if (!chosen) return XR_ERROR_RUNTIME_FAILURE;
    VkPhysicalDeviceProperties p{};
    properties(chosen, &p);
    Log("headset GPU: %s", p.deviceName);
    *out = chosen;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL CreateVulkanDevice(XrInstance h, const XrVulkanDeviceCreateInfoKHR* info, VkDevice* out, VkResult* vk_result) {
    Instance* inst = GetInstance(h);
    if (!inst) return Violation(XR_ERROR_HANDLE_INVALID, "xrCreateVulkanDeviceKHR: invalid instance");
    PT_CHECK_STRUCT(info, XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR);
    if (info->systemId != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (!info->pfnGetInstanceProcAddr || !info->vulkanCreateInfo || !info->vulkanPhysicalDevice) {
        return Violation(XR_ERROR_VALIDATION_FAILURE, "xrCreateVulkanDeviceKHR: null pointers");
    }
    auto create = reinterpret_cast<PFN_vkCreateDevice>(info->pfnGetInstanceProcAddr(inst->created_vk_instance, "vkCreateDevice"));
    if (!create) return XR_ERROR_RUNTIME_FAILURE;
    *vk_result = create(info->vulkanPhysicalDevice, info->vulkanCreateInfo, info->vulkanAllocator, out);
    Log("vkCreateDevice through the runtime: %d (%u extensions, %u queue families)", *vk_result, info->vulkanCreateInfo->enabledExtensionCount,
        info->vulkanCreateInfo->queueCreateInfoCount);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL CreateSession(XrInstance h, const XrSessionCreateInfo* info, XrSession* out) {
    std::lock_guard lock(g_mutex);
    Instance* inst = GetInstance(h);
    if (!inst) return Violation(XR_ERROR_HANDLE_INVALID, "xrCreateSession: invalid instance");
    PT_CHECK_STRUCT(info, XR_TYPE_SESSION_CREATE_INFO);
    if (info->systemId != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (!inst->requirements_checked) return Violation(XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING, "xrCreateSession before the graphics requirements");
    const XrGraphicsBindingVulkanKHR* binding = nullptr;
    for (auto* n = static_cast<const XrBaseInStructure*>(info->next); n; n = n->next) {
        if (n->type == XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR) binding = reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(n);
    }
    if (!binding) return Violation(XR_ERROR_GRAPHICS_DEVICE_INVALID, "xrCreateSession without a Vulkan graphics binding");
    if (!inst->sessions.empty()) return XR_ERROR_LIMIT_REACHED;
    if (binding->instance != inst->created_vk_instance) Violation(XR_ERROR_GRAPHICS_DEVICE_INVALID, "xrCreateSession: VkInstance not the one created through the runtime");
    auto s = std::make_unique<Session>();
    s->magic = kMagicSession;
    s->instance = inst;
    s->vk_instance = binding->instance;
    s->vk_physical = binding->physicalDevice;
    s->vk_device = binding->device;
    s->queue_family = binding->queueFamilyIndex;
    s->queue_index = binding->queueIndex;
    if (!LoadDeviceFunctions(s.get(), inst->vk_gipa)) return XR_ERROR_RUNTIME_FAILURE;
    s->vk.GetDeviceQueue(s->vk_device, s->queue_family, s->queue_index, &s->queue);
    if (!s->queue) return Violation(XR_ERROR_GRAPHICS_DEVICE_INVALID, "xrCreateSession: no queue %u.%u", s->queue_family, s->queue_index);
    Session* raw = s.release();
    inst->sessions.insert(raw);
    *out = reinterpret_cast<XrSession>(raw);
    Log("session created on queue family %u index %u", raw->queue_family, raw->queue_index);
    SetState(raw, XR_SESSION_STATE_IDLE);
    SetState(raw, XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL DestroySession(XrSession h) {
    std::lock_guard lock(g_mutex);
    Session* s = GetSession(h);
    if (!s) return Violation(XR_ERROR_HANDLE_INVALID, "xrDestroySession: invalid session");
    if (s->running) Violation(XR_SUCCESS, "xrDestroySession while running (xrEndSession not called)");
    Log("summary: frames waited %lld begun %lld ended %lld, projection layers %lld, quad layers %lld", static_cast<long long>(s->waited),
        static_cast<long long>(s->begun), static_cast<long long>(s->ended), static_cast<long long>(s->projection_layers),
        static_cast<long long>(s->quad_layers));
    if (s->vk.DeviceWaitIdle) s->vk.DeviceWaitIdle(s->vk_device);
    for (Swapchain* sc : s->swapchains) {
        for (SwapchainImage& i : sc->images) {
            s->vk.DestroyImage(s->vk_device, i.image, nullptr);
            s->vk.FreeMemory(s->vk_device, i.memory, nullptr);
        }
        sc->magic = 0;
        delete sc;
    }
    for (Space* sp : s->spaces) {
        sp->magic = 0;
        delete sp;
    }
    if (s->pool) s->vk.DestroyCommandPool(s->vk_device, s->pool, nullptr);
    s->instance->sessions.erase(s);
    s->magic = 0;
    delete s;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL BeginSession(XrSession h, const XrSessionBeginInfo* info) {
    std::lock_guard lock(g_mutex);
    Session* s = GetSession(h);
    if (!s) return Violation(XR_ERROR_HANDLE_INVALID, "xrBeginSession: invalid session");
    PT_CHECK_STRUCT(info, XR_TYPE_SESSION_BEGIN_INFO);
    if (s->running) return XR_ERROR_SESSION_RUNNING;
    if (s->state != XR_SESSION_STATE_READY) return Violation(XR_ERROR_SESSION_NOT_READY, "xrBeginSession in state %s", StateName(s->state));
    if (info->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    s->running = true;
    Log("session begun");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL EndSession(XrSession h) {
    std::lock_guard lock(g_mutex);
    Session* s = GetSession(h);
    if (!s) return Violation(XR_ERROR_HANDLE_INVALID, "xrEndSession: invalid session");
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    if (s->state != XR_SESSION_STATE_STOPPING) return Violation(XR_ERROR_SESSION_NOT_STOPPING, "xrEndSession in state %s", StateName(s->state));
    s->running = false;
    SetState(s, XR_SESSION_STATE_IDLE);
    if (s->exit_requested) SetState(s, XR_SESSION_STATE_EXITING);
    Log("session ended");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL RequestExitSession(XrSession h) {
    std::lock_guard lock(g_mutex);
    Session* s = GetSession(h);
    if (!s) return Violation(XR_ERROR_HANDLE_INVALID, "xrRequestExitSession: invalid session");
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    s->exit_requested = true;
    if (s->state == XR_SESSION_STATE_FOCUSED) SetState(s, XR_SESSION_STATE_VISIBLE);
    if (s->state == XR_SESSION_STATE_VISIBLE) SetState(s, XR_SESSION_STATE_SYNCHRONIZED);
    SetState(s, XR_SESSION_STATE_STOPPING);
    return XR_SUCCESS;
}

void ScriptEvents(Session* s, int frame) {
    auto range = s->instance->script.events.equal_range(frame);
    for (auto it = range.first; it != range.second; ++it) {
        const std::string& e = it->second;
        Log("script event %s at frame %d", e.c_str(), frame);
        if (e == "focus_lost" && s->state == XR_SESSION_STATE_FOCUSED) {
            SetState(s, XR_SESSION_STATE_VISIBLE);
        } else if (e == "focus_gained" && s->state == XR_SESSION_STATE_VISIBLE) {
            SetState(s, XR_SESSION_STATE_FOCUSED);
        } else if (e == "visible" && s->state == XR_SESSION_STATE_FOCUSED) {
            SetState(s, XR_SESSION_STATE_VISIBLE);
        } else if (e == "exit" || e == "stop") {
            s->exit_requested = e == "exit";
            if (s->state == XR_SESSION_STATE_FOCUSED) SetState(s, XR_SESSION_STATE_VISIBLE);
            if (s->state == XR_SESSION_STATE_VISIBLE) SetState(s, XR_SESSION_STATE_SYNCHRONIZED);
            SetState(s, XR_SESSION_STATE_STOPPING);
        } else if (e == "profile_none") {
            s->profile = XR_NULL_PATH;
            XrEventDataInteractionProfileChanged ev{XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED};
            ev.session = reinterpret_cast<XrSession>(s);
            PushEvent(s->instance, &ev, sizeof(ev));
        }
    }
}

XRAPI_ATTR XrResult XRAPI_CALL WaitFrame(XrSession h, const XrFrameWaitInfo* info, XrFrameState* state) {
    std::lock_guard lock(g_mutex);
    Session* s = GetSession(h);
    if (!s) return Violation(XR_ERROR_HANDLE_INVALID, "xrWaitFrame: invalid session");
    if (info && info->type != XR_TYPE_FRAME_WAIT_INFO) return Violation(XR_ERROR_VALIDATION_FAILURE, "xrWaitFrame: info type");
    PT_CHECK_STRUCT(state, XR_TYPE_FRAME_STATE);
    if (!s->running) return Violation(XR_ERROR_SESSION_NOT_RUNNING, "xrWaitFrame on a session that is not running");
    if (s->waited > s->begun) return Violation(XR_ERROR_CALL_ORDER_INVALID, "xrWaitFrame twice without xrBeginFrame");
    const int frame = static_cast<int>(s->waited);
    ScriptEvents(s, frame);
    ++s->waited;
    s->last_predicted = kStartTime + kPeriod * s->waited;
    s->predicted.insert(s->last_predicted);
    state->predictedDisplayTime = s->last_predicted;
    state->predictedDisplayPeriod = kPeriod;
    state->shouldRender = (s->state == XR_SESSION_STATE_VISIBLE || s->state == XR_SESSION_STATE_FOCUSED) ? XR_TRUE : XR_FALSE;
    if (s->state == XR_SESSION_STATE_READY && s->waited >= 1) SetState(s, XR_SESSION_STATE_SYNCHRONIZED);
    if (s->state == XR_SESSION_STATE_SYNCHRONIZED && s->waited >= 3 && !s->exit_requested) {
        SetState(s, XR_SESSION_STATE_VISIBLE);
        SetState(s, XR_SESSION_STATE_FOCUSED);
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL BeginFrame(XrSession h, const XrFrameBeginInfo* info) {
    std::lock_guard lock(g_mutex);
    Session* s = GetSession(h);
    if (!s) return Violation(XR_ERROR_HANDLE_INVALID, "xrBeginFrame: invalid session");
    if (info && info->type != XR_TYPE_FRAME_BEGIN_INFO) return Violation(XR_ERROR_VALIDATION_FAILURE, "xrBeginFrame: info type");
    if (!s->running) return Violation(XR_ERROR_SESSION_NOT_RUNNING, "xrBeginFrame on a session that is not running");
    if (s->begun >= s->waited) return Violation(XR_ERROR_CALL_ORDER_INVALID, "xrBeginFrame without a matching xrWaitFrame");
    XrResult result = XR_SUCCESS;
    if (s->frame_begun) {
        result = XR_FRAME_DISCARDED;
        Log("frame %lld discarded (xrBeginFrame again without xrEndFrame)", static_cast<long long>(s->begun));
    }
    s->frame_begun = true;
    ++s->begun;
    return result;
}

bool WantDump(Instance* inst, int frame) {
    if (inst->out_dir.empty()) return false;
    return inst->script.dumps.count(frame) || (inst->script.dump_every > 0 && frame % inst->script.dump_every == 0);
}

XrResult CheckSubImage(Session* s, const XrSwapchainSubImage& sub, const char* what, Swapchain** out) {
    Swapchain* sc = GetSwapchain(sub.swapchain);
    if (!sc || sc->session != s) return Violation(XR_ERROR_HANDLE_INVALID, "%s: invalid swapchain", what);
    if (sc->released_count == 0) return Violation(XR_ERROR_LAYER_INVALID, "%s: swapchain never released", what);
    if (sub.imageArrayIndex >= sc->info.arraySize) return Violation(XR_ERROR_VALIDATION_FAILURE, "%s: array index %u of %u", what, sub.imageArrayIndex, sc->info.arraySize);
    const XrRect2Di& r = sub.imageRect;
    if (r.offset.x < 0 || r.offset.y < 0 || r.extent.width <= 0 || r.extent.height <= 0 ||
        r.offset.x + r.extent.width > static_cast<int32_t>(sc->info.width) || r.offset.y + r.extent.height > static_cast<int32_t>(sc->info.height)) {
        return Violation(XR_ERROR_SWAPCHAIN_RECT_INVALID, "%s: image rect %d,%d %dx%d outside %ux%u", what, r.offset.x, r.offset.y, r.extent.width,
                         r.extent.height, sc->info.width, sc->info.height);
    }
    *out = sc;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL EndFrame(XrSession h, const XrFrameEndInfo* info) {
    std::lock_guard lock(g_mutex);
    Session* s = GetSession(h);
    if (!s) return Violation(XR_ERROR_HANDLE_INVALID, "xrEndFrame: invalid session");
    PT_CHECK_STRUCT(info, XR_TYPE_FRAME_END_INFO);
    if (!s->running) return Violation(XR_ERROR_SESSION_NOT_RUNNING, "xrEndFrame on a session that is not running");
    if (!s->frame_begun) return Violation(XR_ERROR_CALL_ORDER_INVALID, "xrEndFrame without xrBeginFrame");
    if (!s->predicted.count(info->displayTime)) return Violation(XR_ERROR_TIME_INVALID, "xrEndFrame: display time %lld was not predicted", static_cast<long long>(info->displayTime));
    if (info->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE) return Violation(XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED, "xrEndFrame: blend mode %d", info->environmentBlendMode);
    if (info->layerCount > 16) return Violation(XR_ERROR_LAYER_LIMIT_EXCEEDED, "xrEndFrame: %u layers", info->layerCount);
    for (Swapchain* sc : s->swapchains) {
        if (!sc->acquired.empty()) Log("note: swapchain %p has %zu images still acquired at xrEndFrame", static_cast<void*>(sc), sc->acquired.size());
    }
    const int frame = static_cast<int>((info->displayTime - kStartTime) / kPeriod) - 1;
    const bool dump = WantDump(s->instance, frame);
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        const XrCompositionLayerBaseHeader* layer = info->layers[i];
        if (!layer) return Violation(XR_ERROR_LAYER_INVALID, "xrEndFrame: layer %u is null", i);
        Space* space = GetSpace(layer->space);
        if (!space || space->session != s) return Violation(XR_ERROR_HANDLE_INVALID, "xrEndFrame: layer %u space invalid", i);
        const XrCompositionLayerFlags known = XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT | XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                              XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT | XR_COMPOSITION_LAYER_INVERTED_ALPHA_BIT_EXT;
        if (layer->layerFlags & ~known) return Violation(XR_ERROR_VALIDATION_FAILURE, "xrEndFrame: layer %u flags %llx", i, static_cast<unsigned long long>(layer->layerFlags));
        if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            auto* p = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
            if (p->viewCount != 2) return Violation(XR_ERROR_VALIDATION_FAILURE, "projection layer with %u views", p->viewCount);
            for (uint32_t v = 0; v < 2; ++v) {
                const XrCompositionLayerProjectionView& view = p->views[v];
                if (view.type != XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW) return Violation(XR_ERROR_VALIDATION_FAILURE, "projection view %u type", v);
                if (!Normalized(view.pose.orientation)) return Violation(XR_ERROR_POSE_INVALID, "projection view %u orientation not normalized", v);
                const XrFovf& f = view.fov;
                if (!(f.angleLeft < f.angleRight && f.angleDown < f.angleUp)) return Violation(XR_ERROR_VALIDATION_FAILURE, "projection view %u fov", v);
                Swapchain* sc = nullptr;
                if (const XrResult r = CheckSubImage(s, view.subImage, "projection view", &sc); r != XR_SUCCESS) return r;
                if (dump) {
                    char name[64];
                    std::snprintf(name, sizeof(name), "f%05d_layer%u_view%u.png", frame, i, v);
                    DumpImage(s, sc, static_cast<uint32_t>(sc->last_released), view.subImage.imageArrayIndex, (std::filesystem::path(s->instance->out_dir) / name).string(),
                              &view.subImage.imageRect);
                    Log("dump layer %u view %u: pose (%.4f %.4f %.4f) q (%.4f %.4f %.4f %.4f) fov (%.4f %.4f %.4f %.4f) rect %d,%d %dx%d format %s",
                        i, v, view.pose.position.x, view.pose.position.y, view.pose.position.z, view.pose.orientation.x, view.pose.orientation.y,
                        view.pose.orientation.z, view.pose.orientation.w, f.angleLeft, f.angleRight, f.angleUp, f.angleDown,
                        view.subImage.imageRect.offset.x, view.subImage.imageRect.offset.y, view.subImage.imageRect.extent.width,
                        view.subImage.imageRect.extent.height, FormatName(sc->info.format).c_str());
                }
            }
            ++s->projection_layers;
        } else if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            auto* q = reinterpret_cast<const XrCompositionLayerQuad*>(layer);
            if (!Normalized(q->pose.orientation)) return Violation(XR_ERROR_POSE_INVALID, "quad layer %u orientation not normalized", i);
            if (!(q->size.width > 0.0f && q->size.height > 0.0f)) return Violation(XR_ERROR_VALIDATION_FAILURE, "quad layer %u size", i);
            if (q->eyeVisibility > XR_EYE_VISIBILITY_RIGHT) return Violation(XR_ERROR_VALIDATION_FAILURE, "quad layer %u eye visibility", i);
            Swapchain* sc = nullptr;
            if (const XrResult r = CheckSubImage(s, q->subImage, "quad layer", &sc); r != XR_SUCCESS) return r;
            if (dump) {
                char name[64];
                std::snprintf(name, sizeof(name), "f%05d_layer%u_quad.png", frame, i);
                DumpImage(s, sc, static_cast<uint32_t>(sc->last_released), q->subImage.imageArrayIndex, (std::filesystem::path(s->instance->out_dir) / name).string(),
                          &q->subImage.imageRect);
                Log("dump layer %u quad: pose (%.4f %.4f %.4f) q (%.4f %.4f %.4f %.4f) size %.3f x %.3f flags %llx space %s", i, q->pose.position.x,
                    q->pose.position.y, q->pose.position.z, q->pose.orientation.x, q->pose.orientation.y, q->pose.orientation.z, q->pose.orientation.w,
                    q->size.width, q->size.height, static_cast<unsigned long long>(q->layerFlags),
                    space->action ? "action" : space->type == XR_REFERENCE_SPACE_TYPE_VIEW ? "view" : space->type == XR_REFERENCE_SPACE_TYPE_LOCAL ? "local" : "stage");
            }
            ++s->quad_layers;
        } else {
            return Violation(XR_ERROR_LAYER_INVALID, "xrEndFrame: layer %u type %d not supported", i, layer->type);
        }
    }
    if (dump) Log("frame %d: %u layers", frame, info->layerCount);
    s->frames_without_layers = info->layerCount == 0 ? s->frames_without_layers + 1 : 0;
    s->predicted.erase(info->displayTime);
    s->predicted.erase(s->predicted.begin(), s->predicted.lower_bound(info->displayTime));
    s->frame_begun = false;
    ++s->ended;
    for (Swapchain* sc : s->swapchains) sc->released_since_end = false;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL LocateViews(XrSession h, const XrViewLocateInfo* info, XrViewState* state, uint32_t capacity, uint32_t* count, XrView* views) {
    std::lock_guard lock(g_mutex);
    Session* s = GetSession(h);
    if (!s) return Violation(XR_ERROR_HANDLE_INVALID, "xrLocateViews: invalid session");
    PT_CHECK_STRUCT(info, XR_TYPE_VIEW_LOCATE_INFO);
    PT_CHECK_STRUCT(state, XR_TYPE_VIEW_STATE);
    if (info->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    if (info->displayTime <= 0) return Violation(XR_ERROR_TIME_INVALID, "xrLocateViews: time %lld", static_cast<long long>(info->displayTime));
    Space* base = GetSpace(info->space);
    if (!base || base->session != s) return Violation(XR_ERROR_HANDLE_INVALID, "xrLocateViews: invalid space");
    *count = 2;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    const int frame = static_cast<int>((info->displayTime - kStartTime) / kPeriod) - 1;
    Pose base_pose;
    if (!SpaceInLocal(base, std::max(0, frame), base_pose)) {
        state->viewStateFlags = 0;
        return XR_SUCCESS;
    }
    const Pose head = HeadPose(s->instance, std::max(0, frame));
    const XrFovf left_fov{-0.9425f, 0.7330f, 0.8203f, -0.8901f};
    for (uint32_t v = 0; v < 2; ++v) {
        if (views[v].type != XR_TYPE_VIEW) Violation(XR_ERROR_VALIDATION_FAILURE, "xrLocateViews: views[%u].type", v);
        const Pose eye_in_head{Quat{}, {(v == 0 ? -0.5f : 0.5f) * s->instance->ipd, 0.0f, 0.0f}};
        const Pose eye = Compose(Inverse(base_pose), Compose(head, eye_in_head));
        views[v].pose = ToXr(eye);
        views[v].fov = v == 0 ? left_fov : XrFovf{-left_fov.angleRight, -left_fov.angleLeft, left_fov.angleUp, left_fov.angleDown};
    }
    state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_TRACKED_BIT |
                            XR_VIEW_STATE_POSITION_TRACKED_BIT;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL EnumerateReferenceSpaces(XrSession h, uint32_t capacity, uint32_t* count, XrReferenceSpaceType* spaces) {
    if (!GetSession(h)) return Violation(XR_ERROR_HANDLE_INVALID, "xrEnumerateReferenceSpaces: invalid session");
    static const XrReferenceSpaceType types[] = {XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL, XR_REFERENCE_SPACE_TYPE_STAGE};
    *count = 3;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < 3) return XR_ERROR_SIZE_INSUFFICIENT;
    std::copy(std::begin(types), std::end(types), spaces);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL CreateReferenceSpace(XrSession h, const XrReferenceSpaceCreateInfo* info, XrSpace* out) {
    std::lock_guard lock(g_mutex);
    Session* s = GetSession(h);
    if (!s) return Violation(XR_ERROR_HANDLE_INVALID, "xrCreateReferenceSpace: invalid session");
    PT_CHECK_STRUCT(info, XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
    if (info->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_VIEW && info->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL &&
        info->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_STAGE) {
        return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    }
    if (!Normalized(info->poseInReferenceSpace.orientation)) return Violation(XR_ERROR_POSE_INVALID, "xrCreateReferenceSpace: pose not normalized");
    auto* sp = new Space;
    sp->magic = kMagicSpace;
    sp->session = s;
    sp->type = info->referenceSpaceType;
    sp->offset = FromXr(info->poseInReferenceSpace);
    s->spaces.insert(sp);
    *out = reinterpret_cast<XrSpace>(sp);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL GetReferenceSpaceBoundsRect(XrSession h, XrReferenceSpaceType, XrExtent2Df* bounds) {
    if (!GetSession(h)) return XR_ERROR_HANDLE_INVALID;
    bounds->width = bounds->height = 0.0f;
    return XR_SPACE_BOUNDS_UNAVAILABLE;
}

XRAPI_ATTR XrResult XRAPI_CALL CreateActionSpace(XrSession h, const XrActionSpaceCreateInfo* info, XrSpace* out) {
    std::lock_guard lock(g_mutex);
    Session* s = GetSession(h);
    if (!s) return Violation(XR_ERROR_HANDLE_INVALID, "xrCreateActionSpace: invalid session");
    PT_CHECK_STRUCT(info, XR_TYPE_ACTION_SPACE_CREATE_INFO);
    Action* a = GetAction(info->action);
    if (!a) return Violation(XR_ERROR_HANDLE_INVALID, "xrCreateActionSpace: invalid action");
    if (a->type != XR_ACTION_TYPE_POSE_INPUT) return Violation(XR_ERROR_ACTION_TYPE_MISMATCH, "xrCreateActionSpace: %s is not a pose action", a->name.c_str());
    if (info->subactionPath != XR_NULL_PATH && std::find(a->subactions.begin(), a->subactions.end(), info->subactionPath) == a->subactions.end()) {
        return Violation(XR_ERROR_PATH_UNSUPPORTED, "xrCreateActionSpace: subaction path not declared for %s", a->name.c_str());
    }
    auto* sp = new Space;
    sp->magic = kMagicSpace;
    sp->session = s;
    sp->action = true;
    sp->action_ptr = a;
    sp->subaction = info->subactionPath;
    sp->offset = FromXr(info->poseInActionSpace);
    s->spaces.insert(sp);
    *out = reinterpret_cast<XrSpace>(sp);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL LocateSpace(XrSpace h, XrSpace base_h, XrTime time, XrSpaceLocation* location) {
    std::lock_guard lock(g_mutex);
    Space* sp = GetSpace(h);
    Space* base = GetSpace(base_h);
    if (!sp || !base) return Violation(XR_ERROR_HANDLE_INVALID, "xrLocateSpace: invalid space");
    if (sp->session != base->session) return Violation(XR_ERROR_VALIDATION_FAILURE, "xrLocateSpace: spaces of two sessions");
    PT_CHECK_STRUCT(location, XR_TYPE_SPACE_LOCATION);
    if (time <= 0) return Violation(XR_ERROR_TIME_INVALID, "xrLocateSpace: time %lld", static_cast<long long>(time));
    const int frame = std::max(0, static_cast<int>((time - kStartTime) / kPeriod) - 1);
    Pose a, b;
    if (!SpaceInLocal(sp, frame, a) || !SpaceInLocal(base, frame, b)) {
        location->locationFlags = 0;
        return XR_SUCCESS;
    }
    location->pose = ToXr(Compose(Inverse(b), a));
    location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                              XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL DestroySpace(XrSpace h) {
    std::lock_guard lock(g_mutex);
    Space* sp = GetSpace(h);
    if (!sp) return Violation(XR_ERROR_HANDLE_INVALID, "xrDestroySpace: invalid space");
    sp->session->spaces.erase(sp);
    sp->magic = 0;
    delete sp;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL EnumerateSwapchainFormats(XrSession h, uint32_t capacity, uint32_t* count, int64_t* formats) {
    Session* s = GetSession(h);
    if (!s) return Violation(XR_ERROR_HANDLE_INVALID, "xrEnumerateSwapchainFormats: invalid session");
    *count = static_cast<uint32_t>(s->instance->formats.size());
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
    std::copy(s->instance->formats.begin(), s->instance->formats.end(), formats);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL CreateSwapchain(XrSession h, const XrSwapchainCreateInfo* info, XrSwapchain* out) {
    std::lock_guard lock(g_mutex);
    Session* s = GetSession(h);
    if (!s) return Violation(XR_ERROR_HANDLE_INVALID, "xrCreateSwapchain: invalid session");
    PT_CHECK_STRUCT(info, XR_TYPE_SWAPCHAIN_CREATE_INFO);
    if (std::find(s->instance->formats.begin(), s->instance->formats.end(), info->format) == s->instance->formats.end()) {
        return Violation(XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED, "xrCreateSwapchain: format %lld not offered", static_cast<long long>(info->format));
    }
    if (info->width == 0 || info->height == 0 || info->width > 4096 || info->height > 4096 || info->arraySize == 0 || info->mipCount == 0 ||
        info->sampleCount == 0 || (info->faceCount != 1 && info->faceCount != 6)) {
        return Violation(XR_ERROR_VALIDATION_FAILURE, "xrCreateSwapchain: size %ux%u array %u mips %u samples %u faces %u", info->width, info->height,
                         info->arraySize, info->mipCount, info->sampleCount, info->faceCount);
    }
    if (info->faceCount != 1 || info->sampleCount != 1) return XR_ERROR_FEATURE_UNSUPPORTED;
    const bool depth = AspectOf(info->format) != VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (depth && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return Violation(XR_ERROR_VALIDATION_FAILURE, "colour usage on a depth format");
    auto sc = std::make_unique<Swapchain>();
    sc->magic = kMagicSwapchain;
    sc->session = s;
    sc->info = *info;
    sc->info.next = nullptr;
    sc->images.resize(3);
    for (SwapchainImage& image : sc->images) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.flags = (info->usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = static_cast<VkFormat>(info->format);
        ci.extent = {info->width, info->height, 1};
        ci.mipLevels = info->mipCount;
        ci.arrayLayers = info->arraySize;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (s->vk.CreateImage(s->vk_device, &ci, nullptr, &image.image) != VK_SUCCESS) return XR_ERROR_RUNTIME_FAILURE;
        VkMemoryRequirements req{};
        s->vk.GetImageMemoryRequirements(s->vk_device, image.image, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = MemoryType(s, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (alloc.memoryTypeIndex == UINT32_MAX || s->vk.AllocateMemory(s->vk_device, &alloc, nullptr, &image.memory) != VK_SUCCESS) {
            return XR_ERROR_RUNTIME_FAILURE;
        }
        s->vk.BindImageMemory(s->vk_device, image.image, image.memory, 0);
    }
    RunCommands(s, [&](VkCommandBuffer cmd) {
        for (SwapchainImage& image : sc->images) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.dstAccessMask = depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image.image;
            barrier.subresourceRange = {AspectOf(info->format), 0, info->mipCount, 0, info->arraySize};
            s->vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
    });
    Swapchain* raw = sc.release();
    s->swapchains.insert(raw);
    *out = reinterpret_cast<XrSwapchain>(raw);
    Log("swapchain %p: %ux%u %s, array %u, usage %llx", static_cast<void*>(raw), info->width, info->height, FormatName(info->format).c_str(),
        info->arraySize, static_cast<unsigned long long>(info->usageFlags));
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL DestroySwapchain(XrSwapchain h) {
    std::lock_guard lock(g_mutex);
    Swapchain* sc = GetSwapchain(h);
    if (!sc) return Violation(XR_ERROR_HANDLE_INVALID, "xrDestroySwapchain: invalid swapchain");
    Session* s = sc->session;
    s->vk.DeviceWaitIdle(s->vk_device);
    for (SwapchainImage& i : sc->images) {
        s->vk.DestroyImage(s->vk_device, i.image, nullptr);
        s->vk.FreeMemory(s->vk_device, i.memory, nullptr);
    }
    s->swapchains.erase(sc);
    sc->magic = 0;
    delete sc;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL EnumerateSwapchainImages(XrSwapchain h, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images) {
    Swapchain* sc = GetSwapchain(h);
    if (!sc) return Violation(XR_ERROR_HANDLE_INVALID, "xrEnumerateSwapchainImages: invalid swapchain");
    *count = static_cast<uint32_t>(sc->images.size());
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
    auto* vk_images = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images);
    for (uint32_t i = 0; i < *count; ++i) {
        if (vk_images[i].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR) {
            return Violation(XR_ERROR_VALIDATION_FAILURE, "xrEnumerateSwapchainImages: image %u type %d", i, vk_images[i].type);
        }
        vk_images[i].image = sc->images[i].image;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL AcquireSwapchainImage(XrSwapchain h, const XrSwapchainImageAcquireInfo* info, uint32_t* index) {
    std::lock_guard lock(g_mutex);
    Swapchain* sc = GetSwapchain(h);
    if (!sc) return Violation(XR_ERROR_HANDLE_INVALID, "xrAcquireSwapchainImage: invalid swapchain");
    if (info && info->type != XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO) return Violation(XR_ERROR_VALIDATION_FAILURE, "xrAcquireSwapchainImage: info type");
    if (sc->acquired.size() >= sc->images.size()) return Violation(XR_ERROR_CALL_ORDER_INVALID, "xrAcquireSwapchainImage: every image already acquired");
    *index = sc->next;
    sc->acquired.push_back(sc->next);
    sc->next = (sc->next + 1) % static_cast<uint32_t>(sc->images.size());
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL WaitSwapchainImage(XrSwapchain h, const XrSwapchainImageWaitInfo* info) {
    std::lock_guard lock(g_mutex);
    Swapchain* sc = GetSwapchain(h);
    if (!sc) return Violation(XR_ERROR_HANDLE_INVALID, "xrWaitSwapchainImage: invalid swapchain");
    PT_CHECK_STRUCT(info, XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
    if (sc->acquired.empty()) return Violation(XR_ERROR_CALL_ORDER_INVALID, "xrWaitSwapchainImage without an acquired image");
    if (sc->waited) return Violation(XR_ERROR_CALL_ORDER_INVALID, "xrWaitSwapchainImage twice for one image");
    sc->waited = true;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL ReleaseSwapchainImage(XrSwapchain h, const XrSwapchainImageReleaseInfo* info) {
    std::lock_guard lock(g_mutex);
    Swapchain* sc = GetSwapchain(h);
    if (!sc) return Violation(XR_ERROR_HANDLE_INVALID, "xrReleaseSwapchainImage: invalid swapchain");
    if (info && info->type != XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO) return Violation(XR_ERROR_VALIDATION_FAILURE, "xrReleaseSwapchainImage: info type");
    if (sc->acquired.empty() || !sc->waited) return Violation(XR_ERROR_CALL_ORDER_INVALID, "xrReleaseSwapchainImage without a waited image");
    sc->last_released = static_cast<int>(sc->acquired.front());
    sc->acquired.pop_front();
    sc->waited = false;
    sc->released_since_end = true;
    ++sc->released_count;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL StringToPath(XrInstance h, const char* text, XrPath* out) {
    std::lock_guard lock(g_mutex);
    Instance* inst = GetInstance(h);
    if (!inst) return Violation(XR_ERROR_HANDLE_INVALID, "xrStringToPath: invalid instance");
    const std::string s = text ? text : "";
    if (s.empty() || s[0] != '/' || s.back() == '/' || s.find("//") != std::string::npos) return Violation(XR_ERROR_PATH_FORMAT_INVALID, "path '%s'", s.c_str());
    for (char c : s) {
        if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '/' || c == '_' || c == '-' || c == '.')) {
            return Violation(XR_ERROR_PATH_FORMAT_INVALID, "path '%s' has '%c'", s.c_str(), c);
        }
    }
    *out = PathOf(inst, s);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL PathToString(XrInstance h, XrPath path, uint32_t capacity, uint32_t* count, char* buffer) {
    std::lock_guard lock(g_mutex);
    Instance* inst = GetInstance(h);
    if (!inst) return XR_ERROR_HANDLE_INVALID;
    if (path == XR_NULL_PATH || path >= inst->path_names.size()) return XR_ERROR_PATH_INVALID;
    const std::string& s = inst->path_names[path];
    *count = static_cast<uint32_t>(s.size() + 1);
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
    std::memcpy(buffer, s.c_str(), s.size() + 1);
    return XR_SUCCESS;
}

bool ValidName(const char* name) {
    if (!name || !*name) return false;
    for (const char* c = name; *c; ++c) {
        if (!(std::islower(static_cast<unsigned char>(*c)) || std::isdigit(static_cast<unsigned char>(*c)) || *c == '_' || *c == '-' || *c == '.')) return false;
    }
    return true;
}

XRAPI_ATTR XrResult XRAPI_CALL CreateActionSet(XrInstance h, const XrActionSetCreateInfo* info, XrActionSet* out) {
    std::lock_guard lock(g_mutex);
    Instance* inst = GetInstance(h);
    if (!inst) return Violation(XR_ERROR_HANDLE_INVALID, "xrCreateActionSet: invalid instance");
    PT_CHECK_STRUCT(info, XR_TYPE_ACTION_SET_CREATE_INFO);
    if (!ValidName(info->actionSetName)) return Violation(XR_ERROR_PATH_FORMAT_INVALID, "action set name '%s'", info->actionSetName);
    if (!info->localizedActionSetName[0]) return Violation(XR_ERROR_LOCALIZED_NAME_INVALID, "action set '%s' without a localized name", info->actionSetName);
    for (ActionSet* set : inst->action_sets)
        if (set->name == info->actionSetName) return Violation(XR_ERROR_NAME_DUPLICATED, "action set '%s' twice", info->actionSetName);
    auto* set = new ActionSet;
    set->magic = kMagicActionSet;
    set->instance = inst;
    set->name = info->actionSetName;
    inst->action_sets.insert(set);
    *out = reinterpret_cast<XrActionSet>(set);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL DestroyActionSet(XrActionSet h) {
    std::lock_guard lock(g_mutex);
    ActionSet* set = GetActionSet(h);
    if (!set) return Violation(XR_ERROR_HANDLE_INVALID, "xrDestroyActionSet: invalid action set");
    for (Action* a : set->actions) a->magic = 0;
    set->magic = 0;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL CreateAction(XrActionSet h, const XrActionCreateInfo* info, XrAction* out) {
    std::lock_guard lock(g_mutex);
    ActionSet* set = GetActionSet(h);
    if (!set) return Violation(XR_ERROR_HANDLE_INVALID, "xrCreateAction: invalid action set");
    PT_CHECK_STRUCT(info, XR_TYPE_ACTION_CREATE_INFO);
    if (set->attached) return Violation(XR_ERROR_ACTIONSETS_ALREADY_ATTACHED, "xrCreateAction after attaching");
    if (!ValidName(info->actionName)) return Violation(XR_ERROR_PATH_FORMAT_INVALID, "action name '%s'", info->actionName);
    if (!info->localizedActionName[0]) return Violation(XR_ERROR_LOCALIZED_NAME_INVALID, "action '%s' without a localized name", info->actionName);
    for (Action* a : set->actions)
        if (a->name == info->actionName) return Violation(XR_ERROR_NAME_DUPLICATED, "action '%s' twice", info->actionName);
    auto* a = new Action;
    a->magic = kMagicAction;
    a->set = set;
    a->name = info->actionName;
    a->type = info->actionType;
    for (uint32_t i = 0; i < info->countSubactionPaths; ++i) {
        const std::string sub = NameOf(set->instance, info->subactionPaths[i]);
        if (sub != "/user/hand/left" && sub != "/user/hand/right" && sub != "/user/head" && sub != "/user/gamepad") {
            delete a;
            return Violation(XR_ERROR_PATH_UNSUPPORTED, "action '%s' subaction path '%s'", info->actionName, sub.c_str());
        }
        a->subactions.push_back(info->subactionPaths[i]);
    }
    set->actions.push_back(a);
    *out = reinterpret_cast<XrAction>(a);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL DestroyAction(XrAction h) {
    std::lock_guard lock(g_mutex);
    Action* a = GetAction(h);
    if (!a) return Violation(XR_ERROR_HANDLE_INVALID, "xrDestroyAction: invalid action");
    a->magic = 0;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL SuggestInteractionProfileBindings(XrInstance h, const XrInteractionProfileSuggestedBinding* info) {
    std::lock_guard lock(g_mutex);
    Instance* inst = GetInstance(h);
    if (!inst) return Violation(XR_ERROR_HANDLE_INVALID, "xrSuggestInteractionProfileBindings: invalid instance");
    PT_CHECK_STRUCT(info, XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING);
    const std::string profile_name = NameOf(inst, info->interactionProfile);
    const Profile* profile = FindProfile(profile_name);
    if (!profile) {
        Log("bindings for unknown profile %s ignored", profile_name.c_str());
        return XR_SUCCESS;
    }
    if (info->countSuggestedBindings == 0) return Violation(XR_ERROR_VALIDATION_FAILURE, "no bindings for %s", profile_name.c_str());
    for (ActionSet* set : inst->action_sets)
        if (set->attached) return Violation(XR_ERROR_ACTIONSETS_ALREADY_ATTACHED, "bindings suggested after attaching");
    std::map<Action*, std::vector<XrPath>> bindings;
    for (uint32_t i = 0; i < info->countSuggestedBindings; ++i) {
        const XrActionSuggestedBinding& b = info->suggestedBindings[i];
        Action* a = GetAction(b.action);
        if (!a) return Violation(XR_ERROR_HANDLE_INVALID, "binding %u: invalid action", i);
        const std::string path = NameOf(inst, b.binding);
        if (!ValidBindingPath(*profile, path)) return Violation(XR_ERROR_PATH_UNSUPPORTED, "%s: binding '%s' (action %s) is not a path of the profile", profile_name.c_str(), path.c_str(), a->name.c_str());
        const bool output = path.find("/output/") != std::string::npos;
        const bool pose = path.size() >= 5 && path.compare(path.size() - 5, 5, "/pose") == 0;
        if ((a->type == XR_ACTION_TYPE_VIBRATION_OUTPUT) != output || (a->type == XR_ACTION_TYPE_POSE_INPUT) != pose) {
            return Violation(XR_ERROR_PATH_UNSUPPORTED, "%s: binding '%s' does not fit action %s's type", profile_name.c_str(), path.c_str(), a->name.c_str());
        }
        bindings[a].push_back(b.binding);
    }
    for (ActionSet* set : inst->action_sets)
        for (Action* a : set->actions) a->bindings.erase(info->interactionProfile);
    for (auto& [a, paths] : bindings) a->bindings[info->interactionProfile] = paths;
    Log("bindings suggested for %s: %u", profile_name.c_str(), info->countSuggestedBindings);
    inst->suggested[info->interactionProfile] = {};
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL AttachSessionActionSets(XrSession h, const XrSessionActionSetsAttachInfo* info) {
    std::lock_guard lock(g_mutex);
    Session* s = GetSession(h);
    if (!s) return Violation(XR_ERROR_HANDLE_INVALID, "xrAttachSessionActionSets: invalid session");
    PT_CHECK_STRUCT(info, XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO);
    if (!s->attached.empty()) return Violation(XR_ERROR_ACTIONSETS_ALREADY_ATTACHED, "xrAttachSessionActionSets twice");
    for (uint32_t i = 0; i < info->countActionSets; ++i) {
        ActionSet* set = GetActionSet(info->actionSets[i]);
        if (!set) return Violation(XR_ERROR_HANDLE_INVALID, "xrAttachSessionActionSets: invalid action set %u", i);
        set->attached = true;
        s->attached.push_back(set);
    }
    Instance* inst = s->instance;
    const XrPath touch = PathOf(inst, "/interaction_profiles/oculus/touch_controller");
    const XrPath simple = PathOf(inst, "/interaction_profiles/khr/simple_controller");
    s->profile = inst->suggested.count(touch) ? touch : inst->suggested.count(simple) ? simple : XR_NULL_PATH;
    Log("action sets attached: %u, interaction profile %s", info->countActionSets, s->profile ? NameOf(inst, s->profile).c_str() : "none");
    XrEventDataInteractionProfileChanged e{XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED};
    e.session = h;
    PushEvent(inst, &e, sizeof(e));
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL GetCurrentInteractionProfile(XrSession h, XrPath top, XrInteractionProfileState* state) {
    std::lock_guard lock(g_mutex);
    Session* s = GetSession(h);
    if (!s) return Violation(XR_ERROR_HANDLE_INVALID, "xrGetCurrentInteractionProfile: invalid session");
    PT_CHECK_STRUCT(state, XR_TYPE_INTERACTION_PROFILE_STATE);
    if (s->attached.empty()) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    const std::string user = NameOf(s->instance, top);
    if (user != "/user/hand/left" && user != "/user/hand/right" && user != "/user/head" && user != "/user/gamepad") {
        return Violation(XR_ERROR_PATH_UNSUPPORTED, "xrGetCurrentInteractionProfile: '%s'", user.c_str());
    }
    state->interactionProfile = (user == "/user/hand/left" || user == "/user/hand/right") ? s->profile : XR_NULL_PATH;
    return XR_SUCCESS;
}

int SubactionIndex(Session* s, Action* a, XrPath sub, XrResult& error) {
    error = XR_SUCCESS;
    if (sub == XR_NULL_PATH) return 0;
    if (std::find(a->subactions.begin(), a->subactions.end(), sub) == a->subactions.end()) {
        error = Violation(XR_ERROR_PATH_UNSUPPORTED, "action %s: subaction path not declared", a->name.c_str());
        return 0;
    }
    const std::string name = NameOf(s->instance, sub);
    return name == "/user/hand/left" ? 1 : name == "/user/hand/right" ? 2 : 0;
}

XrResult GetState(XrSession h, const XrActionStateGetInfo* info, XrActionType type, Action::State& out, Session** session_out = nullptr) {
    Session* s = GetSession(h);
    if (!s) return Violation(XR_ERROR_HANDLE_INVALID, "xrGetActionState: invalid session");
    PT_CHECK_STRUCT(info, XR_TYPE_ACTION_STATE_GET_INFO);
    Action* a = GetAction(info->action);
    if (!a) return Violation(XR_ERROR_HANDLE_INVALID, "xrGetActionState: invalid action");
    if (!a->set->attached || std::find(s->attached.begin(), s->attached.end(), a->set) == s->attached.end()) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    if (a->type != type) return Violation(XR_ERROR_ACTION_TYPE_MISMATCH, "xrGetActionState: action %s type %d asked as %d", a->name.c_str(), a->type, type);
    XrResult error;
    const int index = SubactionIndex(s, a, info->subactionPath, error);
    if (error != XR_SUCCESS) return error;
    out = a->state[index];
    if (session_out) *session_out = s;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL GetActionStateBoolean(XrSession h, const XrActionStateGetInfo* info, XrActionStateBoolean* state) {
    std::lock_guard lock(g_mutex);
    PT_CHECK_STRUCT(state, XR_TYPE_ACTION_STATE_BOOLEAN);
    Action::State st;
    if (const XrResult r = GetState(h, info, XR_ACTION_TYPE_BOOLEAN_INPUT, st); r != XR_SUCCESS) return r;
    state->currentState = st.x > 0.5f ? XR_TRUE : XR_FALSE;
    state->changedSinceLastSync = st.changed;
    state->lastChangeTime = st.change_time;
    state->isActive = st.active;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL GetActionStateFloat(XrSession h, const XrActionStateGetInfo* info, XrActionStateFloat* state) {
    std::lock_guard lock(g_mutex);
    PT_CHECK_STRUCT(state, XR_TYPE_ACTION_STATE_FLOAT);
    Action::State st;
    if (const XrResult r = GetState(h, info, XR_ACTION_TYPE_FLOAT_INPUT, st); r != XR_SUCCESS) return r;
    state->currentState = st.x;
    state->changedSinceLastSync = st.changed;
    state->lastChangeTime = st.change_time;
    state->isActive = st.active;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL GetActionStateVector2f(XrSession h, const XrActionStateGetInfo* info, XrActionStateVector2f* state) {
    std::lock_guard lock(g_mutex);
    PT_CHECK_STRUCT(state, XR_TYPE_ACTION_STATE_VECTOR2F);
    Action::State st;
    if (const XrResult r = GetState(h, info, XR_ACTION_TYPE_VECTOR2F_INPUT, st); r != XR_SUCCESS) return r;
    state->currentState = {st.x, st.y};
    state->changedSinceLastSync = st.changed;
    state->lastChangeTime = st.change_time;
    state->isActive = st.active;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL GetActionStatePose(XrSession h, const XrActionStateGetInfo* info, XrActionStatePose* state) {
    std::lock_guard lock(g_mutex);
    PT_CHECK_STRUCT(state, XR_TYPE_ACTION_STATE_POSE);
    Action::State st;
    if (const XrResult r = GetState(h, info, XR_ACTION_TYPE_POSE_INPUT, st); r != XR_SUCCESS) return r;
    state->isActive = st.active;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL SyncActions(XrSession h, const XrActionsSyncInfo* info) {
    std::lock_guard lock(g_mutex);
    Session* s = GetSession(h);
    if (!s) return Violation(XR_ERROR_HANDLE_INVALID, "xrSyncActions: invalid session");
    PT_CHECK_STRUCT(info, XR_TYPE_ACTIONS_SYNC_INFO);
    if (!s->running) return Violation(XR_ERROR_SESSION_NOT_RUNNING, "xrSyncActions on a session that is not running");
    Instance* inst = s->instance;
    const int frame = CurrentFrame(s);
    std::set<ActionSet*> active;
    for (uint32_t i = 0; i < info->countActiveActionSets; ++i) {
        ActionSet* set = GetActionSet(info->activeActionSets[i].actionSet);
        if (!set) return Violation(XR_ERROR_HANDLE_INVALID, "xrSyncActions: invalid action set %u", i);
        if (std::find(s->attached.begin(), s->attached.end(), set) == s->attached.end()) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
        active.insert(set);
    }
    const bool focused = s->state == XR_SESSION_STATE_FOCUSED;
    for (ActionSet* set : s->attached) {
        for (Action* a : set->actions) {
            for (int sub = 0; sub < 3; ++sub) {
                Action::State next;
                auto it = a->bindings.find(s->profile);
                if (focused && active.count(set) && it != a->bindings.end()) {
                    for (XrPath b : it->second) {
                        const std::string path = NameOf(inst, b);
                        const int side = path.rfind("/user/hand/left", 0) == 0 ? 1 : path.rfind("/user/hand/right", 0) == 0 ? 2 : 0;
                        if (sub != 0 && side != sub) continue;
                        next.active = true;
                        float x = 0, y = 0;
                        InputValue(inst, frame, path, x, y);
                        if (std::abs(x) + std::abs(y) > std::abs(next.x) + std::abs(next.y)) {
                            next.x = x;
                            next.y = y;
                        }
                    }
                }
                Action::State& st = a->state[sub];
                next.changed = next.active && st.active && (next.x != st.x || next.y != st.y);
                if (a->type == XR_ACTION_TYPE_BOOLEAN_INPUT) next.changed = next.active && st.active && ((next.x > 0.5f) != (st.x > 0.5f));
                next.change_time = next.changed ? s->last_predicted : st.change_time;
                st = next;
            }
        }
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL EnumerateBoundSourcesForAction(XrSession h, const XrBoundSourcesForActionEnumerateInfo* info, uint32_t capacity,
                                                              uint32_t* count, XrPath* sources) {
    std::lock_guard lock(g_mutex);
    Session* s = GetSession(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    PT_CHECK_STRUCT(info, XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO);
    Action* a = GetAction(info->action);
    if (!a) return XR_ERROR_HANDLE_INVALID;
    auto it = a->bindings.find(s->profile);
    const std::vector<XrPath> none;
    const std::vector<XrPath>& list = it == a->bindings.end() ? none : it->second;
    *count = static_cast<uint32_t>(list.size());
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
    std::copy(list.begin(), list.end(), sources);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL GetInputSourceLocalizedName(XrSession h, const XrInputSourceLocalizedNameGetInfo* info, uint32_t capacity,
                                                           uint32_t* count, char* buffer) {
    std::lock_guard lock(g_mutex);
    Session* s = GetSession(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    PT_CHECK_STRUCT(info, XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO);
    const std::string name = NameOf(s->instance, info->sourcePath);
    *count = static_cast<uint32_t>(name.size() + 1);
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
    std::memcpy(buffer, name.c_str(), name.size() + 1);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL ApplyHapticFeedback(XrSession h, const XrHapticActionInfo* info, const XrHapticBaseHeader* haptic) {
    std::lock_guard lock(g_mutex);
    Session* s = GetSession(h);
    if (!s) return Violation(XR_ERROR_HANDLE_INVALID, "xrApplyHapticFeedback: invalid session");
    PT_CHECK_STRUCT(info, XR_TYPE_HAPTIC_ACTION_INFO);
    Action* a = GetAction(info->action);
    if (!a) return Violation(XR_ERROR_HANDLE_INVALID, "xrApplyHapticFeedback: invalid action");
    if (a->type != XR_ACTION_TYPE_VIBRATION_OUTPUT) return Violation(XR_ERROR_ACTION_TYPE_MISMATCH, "xrApplyHapticFeedback: %s", a->name.c_str());
    if (!haptic || haptic->type != XR_TYPE_HAPTIC_VIBRATION) return Violation(XR_ERROR_VALIDATION_FAILURE, "xrApplyHapticFeedback: haptic type");
    auto* v = reinterpret_cast<const XrHapticVibration*>(haptic);
    if (v->amplitude < 0.0f || v->amplitude > 1.0f) return Violation(XR_ERROR_VALIDATION_FAILURE, "haptic amplitude %.3f", v->amplitude);
    static int logged = 0;
    if (logged++ < 20) Log("haptic %s amplitude %.3f duration %lld", a->name.c_str(), v->amplitude, static_cast<long long>(v->duration));
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL StopHapticFeedback(XrSession h, const XrHapticActionInfo* info) {
    Session* s = GetSession(h);
    if (!s) return Violation(XR_ERROR_HANDLE_INVALID, "xrStopHapticFeedback: invalid session");
    PT_CHECK_STRUCT(info, XR_TYPE_HAPTIC_ACTION_INFO);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL EnumerateApiLayerProperties(uint32_t, uint32_t* count, XrApiLayerProperties*) {
    *count = 0;
    return XR_SUCCESS;
}

struct Entry {
    const char* name;
    PFN_xrVoidFunction fn;
    bool needs_instance;
};

#define PT_ENTRY(name, fn) {name, reinterpret_cast<PFN_xrVoidFunction>(fn), true}
const Entry kEntries[] = {
    {"xrGetInstanceProcAddr", reinterpret_cast<PFN_xrVoidFunction>(GetInstanceProcAddr), false},
    {"xrEnumerateInstanceExtensionProperties", reinterpret_cast<PFN_xrVoidFunction>(EnumerateInstanceExtensionProperties), false},
    {"xrEnumerateApiLayerProperties", reinterpret_cast<PFN_xrVoidFunction>(EnumerateApiLayerProperties), false},
    {"xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction>(CreateInstance), false},
    PT_ENTRY("xrDestroyInstance", DestroyInstance),
    PT_ENTRY("xrGetInstanceProperties", GetInstanceProperties),
    PT_ENTRY("xrPollEvent", PollEvent),
    PT_ENTRY("xrResultToString", ResultToString),
    PT_ENTRY("xrStructureTypeToString", StructureTypeToString),
    PT_ENTRY("xrGetSystem", GetSystem),
    PT_ENTRY("xrGetSystemProperties", GetSystemProperties),
    PT_ENTRY("xrEnumerateEnvironmentBlendModes", EnumerateEnvironmentBlendModes),
    PT_ENTRY("xrCreateSession", CreateSession),
    PT_ENTRY("xrDestroySession", DestroySession),
    PT_ENTRY("xrEnumerateReferenceSpaces", EnumerateReferenceSpaces),
    PT_ENTRY("xrCreateReferenceSpace", CreateReferenceSpace),
    PT_ENTRY("xrGetReferenceSpaceBoundsRect", GetReferenceSpaceBoundsRect),
    PT_ENTRY("xrCreateActionSpace", CreateActionSpace),
    PT_ENTRY("xrLocateSpace", LocateSpace),
    PT_ENTRY("xrDestroySpace", DestroySpace),
    PT_ENTRY("xrEnumerateViewConfigurations", EnumerateViewConfigurations),
    PT_ENTRY("xrGetViewConfigurationProperties", GetViewConfigurationProperties),
    PT_ENTRY("xrEnumerateViewConfigurationViews", EnumerateViewConfigurationViews),
    PT_ENTRY("xrEnumerateSwapchainFormats", EnumerateSwapchainFormats),
    PT_ENTRY("xrCreateSwapchain", CreateSwapchain),
    PT_ENTRY("xrDestroySwapchain", DestroySwapchain),
    PT_ENTRY("xrEnumerateSwapchainImages", EnumerateSwapchainImages),
    PT_ENTRY("xrAcquireSwapchainImage", AcquireSwapchainImage),
    PT_ENTRY("xrWaitSwapchainImage", WaitSwapchainImage),
    PT_ENTRY("xrReleaseSwapchainImage", ReleaseSwapchainImage),
    PT_ENTRY("xrBeginSession", BeginSession),
    PT_ENTRY("xrEndSession", EndSession),
    PT_ENTRY("xrRequestExitSession", RequestExitSession),
    PT_ENTRY("xrWaitFrame", WaitFrame),
    PT_ENTRY("xrBeginFrame", BeginFrame),
    PT_ENTRY("xrEndFrame", EndFrame),
    PT_ENTRY("xrLocateViews", LocateViews),
    PT_ENTRY("xrStringToPath", StringToPath),
    PT_ENTRY("xrPathToString", PathToString),
    PT_ENTRY("xrCreateActionSet", CreateActionSet),
    PT_ENTRY("xrDestroyActionSet", DestroyActionSet),
    PT_ENTRY("xrCreateAction", CreateAction),
    PT_ENTRY("xrDestroyAction", DestroyAction),
    PT_ENTRY("xrSuggestInteractionProfileBindings", SuggestInteractionProfileBindings),
    PT_ENTRY("xrAttachSessionActionSets", AttachSessionActionSets),
    PT_ENTRY("xrGetCurrentInteractionProfile", GetCurrentInteractionProfile),
    PT_ENTRY("xrGetActionStateBoolean", GetActionStateBoolean),
    PT_ENTRY("xrGetActionStateFloat", GetActionStateFloat),
    PT_ENTRY("xrGetActionStateVector2f", GetActionStateVector2f),
    PT_ENTRY("xrGetActionStatePose", GetActionStatePose),
    PT_ENTRY("xrSyncActions", SyncActions),
    PT_ENTRY("xrEnumerateBoundSourcesForAction", EnumerateBoundSourcesForAction),
    PT_ENTRY("xrGetInputSourceLocalizedName", GetInputSourceLocalizedName),
    PT_ENTRY("xrApplyHapticFeedback", ApplyHapticFeedback),
    PT_ENTRY("xrStopHapticFeedback", StopHapticFeedback),
    PT_ENTRY("xrGetVulkanGraphicsRequirements2KHR", GetVulkanGraphicsRequirements2),
    PT_ENTRY("xrCreateVulkanInstanceKHR", CreateVulkanInstance),
    PT_ENTRY("xrCreateVulkanDeviceKHR", CreateVulkanDevice),
    PT_ENTRY("xrGetVulkanGraphicsDevice2KHR", GetVulkanGraphicsDevice2),
};
#undef PT_ENTRY

XRAPI_ATTR XrResult XRAPI_CALL GetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;
    *function = nullptr;
    for (const Entry& e : kEntries) {
        if (std::strcmp(e.name, name) == 0) {
            if (e.needs_instance && !GetInstance(instance)) return XR_ERROR_HANDLE_INVALID;
            if (std::strstr(name, "Vulkan") && !g_instance->extensions.count(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) return XR_ERROR_FUNCTION_UNSUPPORTED;
            *function = e.fn;
            return XR_SUCCESS;
        }
    }
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

}

PT_XR_EXPORT XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loader, XrNegotiateRuntimeRequest* request) {
    if (!loader || !request || loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST || loader->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
        loader->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    request->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    request->runtimeApiVersion = XR_MAKE_VERSION(1, 0, XR_VERSION_PATCH(XR_CURRENT_API_VERSION));
    request->getInstanceProcAddr = GetInstanceProcAddr;
    return XR_SUCCESS;
}
