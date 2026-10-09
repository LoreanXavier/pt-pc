// The installer on Linux (docs/installer.md, "Linux"): the same steps and checks as the Windows setup (setup_core.h), from
// a terminal, or with zenity file pickers when it is started without one. The payload is appended to this executable by
// tools/linux/attach_payload.py: payload, then "PTPAYLD1", the payload's offset and size (u64 each) at the very end.
#include "engine/platform/os.h"
#include "engine/platform/self_integrity.h"
#include "engine/platform/update_check.h"
#include "setup_core.h"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <thread>

using namespace pt::setup;

namespace {
constexpr const char* kProduct = "P.T. PC Port";
constexpr const char* kSlug = "pt-pc-port";
std::atomic<bool> cancel{false};

std::string HexDigest(const unsigned char digest[32]) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 32; ++i) {
        out += hex[digest[i] >> 4];
        out += hex[digest[i] & 15];
    }
    return out;
}
std::string HashFile(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
    unsigned char digest[32];
    if (!input.eof() && !input) throw std::runtime_error("Could not verify file integrity.");
    pt::integrity::Sha256(bytes.data(), bytes.size(), digest);
    return HexDigest(digest);
}
// the payload appended to this executable
std::vector<unsigned char> Payload() {
    std::ifstream self("/proc/self/exe", std::ios::binary);
    self.seekg(0, std::ios::end);
    const auto size = uint64_t(self.tellg());
    if (size < 24) throw std::runtime_error("Installer payload missing.");
    char trailer[24];
    self.seekg(std::streamoff(size - 24));
    self.read(trailer, 24);
    uint64_t offset = 0, length = 0;
    std::memcpy(&offset, trailer + 8, 8);
    std::memcpy(&length, trailer + 16, 8);
    if (std::memcmp(trailer, "PTPAYLD1", 8) != 0 || offset + length + 24 != size) throw std::runtime_error("Installer payload missing.");
    std::vector<unsigned char> data(length);
    self.seekg(std::streamoff(offset));
    self.read(reinterpret_cast<char*>(data.data()), std::streamsize(length));
    if (!self) throw std::runtime_error("Installer payload missing.");
    return data;
}
// the target (when it exists) and every folder above it: no symbolic links
// Symbolic links among the existing parents are resolved first: on Fedora Atomic desktops (Bazzite, Silverblue, Kinoite) /home
// is a link to /var/home, so every default path held one and players had to type /var/home by hand. A link at or below the
// chosen folder, which the setup would write through, is still refused.
void CheckParents(const fs::path& destination) {
    std::error_code canonical_error;
    fs::path resolved = fs::absolute(destination);
    fs::path existing = resolved;
    while (!existing.empty() && !fs::exists(existing, canonical_error) && existing.parent_path() != existing) existing = existing.parent_path();
    if (existing != resolved) {
        const fs::path real = fs::canonical(existing, canonical_error);
        if (!canonical_error) resolved = real / fs::relative(resolved, existing, canonical_error);
    } else if (fs::is_symlink(resolved, canonical_error)) {
        throw std::runtime_error("Choose a destination without symbolic links.");
    } else {
        const fs::path real = fs::canonical(resolved, canonical_error);
        if (!canonical_error) resolved = real;
    }
    for (auto p = resolved; !p.empty();) {
        std::error_code error;
        if (fs::is_symlink(p, error)) throw std::runtime_error("Choose a destination without symbolic links.");
        const auto parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
}
// the setup's own file against a corrupt download (self_integrity.h, the stamp of tools/linux/attach_payload.py)
void VerifyIntegrity() {
    CheckCancel();
    if (!pt::integrity::IntegrityOk()) throw std::runtime_error("This setup file is damaged or was modified. Please download it again.");
}
// the payload stores no file modes: the programs get theirs here
void MarkExecutables(const fs::path& root) {
    for (const char* name : {"pt", "extractor/PT.PkgExtract", "texture-tools/realesrgan-ncnn-vulkan"}) {
        std::error_code error;
        if (fs::is_regular_file(root / name, error)) fs::permissions(root / name, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, error);
    }
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().filename().string().find(".so") != std::string::npos) {
            std::error_code error;
            fs::permissions(entry.path(), fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, error);
        }
    }
}
void Extract(const fs::path& staging, const fs::path& package) {
    // the helper's output files, polled for the progress while RunProcess waits
    const fs::path assets = staging / "CUSA01127";
    const uint64_t before = progress.done;
    std::atomic<bool> finished{false};
    std::thread poll([&] {
        while (!finished) {
            progress.At(before + FolderBytes(assets));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });
    const auto result = pt::os::RunProcess(staging / "extractor" / "PT.PkgExtract", {fs::absolute(package).string(), assets.string()}, staging,
                                           staging / "install-extraction.log", cancel, std::chrono::hours(2));
    finished = true;
    poll.join();
    if (!result.started) throw std::runtime_error("Could not start the PKG extraction helper.");
    CheckCancel();
    if (result.exit_code != 0) {
        std::ifstream input(staging / "install-extraction.log");
        std::string details((std::istreambuf_iterator<char>(input)), {});
        throw std::runtime_error("PKG extraction failed. " + details.substr(0, 600));
    }
}
// a menu entry in ~/.local/share/applications
void WriteDesktop(const fs::path& file, const fs::path& destination) {
    std::ofstream out(file);
    out << "[Desktop Entry]\nType=Application\nName=" << kProduct << "\nComment=" << kProduct << "\nExec=\""
        << (destination / "pt").string() << "\"\nPath=" << destination.string() << "\nTerminal=false\nCategories=Game;\n";
    std::error_code error;
    if (fs::is_regular_file(destination / "icon0.png", error)) out << "Icon=" << (destination / "icon0.png").string() << "\n";
}
void Shortcut(const fs::path& destination) {
    const char* home = std::getenv("HOME");
    if (!home) return;
    const fs::path dir = fs::path(home) / ".local" / "share" / "applications";
    std::error_code error;
    fs::create_directories(dir, error);
    fs::path file = dir / (std::string(kSlug) + ".desktop");
    if (fs::exists(file)) file = dir / (std::string(kSlug) + "-" + destination.filename().string() + ".desktop");
    if (!fs::exists(file)) WriteDesktop(file, destination);
    const fs::path desktop = fs::path(home) / "Desktop";
    if (fs::is_directory(desktop, error)) {
        fs::path link = desktop / (std::string(kProduct) + ".desktop");
        if (fs::exists(link)) link = desktop / (std::string(kProduct) + " " + destination.filename().string() + ".desktop");
        if (!fs::exists(link)) {
            WriteDesktop(link, destination);
            ::chmod(link.c_str(), 0755);
        }
    }
}
// A new install, or the update of the install in `destination` (asked first when interactive)
InstallOutcome Install(const fs::path& input, const fs::path& destination, bool shortcut) {
    InstallSteps steps;
    steps.version = std::string(pt::update::CurrentVersion());
    std::random_device random;
    char id[17];
    std::snprintf(id, sizeof(id), "%08x%08x", random(), random());
    steps.unique_id = id;
    steps.check_parents = CheckParents;
    steps.verify_integrity = VerifyIntegrity;
    // the payload is read once: its size for the progress total, then the unpack
    auto payload = std::make_shared<std::vector<unsigned char>>();
    steps.payload_bytes = [payload] {
        if (payload->empty()) *payload = Payload();
        return PayloadBytes(payload->data(), payload->size());
    };
    steps.unpack = [payload](const fs::path& staging) {
        if (payload->empty()) *payload = Payload();
        auto files = UnpackPayload(payload->data(), payload->size(), staging, HashFile);
        MarkExecutables(staging);
        return files;
    };
    steps.extract = Extract;
    steps.shortcut = Shortcut;
    return RunInstall(input, destination, shortcut, steps);
}
std::string Outcome(const InstallOutcome& outcome) {
    std::string text = outcome.updated
        ? "Updated to version " + std::string(pt::update::CurrentVersion()) + ": " + std::to_string(outcome.swap.replaced) + " files replaced, " +
              std::to_string(outcome.swap.added) + " added, " + std::to_string(outcome.swap.removed) + " removed" +
              (outcome.archives_restored ? ", game archives restored" : "") + ". Your settings and saves are unchanged."
        : "Installed. Start pt from the install folder or the application menu.";
    if (!outcome.notes.empty()) text += " Your copy differs from the tested US release; install-notes.txt in the install folder lists how.";
    return text;
}
// The desktop dialogs: zenity (GNOME and most desktops) or kdialog (KDE; Bazzite's and Kinoite's KDE images ship without
// zenity, and started from the file manager the setup then had no way to show anything)
enum class Dialogs { None, Zenity, Kdialog };
Dialogs FindDialogs() {
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) return Dialogs::None;
    if (std::system("command -v zenity >/dev/null 2>&1") == 0) return Dialogs::Zenity;
    if (std::system("command -v kdialog >/dev/null 2>&1") == 0) return Dialogs::Kdialog;
    return Dialogs::None;
}
// a text in single quotes for the shell
std::string Quote(const std::string& text) {
    std::string out = "'";
    for (char c : text) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}
std::string RunDialog(const std::string& command) {
    std::string out;
    if (FILE* pipe = popen((command + " 2>/dev/null").c_str(), "r")) {
        char buffer[4096];
        while (std::fgets(buffer, sizeof(buffer), pipe)) out += buffer;
        pclose(pipe);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}
std::string HomeDir() {
    const char* home = std::getenv("HOME");
    return home ? home : "/";
}
std::string PickDirectory(Dialogs d, const std::string& title) {
    return d == Dialogs::Zenity ? RunDialog("zenity --file-selection --directory --title=" + Quote(title))
                                : RunDialog("kdialog --title " + Quote(title) + " --getexistingdirectory " + Quote(HomeDir()));
}
std::string PickPackage(Dialogs d, const std::string& title) {
    return d == Dialogs::Zenity ? RunDialog("zenity --file-selection --title=" + Quote(title) + " --file-filter='PS4 package | *.pkg *.PKG'")
                                : RunDialog("kdialog --title " + Quote(title) + " --getopenfilename " + Quote(HomeDir()) + " 'PS4 package (*.pkg *.PKG)'");
}
std::string AskText(Dialogs d, const std::string& text, const std::string& fallback) {
    return d == Dialogs::Zenity ? RunDialog("zenity --entry --title=" + Quote(kProduct) + " --text=" + Quote(text) + " --entry-text=" + Quote(fallback))
                                : RunDialog("kdialog --title " + Quote(kProduct) + " --inputbox " + Quote(text) + " " + Quote(fallback));
}
bool AskYesNo(Dialogs d, const std::string& text) {
    const std::string command = d == Dialogs::Zenity ? "zenity --question --title=" + Quote(kProduct) + " --text=" + Quote(text)
                                                     : "kdialog --title " + Quote(kProduct) + " --yesno " + Quote(text);
    return std::system((command + " 2>/dev/null").c_str()) == 0;
}
void ShowMessage(Dialogs d, bool error, const std::string& text) {
    if (d == Dialogs::Zenity) RunDialog(std::string("zenity ") + (error ? "--error" : "--info") + " --title=" + Quote(kProduct) + " --text=" + Quote(text));
    else if (d == Dialogs::Kdialog) RunDialog("kdialog --title " + Quote(kProduct) + (error ? " --error " : " --msgbox ") + Quote(text));
}
// "~" and "~/..." as a shell would expand them: the terminal prompt and the dialogs pass the text through as typed, and
// "~/Games/PT" became a folder named "~" in the working directory
std::string ExpandHome(std::string path) {
    while (!path.empty() && (path.back() == ' ' || path.back() == '\t')) path.pop_back();
    if (path == "~" || path.starts_with("~/")) path = HomeDir() + path.substr(1);
    return path;
}
std::string Ask(const std::string& question, const std::string& fallback) {
    std::cout << question << (fallback.empty() ? "" : " [" + fallback + "]") << ": " << std::flush;
    std::string line;
    std::getline(std::cin, line);
    return line.empty() ? fallback : line;
}
int Interactive() {
    const bool terminal = isatty(0);
    const Dialogs dialogs = terminal ? Dialogs::None : FindDialogs();
    const bool gui = dialogs != Dialogs::None;
    const char* home = std::getenv("HOME");
    const std::string default_dest = home ? (fs::path(home) / ".local" / "share" / kSlug).string() : "";
    std::cout << kProduct << " setup, version " << pt::update::CurrentVersion() << "\n";
    pt::update::Checker updates;
    updates.Start();
    std::string input, destination;
    if (gui) {
        input = PickDirectory(dialogs, "Select the P.T. game folder from your dump (Cancel to pick a PKG)");
        if (input.empty()) input = PickPackage(dialogs, "Select your P.T. fake PKG");
        if (input.empty()) return 1;
        destination = ExpandHome(AskText(dialogs, "Installation folder (a new or empty one, or an existing install to update)", default_dest));
        if (destination.empty()) return 1;
    } else if (terminal) {
        std::cout << "Select your P.T. fake PKG or dumped game folder. No game assets are included.\n";
        destination = ExpandHome(Ask("Installation folder (a new or empty one, or an existing install to update)", default_dest));
        input = ExpandHome(Ask("PKG file or game folder (empty to keep the game files of an existing install)", ""));
    } else {
        std::cerr << "Run this setup from a terminal, or install zenity or kdialog for file pickers.\n";
        return 2;
    }
    // one line per step; its percentage is rewritten in place once the install is measured
    struct Line { std::string text; int percent = -1; bool open = false; };
    static Line line;
    hooks.report = [](const std::string& text) {
        if (line.open) std::cout << "\n";
        line = {text, -1, true};
        std::cout << text << std::flush;
    };
    hooks.progress = [](uint64_t done, uint64_t total) {
        const int percent = total ? int(done * 100 / total) : 0;
        if (!total || percent == line.percent) return;
        line.percent = percent;
        line.open = true;
        std::cout << "\r" << line.text << " " << percent << "%" << std::flush;
    };
    hooks.cancelled = [] { return cancel.load(); };
    std::string result;
    int code = 0;
    try {
        const auto question = UpdateQuestion(InspectInstall(destination), std::string(pt::update::CurrentVersion()));
        if (!question.empty()) {
            const bool yes = gui ? AskYesNo(dialogs, question) : Ask(question + " (y/n)", "n").starts_with("y");
            if (!yes) return 1;
        }
        result = Outcome(Install(input, destination, true));
    } catch (const std::exception& e) {
        result = e.what();
        code = 1;
    }
    for (int i = 0; i < 20 && !updates.Done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (const auto newer = updates.Newer()) result += "\nVersion " + newer->version + " is available: " + newer->url;
    if (line.open) std::cout << "\n";
    std::cout << result << "\n";
    if (gui) ShowMessage(dialogs, code != 0, result);
    return code;
}
}

int main(int argc, char** argv) {
    if (argc == 1) return Interactive();
    std::string mode = argv[1];
    const char* result_file = argv[argc - 1];
    int result = 0;
    try {
        if (mode == "--install" && argc == 5) {
            hooks.cancelled = [] { return cancel.load(); };
            static ProgressTrace trace;
            hooks.progress = [](uint64_t done, uint64_t total) { trace.Note(done, total); };
            if (!ResultPathWritable(argv[4])) throw std::runtime_error("refusing to overwrite the result file " + std::string(argv[4]));
            const auto outcome = Install(argv[2], argv[3], false);
            std::ofstream out(argv[4]);
            out << (outcome.updated ? "PASS updated" : "PASS installed");
            if (outcome.updated)
                out << "\nfrom " << (outcome.old_version.empty() ? "older install" : outcome.old_version) << " replaced " << outcome.swap.replaced << " added "
                    << outcome.swap.added << " removed " << outcome.swap.removed << " archives " << (outcome.archives_restored ? "restored" : "kept");
            for (const auto& note : outcome.notes) out << "\nnote: " << note;
            out << "\n" << trace.Summary();
        } else if (mode == "--verify-integrity" && argc == 3) {
            if (!ResultPathWritable(argv[2])) throw std::runtime_error("refusing to overwrite the result file " + std::string(argv[2]));
            VerifyIntegrity();
            std::ofstream(argv[2]) << "PASS integrity verified";
        } else if (mode == "--check-update" && argc == 3) {
            pt::update::Checker updates;
            updates.Start();
            for (int i = 0; i < 200 && !updates.Done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const auto newer = updates.Newer();
            std::ofstream(argv[2]) << "url " << pt::update::ManifestUrl() << "\nthis " << pt::update::CurrentVersion() << "\ndone " << updates.Done()
                                   << "\nnewer " << (newer ? newer->version + " " + newer->url : std::string("none"));
        } else if (mode == "--self-test" && argc == 3) {
            bool rejected = false;
            try { Contained("/tmp/test", "../escape"); } catch (...) { rejected = true; }
            if (!rejected) throw std::runtime_error("Traversal accepted");
            if (pt::update::CurrentVersion() == "0.0.0-dev" || pt::update::ManifestUrl().find("github.com/") == std::string::npos) throw std::runtime_error("Built without pt_version.h or with a placeholder manifest address");
            const fs::path root = fs::temp_directory_path() / ("pt-setup-selftest-" + std::to_string(getpid()));
            const fs::path game = root / "dump" / "CUSA01127-app";
            fs::create_directories(game);
            auto write = [](const fs::path& file, const std::string& bytes) { std::ofstream(file, std::ios::binary) << bytes; };
            std::string qar(0x40, '\0');
            qar[0x40 - 0x24 + 0x16] = 'a';
            qar[0x40 - 0x24 + 0x17] = 'q';
            write(game / "chunk1.psarc", "PSAR" + std::string(60, '\0'));
            write(game / "texture.qar", qar);
            write(game / "pathid_list_ps4.bin", "/Assets/sh/level/pt14_hallway/" + std::string(40, 'x'));
            std::string failures;
            try {
                const auto found = ResolveSource(root);
                if (found.kind != SourceKind::Folder || found.path != game || !found.files.notes.empty()) failures += " folder-not-found";
            } catch (const std::exception& e) { failures += std::string(" folder:") + e.what(); }
            write(game / "chunk1.psarc", std::string(64, 'Z'));
            rejected = false;
            try { ResolveSource(game); } catch (...) { rejected = true; }
            if (!rejected) failures += " encrypted-accepted";
            GameFiles europe;
            europe.title = "CUSA01114";
            europe.pathid = game / "pathid_list_ps4.bin";
            if (!ConfirmPt(europe) || europe.notes.empty()) failures += " region-refused";
            failures += SelfTestIcon(root / "icon");
            std::error_code error;
            fs::remove_all(root, error);
            failures += SelfTestUpdate(root / "update");
            failures += SelfTestUnicodePaths(root);
            fs::remove_all(root, error);
            if (pt::update::CompareVersions("0.10.0", "0.9.2") <= 0) failures += " version-order";
            if (!failures.empty()) throw std::runtime_error("Self test failed:" + failures);
            std::ofstream(argv[2]) << "PASS path containment, input formats, update manifest and in-place update";
        } else {
            std::cerr << "usage: " << argv[0] << " [--install <pkg or folder> <new folder> <result file> | --verify-integrity <file> | --check-update <file> | --self-test <file>]\n";
            result = 2;
        }
    } catch (const std::exception& e) {
        result = 1;
        std::cerr << e.what() << "\n";
        if (ResultPathWritable(result_file)) std::ofstream(result_file) << e.what();
    }
    return result;
}
