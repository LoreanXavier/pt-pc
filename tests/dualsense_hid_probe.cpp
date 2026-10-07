#include <SDL3/SDL.h>
#include <SDL3/SDL_hidapi.h>
#include <windows.h>
#include <zlib.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    const bool enable = argc == 2 && std::strcmp(argv[1], "--enable-mic") == 0;
    if (enable) {
        std::fprintf(stderr, "Microphone writes disabled: disconnects and unintended desktop input were observed.\n");
        return 4;
    }
    const bool hold = argc == 2 && std::strcmp(argv[1], "--keepalive") == 0;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SDL_hid_init();
    auto* devices = SDL_hid_enumerate(0x054c, 0);
    SDL_hid_device* device = nullptr;
    std::wstring device_path;
    for (auto* d = devices; d; d = d->next) {
        if ((d->product_id == 0x0ce6 || d->product_id == 0x0df2) && d->bus_type == SDL_HID_API_BUS_BLUETOOTH) {
            device = SDL_hid_open_path(d->path);
            if (device) {
                const int count = MultiByteToWideChar(CP_UTF8, 0, d->path, -1, nullptr, 0);
                device_path.resize(count);
                MultiByteToWideChar(CP_UTF8, 0, d->path, -1, device_path.data(), count);
                std::printf("DualSense Bluetooth HID opened\n"); break;
            }
        }
    }
    SDL_hid_free_enumeration(devices);
    if (!device) { std::printf("No accessible Bluetooth DualSense\n"); SDL_hid_exit(); return 2; }
    HANDLE writer = enable ? CreateFileW(device_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr) : INVALID_HANDLE_VALUE;
    if (enable && writer == INVALID_HANDLE_VALUE) { std::printf("raw HID writer failed: %lu\n", GetLastError()); SDL_hid_close(device); SDL_hid_exit(); return 3; }
    uint8_t sequence = 0;
    unsigned writes = 0;
    auto control = [&](bool on) {
        std::array<uint8_t, 398> report{};
        report[0] = 0x36; report[1] = (sequence++ & 15) << 4;
        report[2] = 0x91; report[3] = 7; report[4] = on ? 0xff : 0xfe;
        for (int i = 5; i <= 9; ++i) report[i] = 16;
        report[10] = sequence; report[11] = 0x90; report[12] = 63;
        report[13] = 0x40; report[14] = 0x02; report[19] = 0xff; report[22] = 0;
        report[76] = 0x92; report[77] = 64;
        const uint8_t prefix = 0xa2;
        uint32_t crc = crc32(0, &prefix, 1);
        crc = crc32(crc, report.data(), report.size() - 4);
        std::memcpy(report.data() + report.size() - 4, &crc, 4);
        OVERLAPPED operation{}; operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD bytes = 0;
        bool ok = WriteFile(writer, report.data(), static_cast<DWORD>(report.size()), &bytes, &operation);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(operation.hEvent, 1000) != WAIT_OBJECT_0) CancelIoEx(writer, &operation);
            ok = GetOverlappedResult(writer, &operation, &bytes, TRUE);
        }
        const DWORD error = ok ? 0 : GetLastError();
        CloseHandle(operation.hEvent);
        if (!writes || !on || !ok) std::printf("microphone %s raw control: %lu bytes, error %lu\n", on ? "enable" : "disable", bytes, error);
        ++writes;
        return ok && bytes >= report.size();
    };
    if (enable && !control(true)) { control(false); CloseHandle(writer); SDL_hid_close(device); SDL_hid_exit(); return 3; }
    unsigned normal = 0, mic = 0, other = 0, errors = 0;
    const uint64_t deadline = SDL_GetTicks() + (hold ? 120000 : 4000);
    uint64_t keepalive = SDL_GetTicks() + 1000;
    uint64_t next_control = SDL_GetTicks() + 8;
    while (SDL_GetTicks() < deadline) {
        std::array<uint8_t, 512> report{};
        const int n = SDL_hid_read_timeout(device, report.data(), report.size(), enable ? 4 : 50);
        if (n < 0) { std::printf("HID read error: %s\n", SDL_GetError()); ++errors; break; }
        if (n > 0) {
            if (n == 78 && report[0] == 0x31 && (report[1] & 2)) ++mic;
            else if (report[0] == 0x01 || (n == 78 && report[0] == 0x31 && (report[1] & 1))) ++normal;
            else ++other;
        }
        if (SDL_GetTicks() >= keepalive) {
            std::array<uint8_t, 64> feature{}; feature[0] = 0x05;
            const int result = SDL_hid_get_feature_report(device, feature.data(), feature.size());
            std::printf("keepalive feature response: %d bytes\n", result);
            keepalive = SDL_GetTicks() + 1000;
        }
        if (enable && SDL_GetTicks() >= next_control) {
            if (!control(true)) { ++errors; break; }
            next_control = SDL_GetTicks() + 8;
        }
    }
    const bool disabled = !enable || control(false);
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    SDL_hid_close(device); SDL_hid_exit();
    std::printf("reports: normal=%u microphone=%u other=%u errors=%u\n", normal, mic, other, errors);
    return errors || !disabled || (enable && !mic) ? 1 : 0;
}
