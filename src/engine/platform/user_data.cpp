#include "engine/platform/user_data.h"

#include <atomic>
#include <chrono>
#include <cerrno>
#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#endif

namespace pt::platform {
namespace {

constexpr const char* kMarker = ".pt-user-data-migration-v1";
std::atomic<unsigned long long> g_temp_counter{0};

std::string PathText(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

void AddError(UserDataReport& report, const std::string& action,
              const std::filesystem::path& path, const std::error_code& ec) {
    report.errors.push_back(action + " '" + PathText(path) + "': " + ec.message());
}

bool ReserveExclusive(const std::filesystem::path& path, std::error_code& ec) {
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    CloseHandle(file);
    ec.clear();
    return true;
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        ec = std::error_code(errno, std::generic_category());
        return false;
    }
    ::close(fd);
    ec.clear();
    return true;
#endif
}

bool IsReparsePoint(const std::filesystem::path& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    (void)path;
    return false;
#endif
}

bool AtomicPublishNoReplace(const std::filesystem::path& temp,
                            const std::filesystem::path& target,
                            std::error_code& ec) {
#ifdef _WIN32
    if (MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
        ec.clear();
        return true;
    }
    ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
    return false;
#elif defined(__linux__)
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
    if (::syscall(SYS_renameat2, AT_FDCWD, temp.c_str(), AT_FDCWD, target.c_str(), RENAME_NOREPLACE) == 0) {
        ec.clear();
        return true;
    }
    if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) {
        ec = std::error_code(errno, std::generic_category());
        return false;
    }
    // Older Linux kernels may lack renameat2; link is the atomic no-replace fallback.
    if (::link(temp.c_str(), target.c_str()) == 0) {
        if (::unlink(temp.c_str()) != 0) {
            ec = std::error_code(errno, std::generic_category());
            return false;
        }
        ec.clear();
        return true;
    }
    ec = std::error_code(errno, std::generic_category());
    return false;
#else
    // POSIX filesystems with hard-link support can publish without replacing an existing entry.
    if (::link(temp.c_str(), target.c_str()) == 0) {
        if (::unlink(temp.c_str()) != 0) {
            ec = std::error_code(errno, std::generic_category());
            return false;
        }
        ec.clear();
        return true;
    }
    ec = std::error_code(errno, std::generic_category());
    return false;
#endif
}

bool IsValidMarker(const std::filesystem::path& marker, bool& exists,
                   UserDataReport& report) {
    exists = false;
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(marker, ec);
    if (ec == std::errc::no_such_file_or_directory) return true;
    if (ec) {
        AddError(report, "Cannot inspect migration marker", marker, ec);
        return false;
    }
    if (status.type() == std::filesystem::file_type::not_found) return true;
    exists = true;
    if (std::filesystem::is_symlink(status) || IsReparsePoint(marker) || !std::filesystem::is_regular_file(status)) {
        report.errors.push_back("Migration marker must be a regular file: '" + PathText(marker) + "'");
        return false;
    }
    std::ifstream input(marker, std::ios::binary);
    if (!input) {
        report.errors.push_back("Cannot read migration marker: '" + PathText(marker) + "'");
        return false;
    }
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad() || contents != "1\n") {
        report.errors.push_back("Migration marker is invalid: '" + PathText(marker) + "'");
        return false;
    }
    return true;
}

bool MakeTempFile(const std::filesystem::path& parent, std::filesystem::path& temp,
                  std::error_code& ec) {
    for (int attempt = 0; attempt != 64; ++attempt) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto serial = g_temp_counter.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
        const auto process = static_cast<unsigned long long>(GetCurrentProcessId());
#else
        const auto process = static_cast<unsigned long long>(::getpid());
#endif
        temp = parent / (".pt-migrate-" + std::to_string(process) + "-" +
                         std::to_string(stamp) + "-" + std::to_string(serial));
        if (ReserveExclusive(temp, ec)) return true;
        if (ec != std::errc::file_exists) return false;
    }
    ec = std::make_error_code(std::errc::file_exists);
    return false;
}

bool PublishFile(const std::filesystem::path& source, const std::filesystem::path& target,
                 std::error_code& ec) {
    std::filesystem::path temp;
    if (!MakeTempFile(target.parent_path(), temp, ec)) return false;

    std::ifstream in(source, std::ios::binary);
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!in || !out) {
        ec = std::make_error_code(std::errc::io_error);
        out.close();
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return false;
    }
    char buffer[64 * 1024];
    while (in) {
        in.read(buffer, sizeof(buffer));
        const auto count = in.gcount();
        if (count > 0) out.write(buffer, count);
    }
    out.flush();
    const bool copied = !in.bad() && static_cast<bool>(out);
    out.close();
    if (!copied || !out) {
        ec = std::make_error_code(std::errc::io_error);
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return false;
    }

    if (AtomicPublishNoReplace(temp, target, ec)) return true;
    std::error_code cleanup_error;
    std::filesystem::remove(temp, cleanup_error);
    return false;
}

bool WriteMarker(const std::filesystem::path& destination, std::error_code& ec) {
    const auto target = destination / kMarker;
    std::filesystem::path temp;
    if (!MakeTempFile(destination, temp, ec)) return false;
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << "1\n";
        out.flush();
        if (!out) {
            ec = std::make_error_code(std::errc::io_error);
            out.close();
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            return false;
        }
        out.close();
        if (!out) {
            ec = std::make_error_code(std::errc::io_error);
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            return false;
        }
    }
    if (AtomicPublishNoReplace(temp, target, ec)) return true;
    std::error_code cleanup_error;
    std::filesystem::remove(temp, cleanup_error);
    return false;
}

bool ProbeWritable(const std::filesystem::path& destination, std::error_code& ec) {
    std::filesystem::path probe;
    if (!MakeTempFile(destination, probe, ec)) return false;
    std::error_code remove_error;
    std::filesystem::remove(probe, remove_error);
    if (remove_error) {
        ec = remove_error;
        return false;
    }
    ec.clear();
    return true;
}

bool EnsureDestinationDirectories(const std::filesystem::path& root,
                                  const std::filesystem::path& relative,
                                  UserDataReport& report) {
    auto current = root;
    for (const auto& component : relative) {
        current /= component;
        std::error_code ec;
        auto status = std::filesystem::symlink_status(current, ec);
        if (ec == std::errc::no_such_file_or_directory) ec.clear();
        if (ec) {
            AddError(report, "Cannot inspect destination directory", current, ec);
            return false;
        }
        if (!std::filesystem::exists(status)) {
            if (!std::filesystem::create_directory(current, ec) && ec != std::errc::file_exists) {
                AddError(report, "Cannot create user data directory", current, ec);
                return false;
            }
            status = std::filesystem::symlink_status(current, ec);
            if (ec) {
                AddError(report, "Cannot inspect destination directory", current, ec);
                return false;
            }
        }
        if (std::filesystem::is_symlink(status) || IsReparsePoint(current) || !std::filesystem::is_directory(status)) {
            report.errors.push_back("Destination path is not a real directory: '" + PathText(current) + "'");
            return false;
        }
    }
    return true;
}

void CopyTree(const std::filesystem::path& source, const std::filesystem::path& destination,
              UserDataReport& report) {
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(source, ec), end;
    if (ec) {
        AddError(report, "Cannot read legacy data directory", source, ec);
        return;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) {
            AddError(report, "Cannot scan legacy data directory", source, ec);
            ec.clear();
            continue;
        }
        const auto from = it->path();
        const auto relative = from.lexically_relative(source);
        if (relative.empty() || relative == ".") continue;
        const auto status = it->symlink_status(ec);
        if (ec) {
            AddError(report, "Cannot inspect legacy entry", from, ec);
            ec.clear();
            continue;
        }
        if (std::filesystem::is_symlink(status) || IsReparsePoint(from)) {
            report.warnings.push_back("Skipped linked legacy path: '" + PathText(from) + "'");
            it.disable_recursion_pending();
            continue;
        }
        if (std::filesystem::is_directory(status)) {
            if (!EnsureDestinationDirectories(destination, relative, report)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) continue;
        if (relative == kMarker) continue;

        const auto to = destination / relative;
        const auto target_status = std::filesystem::symlink_status(to, ec);
        if (ec == std::errc::no_such_file_or_directory) ec.clear();
        if (ec) {
            AddError(report, "Cannot inspect destination file", to, ec);
            ec.clear();
            continue;
        }
        if (std::filesystem::exists(target_status)) {
            if (std::filesystem::is_directory(target_status)) {
                AddError(report, "Destination directory blocks legacy file", to,
                         std::make_error_code(std::errc::file_exists));
            }
            continue; // Existing destination entries always win.
        }
        const auto parentRelative = relative.parent_path();
        if (!EnsureDestinationDirectories(destination, parentRelative, report)) continue;
        if (PublishFile(from, to, ec)) {
            ++report.files_copied;
        } else {
            std::error_code statusError;
            const auto publishedStatus = std::filesystem::symlink_status(to, statusError);
            if (!statusError && std::filesystem::exists(publishedStatus)) {
                ec.clear(); // A concurrent writer published first; preserve its entry.
            } else {
                AddError(report, "Cannot migrate user data file", to, ec);
                ec.clear();
            }
        }
    }
    if (ec) AddError(report, "Cannot finish scanning legacy data directory", source, ec);
}

} // namespace

UserDataReport PrepareUserDataDirectory(const std::filesystem::path& destination,
                                       const std::filesystem::path& legacy,
                                       bool migrate_legacy,
                                       const std::function<bool(const std::filesystem::path&)>& ask) {
    UserDataReport report;
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    if (ec) {
        AddError(report, "Cannot create user data directory", destination, ec);
        return report;
    }
    const auto dest_status = std::filesystem::symlink_status(destination, ec);
    if (ec || std::filesystem::is_symlink(dest_status) || IsReparsePoint(destination) ||
        !std::filesystem::is_directory(dest_status)) {
        if (ec) AddError(report, "Cannot inspect user data directory", destination, ec);
        else report.errors.push_back("User data destination must be a real directory: '" + PathText(destination) + "'");
        return report;
    }
    if (!ProbeWritable(destination, ec)) {
        AddError(report, "User data directory is not writable", destination, ec);
        return report;
    }
    if (!migrate_legacy) {
        report.success = true;
        return report;
    }

    const auto marker = destination / kMarker;
    bool markerExists = false;
    if (!IsValidMarker(marker, markerExists, report)) return report;
    if (markerExists) {
        report.success = true;
        return report;
    }

    bool legacy_exists = false;
    if (!legacy.empty()) {
        const auto legacy_status = std::filesystem::symlink_status(legacy, ec);
        if (!ec && std::filesystem::is_directory(legacy_status) &&
            !std::filesystem::is_symlink(legacy_status) && !IsReparsePoint(legacy)) {
            legacy_exists = true;
        } else if (!ec && (std::filesystem::is_symlink(legacy_status) || IsReparsePoint(legacy))) {
            report.warnings.push_back("Skipped linked legacy data directory: '" + PathText(legacy) + "'");
        } else if (ec == std::errc::no_such_file_or_directory) {
            ec.clear();
        } else if (ec) {
            AddError(report, "Cannot inspect legacy data directory", legacy, ec);
            return report;
        } else if (std::filesystem::exists(legacy_status)) {
            report.errors.push_back("Legacy data path is not a real directory: '" + PathText(legacy) + "'");
            return report;
        }
    }
    if (legacy_exists) {
        std::error_code samePathError;
        const bool samePath = std::filesystem::equivalent(legacy, destination, samePathError);
        if (samePathError) {
            AddError(report, "Cannot compare legacy and destination data paths", legacy, samePathError);
            return report;
        }
        bool has_files = false;
        std::error_code scan;
        for (auto it = std::filesystem::recursive_directory_iterator(legacy, scan); !scan && it != std::filesystem::recursive_directory_iterator();
             it.increment(scan)) {
            if (it->is_regular_file(scan)) {
                has_files = true;
                break;
            }
        }
        if (samePath) {
            report.warnings.push_back("Legacy and destination data paths are the same; skipped self-migration");
        } else if (has_files && ask && !ask(legacy)) {
            report.declined_legacy = true;
        } else if (has_files) {
            CopyTree(legacy, destination, report);
            if (!report.errors.empty()) return report; // Retry partial work next launch.
            report.migrated_legacy = true;
            if (ask) {
                std::error_code removeError;
                std::filesystem::remove_all(legacy, removeError);
                if (removeError) {
                    report.warnings.push_back("Moved the old saves but could not delete '" + PathText(legacy) + "': " + removeError.message());
                } else {
                    report.removed_legacy = true;
                    // the empty pt-port folder above it too
                    std::filesystem::remove(legacy.parent_path(), removeError);
                }
            }
        }
    }

    if (!WriteMarker(destination, ec)) {
        bool appeared = false;
        if (IsValidMarker(marker, appeared, report) && appeared) {
            report.success = true;
            return report;
        }
        if (!report.errors.empty()) return report;
        AddError(report, "Cannot write user data migration marker", marker, ec);
        return report;
    }
    report.success = true;
    return report;
}

} // namespace pt::platform
