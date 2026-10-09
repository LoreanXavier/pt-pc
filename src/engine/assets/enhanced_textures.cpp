#include "engine/assets/enhanced_textures.h"
#include "engine/core/log.h"
#include "engine/core/pathcode.h"
#include "engine/platform/os.h"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace pt {
namespace {
constexpr std::string_view kModel = "realesr-general-x4v3-x2";
uint64_t Hash(std::span<const uint8_t> bytes, uint64_t hash = 14695981039346656037ull) {
    for (uint8_t byte : bytes) { hash ^= byte; hash *= 1099511628211ull; }
    return hash;
}
std::vector<uint8_t> Read(const std::filesystem::path& path, size_t max = 64 * 1024 * 1024) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (!file || size < 0 || static_cast<uint64_t>(size) > max) throw std::runtime_error("Cannot read texture source or model.");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!file) throw std::runtime_error("Incomplete texture source or model.");
    return bytes;
}
std::vector<std::string> TexturePaths(const std::filesystem::path& game) {
    const auto bytes = Read(game / "pathid_list_ps4.bin");
    auto u32 = [&](size_t at) { uint32_t v; if (at > bytes.size() || bytes.size() - at < 4) throw std::runtime_error("Invalid texture name table.");
        std::memcpy(&v, bytes.data() + at, 4); return v; };
    auto align = [](size_t n) { return (n + 15) & ~size_t(15); };
    const size_t count = u32(4), dirs = u32(8), names = u32(12);
    if (count > 1000000 || dirs > 1000000 || names > 1000000) throw std::runtime_error("Invalid texture name table.");
    const size_t records = align(32 + 8 * count), dir_table = align(records + 4 * count);
    const size_t name_table = align(dir_table + 4 * dirs), strings = align(name_table + 4 * names);
    auto string = [&](size_t offset) {
        const size_t start = strings + offset;
        if (start >= bytes.size()) throw std::runtime_error("Invalid texture path.");
        const auto end = std::find(bytes.begin() + start, bytes.end(), 0);
        if (end == bytes.end()) throw std::runtime_error("Unterminated texture path.");
        return std::string(bytes.begin() + start, end);
    };
    std::vector<std::string> paths;
    for (size_t i = 0; i < count; ++i) {
        const size_t code_at = 32 + i * 8;
        if (code_at > bytes.size() || bytes.size() - code_at < 8) throw std::runtime_error("Invalid texture code table.");
        uint64_t code;
        std::memcpy(&code, bytes.data() + code_at, 8);
        // The name table stores stems without extensions; file type is encoded in the high bits of the path code.
        if ((code >> 51) != ExtensionType("ftex")) continue;
        const uint32_t record = u32(records + i * 4), dir = record >> 20, name = record & 0xffff;
        if (dir >= dirs || name >= names) throw std::runtime_error("Invalid texture path index.");
        std::string path = string(u32(dir_table + dir * 4)) + "/" + string(u32(name_table + name * 4));
        if (path.starts_with("/app0/as/")) path = "/Assets/" + path.substr(9);
        paths.push_back(FtexStem(path));
    }
    std::sort(paths.begin(), paths.end()); paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}
void WritePng(const std::filesystem::path& path, uint32_t w, uint32_t h, std::span<const uint8_t> pixels) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    auto write = [](void* ctx, void* data, int size) { static_cast<std::ofstream*>(ctx)->write(static_cast<char*>(data), size); };
    if (!stbi_write_png_to_func(write, &file, static_cast<int>(w), static_cast<int>(h), 4, pixels.data(), w * 4) || !file)
        throw std::runtime_error("Cannot write temporary texture (check disk space).");
}
// the upscaler's own release build for this platform (cmake/EnhancedTextures.cmake)
#ifdef _WIN32
constexpr const char* kUpscalerExe = "realesrgan-ncnn-vulkan.exe";
#else
constexpr const char* kUpscalerExe = "realesrgan-ncnn-vulkan";
#endif
std::string Utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}
bool Upscale(const std::filesystem::path& runtime, const std::filesystem::path& input,
             const std::filesystem::path& output, const std::filesystem::path& log, const std::atomic<bool>& cancel) {
    const auto result = os::RunProcess(runtime / kUpscalerExe,
        {"-i", Utf8(input), "-o", Utf8(output), "-m", Utf8(runtime / "models"), "-n", "realesr-general-x4v3-x2", "-s", "2", "-t", "128",
         "-j", "1:1:1", "-f", "png"},
        runtime, log, cancel, std::chrono::minutes(2));
    if (!result.started) throw std::runtime_error("Cannot start the bundled texture upscaler.");
    if (result.cancelled || cancel) return false;
    if (result.timed_out) throw std::runtime_error("Texture upscaler timed out; original textures remain active.");
    if (result.exit_code != 0) throw std::runtime_error("Texture upscaler failed; see enhanced-textures/upscaler.log.");
    return true;
}
}

uint64_t EnhancedModelKey(const std::filesystem::path& runtime) {
    try {
        uint64_t key = Hash(Read(runtime / "models" / (std::string(kModel) + ".param")));
        key = Hash(Read(runtime / "models" / (std::string(kModel) + ".bin")), key);
        return Hash(Read(runtime / kUpscalerExe), key);
    } catch (...) { return 0; }
}
uint64_t EnhancedTextureKey(const FtexTexture& t, uint64_t model) {
    const uint32_t header[] = {2, t.width, t.height, t.pixel_format, t.flags, t.faces, t.mip_count};
    uint64_t key = Hash({reinterpret_cast<const uint8_t*>(header), sizeof(header)}, model);
    for (const auto& mip : t.mips) key = Hash(mip, key);
    return key;
}
std::filesystem::path EnhancedCacheFile(const std::filesystem::path& root, std::string_view stem) {
    return root / std::format("{:016x}.pttex", PathCode64(std::string(stem) + ".ftex"));
}

EnhancedTextureJob::~EnhancedTextureJob() { Cancel(); if (worker_.joinable()) worker_.join(); }
void EnhancedTextureJob::Cancel() { cancel_ = true; }
EnhancedTextureJob::Status EnhancedTextureJob::GetStatus() const { std::lock_guard lock(mutex_); return status_; }
void EnhancedTextureJob::Publish(State state, uint32_t done, uint32_t total, std::string note) {
    std::lock_guard lock(mutex_); status_ = {state, done, total, std::move(note)};
}
bool EnhancedTextureJob::Start(const std::filesystem::path& game, const std::filesystem::path& cache,
                               const std::filesystem::path& runtime, uint32_t limit, uint32_t max_output) {
    if (GetStatus().state == State::Running) return false;
    if (worker_.joinable()) worker_.join();
    cancel_ = false; Publish(State::Running, 0, 0, "Checking texture cache...");
    worker_ = std::thread(&EnhancedTextureJob::Run, this, game, cache, runtime, limit, max_output);
    return true;
}
void EnhancedTextureJob::Run(std::filesystem::path game, std::filesystem::path cache, std::filesystem::path runtime, uint32_t limit,
                             uint32_t max_output) {
    const auto job_started = std::chrono::steady_clock::now();
    uint32_t generated = 0;
    std::optional<os::FileLock> lock;
    std::filesystem::path temporary;
    uint32_t done = 0, total = 0;
    try {
        const uint64_t model = EnhancedModelKey(runtime);
        if (!model) throw std::runtime_error("Enhanced texture tools or model are missing.");
        std::filesystem::create_directories(cache);
        lock.emplace(cache / "generation.lock");
        if (!lock->Held()) throw std::runtime_error("Texture generation is already running in another instance.");
        QarArchive qar;
        if (!qar.Open(game / "texture.qar")) throw std::runtime_error("Cannot open the game's texture archive.");
        std::vector<std::string> candidates;
        for (const auto& path : TexturePaths(game)) {
            if (cancel_) break;
            // The cheap name check avoids decompressing normal/specular maps during inventory.
            if (!path.ends_with("_bsm") && !path.ends_with("_lym") && !path.ends_with("_ils")) continue;
            FtexTexture source;
            if (LoadFtex(qar, path, source) && EnhancedTextureEligible(path, source)) candidates.push_back(path);
        }
        if (limit && candidates.size() > limit) candidates.resize(limit);
        total = static_cast<uint32_t>(candidates.size());
        if (!total && !cancel_) throw std::runtime_error("No supported colour textures were found.");
        temporary = cache / ("job-" + std::to_string(os::ProcessId()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(temporary);
        for (const auto& path : candidates) {
            if (cancel_) break;
            Publish(State::Running, done, total, std::format("Generating textures: {} / {}. Switch Off to cancel.", done, total));
            FtexTexture source, cached;
            if (!LoadFtex(qar, path, source)) throw std::runtime_error("A texture could not be decoded.");
            const uint64_t key = EnhancedTextureKey(source, model);
            const auto file = EnhancedCacheFile(cache, path);
            if (!ReadTextureCache(file, key, cached)) {
                std::vector<uint8_t> pixels;
                if (!DecodeFtexLevel(source, 0, pixels)) throw std::runtime_error("A top-level texture could not be decoded.");
                bool opaque = true;
                for (size_t i = 3; i < pixels.size(); i += 4) if (pixels[i] != 255) { opaque = false; break; }
                if (!opaque) { ++done; continue; } // BC1 can contain cutouts too.
                if (std::filesystem::space(cache).available < 256 * 1024 * 1024ull) throw std::runtime_error("Not enough free space for enhanced textures.");
                const auto in = temporary / "input.png", out = temporary / "output.png";
                WritePng(in, source.width, source.height, pixels);
                if (!Upscale(runtime, in, out, cache / "upscaler.log", cancel_)) break;
                const auto bytes = Read(out, 128 * 1024 * 1024);
                int w = 0, h = 0, channels = 0;
                if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels) ||
                    w != static_cast<int>(source.width * 2) || h != static_cast<int>(source.height * 2))
                    throw std::runtime_error("Texture upscaler returned an invalid size.");
                stbi_uc* image = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
                if (!image) throw std::runtime_error("Texture upscaler output could not be read.");
                const bool dimensions = w == static_cast<int>(source.width * 2) && h == static_cast<int>(source.height * 2);
                uint64_t input_rgb = 0, output_rgb = 0;
                for (size_t i = 0; i < pixels.size(); ++i) if ((i & 3) != 3) input_rgb += pixels[i];
                if (dimensions) for (size_t i = 0; i < size_t(w) * h * 4; ++i) if ((i & 3) != 3) output_rgb += image[i];
                const bool black_output = dimensions && input_rgb > pixels.size() * 4 && output_rgb == 0;
                std::vector<uint8_t> result(image, image + size_t(w) * h * 4);
                stbi_image_free(image);
                // the capped mode (devices with less video memory): the 2x result is reduced back to the cap, a 2x2 box in linear
                // light, so a 2048 source keeps its size with the upscaler's detail
                while (dimensions && max_output && (static_cast<uint32_t>(w) > max_output || static_cast<uint32_t>(h) > max_output)) {
                    const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
                    std::vector<uint8_t> half(size_t(nw) * nh * 4);
                    for (int y = 0; y < nh; ++y) for (int x = 0; x < nw; ++x) for (int c = 0; c < 4; ++c) {
                        float sum = 0.0f;
                        for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
                            const float v = result[(size_t(std::min(y * 2 + dy, h - 1)) * w + std::min(x * 2 + dx, w - 1)) * 4 + c] / 255.0f;
                            sum += source.Srgb() && c < 3 ? std::pow(v, 2.2f) : v;
                        }
                        const float mean = sum * 0.25f;
                        half[(size_t(y) * nw + x) * 4 + c] =
                            static_cast<uint8_t>(std::lround(std::clamp(source.Srgb() && c < 3 ? std::pow(mean, 1.0f / 2.2f) : mean, 0.0f, 1.0f) * 255.0f));
                    }
                    result = std::move(half);
                    w = nw;
                    h = nh;
                }
                bool encoded = dimensions && !black_output && EncodeEnhancedTexture(w, h, result, source.Srgb(), cached);
                if (!encoded) throw std::runtime_error("Texture upscaler returned invalid pixels or size.");
                if (cancel_) break;
                if (!WriteTextureCache(file, key, cached)) throw std::runtime_error("Cannot save enhanced textures (check disk space).");
                ++generated;
                std::filesystem::remove(in); std::filesystem::remove(out);
            }
            ++done;
        }
        if (!cancel_) {
            std::ofstream manifest(cache / "manifest.txt", std::ios::trunc);
            manifest << "cache_version=2\nmodel=" << kModel << "\nmodel_fingerprint=" << model
                << "\narchive_bytes=" << std::filesystem::file_size(game / "texture.qar")
                << "\narchive_mtime=" << static_cast<long long>(std::filesystem::last_write_time(game / "texture.qar").time_since_epoch().count())
                << "\ntextures=" << total << "\nmax_output=" << max_output << "\n";
            if (!manifest) throw std::runtime_error("Cannot save enhanced texture manifest.");
        }
        Publish(cancel_ ? State::Cancelled : State::Ready, done, total, cancel_ ? "Texture generation cancelled." : "Enhanced textures ready.");
        LogInfo("enhanced textures: {} / {}, {} ({} generated in {:.1f} s, output {})", done, total, cancel_ ? "cancelled" : "ready", generated,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - job_started).count(),
                max_output ? std::format("capped at {}", max_output) : std::string("2x"));
    } catch (const std::exception& e) { Publish(State::Failed, done, total, e.what()); LogWarn("enhanced textures: {}", e.what()); }
    lock.reset();
    if (!temporary.empty()) { std::error_code ec; std::filesystem::remove_all(temporary, ec); }
}
}
