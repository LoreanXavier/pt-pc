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
void CheckParents(const fs::path& destination) {
    for (auto p = fs::absolute(destination); !p.empty();) {
        std::error_code error;
        if (fs::is_symlink(p, error)) throw std::runtime_error("Choose a destination without symbolic links.");
        const auto parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
}
void VerifyIntegrity() {
    CheckCancel();
    if (!pt::integrity::IntegrityOk()) throw std::runtime_error("This setup file is damaged or was modified. Please download it again.");
}
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
void Shortcut(const fs::path& destination) {
    const char* home = std::getenv("HOME");
    if (!home) return;
    const fs::path dir = fs::path(home) / ".local" / "share" / "applications";
    std::error_code error;
    fs::create_directories(dir, error);
    fs::path file = dir / (std::string(kSlug) + ".desktop");
    if (fs::exists(file)) file = dir / (std::string(kSlug) + "-" + destination.filename().string() + ".desktop");
    if (fs::exists(file)) return;
    std::ofstream out(file);
    out << "[Desktop Entry]\nType=Application\nName=" << kProduct << "\nComment=" << kProduct << "\nExec=\""
        << (destination / "pt").string() << "\"\nPath=" << destination.string() << "\nTerminal=false\nCategories=Game;\n";
}
InstallOutcome Install(const fs::path& input, const fs::path& destination, bool shortcut) {
    InstallSteps steps;
    steps.version = std::string(pt::update::CurrentVersion());
    std::random_device random;
    char id[17];
    std::snprintf(id, sizeof(id), "%08x%08x", random(), random());
    steps.unique_id = id;
    steps.check_parents = CheckParents;
    steps.verify_integrity = VerifyIntegrity;
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
bool HasZenity() {
    return (std::getenv("DISPLAY") || std::getenv("WAYLAND_DISPLAY")) && std::system("command -v zenity >/dev/null 2>&1") == 0;
}
std::string Quote(const std::string& text) {
    std::string out = "'";
    for (char c : text) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}
std::string Zenity(const std::string& args) {
    std::string out;
    if (FILE* pipe = popen(("zenity " + args + " 2>/dev/null").c_str(), "r")) {
        char buffer[4096];
        while (std::fgets(buffer, sizeof(buffer), pipe)) out += buffer;
        pclose(pipe);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}
std::string Ask(const std::string& question, const std::string& fallback) {
    std::cout << question << (fallback.empty() ? "" : " [" + fallback + "]") << ": " << std::flush;
    std::string line;
    std::getline(std::cin, line);
    return line.empty() ? fallback : line;
}
int Interactive() {
    const bool terminal = isatty(0);
    const bool gui = !terminal && HasZenity();
    const char* home = std::getenv("HOME");
    const std::string default_dest = home ? (fs::path(home) / ".local" / "share" / kSlug).string() : "";
    std::cout << kProduct << " setup, version " << pt::update::CurrentVersion() << "\n";
    pt::update::Checker updates;
    updates.Start();
    std::string input, destination;
    if (gui) {
        input = Zenity("--file-selection --directory --title='Select the P.T. game folder from your dump (Cancel to pick a PKG)'");
        if (input.empty()) input = Zenity("--file-selection --title='Select your P.T. fake PKG' --file-filter='PS4 package | *.pkg *.PKG'");
        if (input.empty()) return 1;
        destination = Zenity("--entry --title='" + std::string(kProduct) + "' --text='New installation folder' --entry-text=" + Quote(default_dest));
        if (destination.empty()) return 1;
    } else if (terminal) {
        std::cout << "Select your P.T. fake PKG or dumped game folder. No game assets are included.\n";
        destination = Ask("Installation folder (a new one, or an existing install to update)", default_dest);
        input = Ask("PKG file or game folder (empty to keep the game files of an existing install)", "");
    } else {
        std::cerr << "Run this setup from a terminal, or install zenity for file pickers.\n";
        return 2;
    }
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
            const bool yes = gui ? std::system(("zenity --question --title='" + std::string(kProduct) + "' --text=" + Quote(question) + " 2>/dev/null").c_str()) == 0
                                 : Ask(question + " (y/n)", "n").starts_with("y");
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
    if (gui) Zenity(std::string(code ? "--error" : "--info") + " --title='" + kProduct + "' --text=" + Quote(result));
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
            const auto outcome = Install(argv[2], argv[3], false);
            std::ofstream out(argv[4]);
            out << (outcome.updated ? "PASS updated" : "PASS installed");
            if (outcome.updated)
                out << "\nfrom " << (outcome.old_version.empty() ? "older install" : outcome.old_version) << " replaced " << outcome.swap.replaced << " added "
                    << outcome.swap.added << " removed " << outcome.swap.removed << " archives " << (outcome.archives_restored ? "restored" : "kept");
            for (const auto& note : outcome.notes) out << "\nnote: " << note;
            out << "\n" << trace.Summary();
        } else if (mode == "--verify-integrity" && argc == 3) {
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
        std::ofstream(result_file) << e.what();
    }
    return result;
}
