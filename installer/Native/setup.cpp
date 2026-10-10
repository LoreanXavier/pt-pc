#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <objidl.h>
#include <bcrypt.h>
#include <zlib.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <stdexcept>
#include <array>
#include <algorithm>
#include <optional>
#include <cwctype>
#include "engine/platform/self_integrity.h"
#include "engine/platform/update_check.h"
#include "setup_core.h"

namespace fs=std::filesystem;
namespace {
using namespace pt::setup;
constexpr const wchar_t* kProduct=L"P.T. PC Port";constexpr const wchar_t* kSetupTitle=L"P.T. PC Port Setup";constexpr const wchar_t* kFolder=L"P.T. PC Port";
// the worker thread never touches a window: it posts the status text, the progress (permille) and the result
constexpr UINT kStatus=WM_APP+1,kFinished=WM_APP+2,kProgress=WM_APP+3;
HWND window,pkg_edit,dest_edit,status_label,install_button,cancel_button,shortcut_check,progress_bar,update_label;
// a newer release, looked for once when the window opens (docs/updates.md); a timer shows it when the thread is done
pt::update::Checker updates;constexpr UINT_PTR kUpdateTimer=1;
float dpi_scale=1.0f;HFONT body_font;
std::atomic<bool> cancel{false},busy{false};std::thread worker;
std::atomic<int> posted_permille{-1};
// the status line: the current step and, once the install is measured, its percentage; the bar is a marquee until then
std::wstring status_text;int percent=-1;bool marquee=false;
ProgressTrace trace;  // the --install command line's progress record
std::wstring Widen(const std::string& s){int n=MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),nullptr,0);std::wstring w(n,L'\0');MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),w.data(),n);return w;}
std::wstring Text(HWND h){int n=GetWindowTextLengthW(h);std::wstring s(n+1,L'\0');GetWindowTextW(h,s.data(),n+1);s.resize(n);return s;}
void ReportWide(const std::wstring& text){if(window)PostMessageW(window,kStatus,0,reinterpret_cast<LPARAM>(new std::wstring(text)));}
void ReportProgress(uint64_t done,uint64_t total){
    if(!window){trace.Note(done,total);return;}
    const int permille=total?int(done*1000/total):0;
    if(permille!=posted_permille.exchange(permille))PostMessageW(window,kProgress,WPARAM(permille),0);
}
void ShowStatus(){SetWindowTextW(status_label,(percent<0?status_text:status_text+L" "+std::to_wstring(percent)+L"%").c_str());}
void BarMarquee(bool on){
    marquee=on;
    if(on){SetWindowLongPtrW(progress_bar,GWL_STYLE,GetWindowLongPtrW(progress_bar,GWL_STYLE)|PBS_MARQUEE);SendMessageW(progress_bar,PBM_SETMARQUEE,TRUE,30);return;}
    SendMessageW(progress_bar,PBM_SETMARQUEE,FALSE,0);SetWindowLongPtrW(progress_bar,GWL_STYLE,GetWindowLongPtrW(progress_bar,GWL_STYLE)&~LONG_PTR(PBS_MARQUEE));
    SendMessageW(progress_bar,PBM_SETRANGE32,0,1001);SendMessageW(progress_bar,PBM_SETPOS,0,0);
}
// the themed bar animates towards a higher position over a second and shows less than it was told; a step back is drawn at once
void BarAt(int permille){if(marquee)BarMarquee(false);SendMessageW(progress_bar,PBM_SETPOS,permille+1,0);SendMessageW(progress_bar,PBM_SETPOS,permille,0);}
// the target (when it exists) and every folder above it: no junctions or symbolic links
void CheckParents(const fs::path& destination){
    for(auto p=fs::absolute(destination);!p.empty();){
        DWORD attr=GetFileAttributesW(p.c_str());if(attr!=INVALID_FILE_ATTRIBUTES && (attr&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Choose a destination without junctions or symbolic links.");
        auto parent=p.parent_path();if(parent==p)break;p=parent;
    }
}
std::string Hash(const fs::path& file){
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("SHA256 provider unavailable.");
    DWORD size=0,used=0;BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&used,0);
    std::vector<UCHAR> object(size);if(BCryptCreateHash(algorithm,&hash,object.data(),size,nullptr,0,0)<0){BCryptCloseAlgorithmProvider(algorithm,0);throw std::runtime_error("SHA256 initialization failed.");}
    std::ifstream input(file,std::ios::binary);std::vector<char> buffer(1024*1024);bool ok=bool(input);
    while(input){CheckCancel();input.read(buffer.data(),buffer.size());if(input.gcount() && BCryptHashData(hash,reinterpret_cast<PUCHAR>(buffer.data()),ULONG(input.gcount()),0)<0)ok=false;}
    std::array<UCHAR,32> bytes{};ok=ok && input.eof() && BCryptFinishHash(hash,bytes.data(),bytes.size(),0)>=0;BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);
    if(!ok)throw std::runtime_error("Could not verify file integrity.");
    const char* hex="0123456789abcdef";std::string result;for(auto b:bytes){result+=hex[b>>4];result+=hex[b&15];}return result;
}
// the setup's own file against a corrupt download (self_integrity.h, the stamp of tools/ci/stamp_integrity.py)
void VerifyIntegrity(){
    CheckCancel();
    if(!pt::integrity::IntegrityOk())throw std::runtime_error("This setup file is damaged or was modified. Please download it again.");
}
std::vector<InstalledFile> Unpack(const fs::path& root){
    auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(100),RT_RCDATA);if(!resource)throw std::runtime_error("Installer payload missing.");
    auto loaded=LoadResource(nullptr,resource);
    return UnpackPayload(static_cast<const unsigned char*>(LockResource(loaded)),SizeofResource(nullptr,resource),root,Hash);
}
void Extract(const fs::path& staging,const fs::path& package){
    fs::path helper=staging/L"extractor/PT.PkgExtract.exe",assets=staging/L"CUSA01127";
    std::wstring command=L"\""+helper.native()+L"\" \""+fs::absolute(package).native()+L"\" \""+assets.native()+L"\"";
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    HANDLE log=CreateFileW((staging/L"install-extraction.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(log==INVALID_HANDLE_VALUE)throw std::runtime_error("Could not create extraction log.");
    HANDLE nul=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    STARTUPINFOW start{sizeof(start)};start.dwFlags=STARTF_USESTDHANDLES;start.hStdOutput=start.hStdError=log;start.hStdInput=nul;PROCESS_INFORMATION process{};
    BOOL launched=CreateProcessW(helper.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,staging.c_str(),&start,&process);CloseHandle(log);if(nul!=INVALID_HANDLE_VALUE)CloseHandle(nul);
    if(!launched)throw std::runtime_error("Could not start the PKG extraction helper.");
    const uint64_t before=progress.done;
    while(WaitForSingleObject(process.hProcess,100)==WAIT_TIMEOUT){
        if(cancel){TerminateProcess(process.hProcess,1);WaitForSingleObject(process.hProcess,5000);break;}
        progress.At(before+FolderBytes(assets));
    }
    DWORD code=1;GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hThread);CloseHandle(process.hProcess);CheckCancel();
    if(code){std::ifstream input(staging/L"install-extraction.log");std::string details((std::istreambuf_iterator<char>(input)),{});throw std::runtime_error("PKG extraction failed. "+details.substr(0,600));}
}
// The folder of the port install the desktop shortcut "P.T. PC Port" starts, or empty
fs::path ShortcutInstall(){
    fs::path result;PWSTR desktop=nullptr;
    if(FAILED(SHGetKnownFolderPath(FOLDERID_Desktop,0,nullptr,&desktop))){CoTaskMemFree(desktop);return result;}
    const fs::path name=fs::path(desktop)/(std::wstring(kProduct)+L".lnk");CoTaskMemFree(desktop);
    std::error_code error;if(!fs::is_regular_file(name,error))return result;
    const bool com=SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED));IShellLinkW* link=nullptr;
    if(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_IShellLinkW,reinterpret_cast<void**>(&link)))){
        IPersistFile* persist=nullptr;wchar_t target[MAX_PATH]{};
        if(SUCCEEDED(link->QueryInterface(IID_IPersistFile,reinterpret_cast<void**>(&persist)))){
            if(SUCCEEDED(persist->Load(name.c_str(),STGM_READ)) && SUCCEEDED(link->GetPath(target,MAX_PATH,nullptr,SLGP_RAWPATH))){
                const fs::path dir=fs::path(target).parent_path();try{if(InspectInstall(dir).found)result=dir;}catch(...){}
            }
            persist->Release();
        }
        link->Release();
    }
    if(com)CoUninitialize();
    return result;
}
void Shortcut(const fs::path& destination){
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);IShellLinkW* link=nullptr;
    if(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_IShellLinkW,reinterpret_cast<void**>(&link)))){
        link->SetPath((destination/L"pt.exe").c_str());link->SetWorkingDirectory(destination.c_str());link->SetDescription(kProduct);
        // the port's own icon (assets/pt.ico, in pt.exe), not one made from the game's icon0.png
        link->SetIconLocation((destination/L"pt.exe").c_str(),0);
        PWSTR desktop=nullptr;IPersistFile* persist=nullptr;
        if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop,0,nullptr,&desktop)) && SUCCEEDED(link->QueryInterface(IID_IPersistFile,reinterpret_cast<void**>(&persist)))){
            // the shortcut always starts what was just installed: an existing one is rewritten (it kept an update's old exe and icon)
            const fs::path name=fs::path(desktop)/(std::wstring(kProduct)+L".lnk");
            persist->Save(name.c_str(),TRUE);persist->Release();
        }
        CoTaskMemFree(desktop);link->Release();
    }
    CoUninitialize();
}
// A new install, or the update of the install in `destination` (the window asks first; the command line updates directly).
InstallOutcome Install(const fs::path& input,const fs::path& destination,bool shortcut){
    InstallSteps steps;steps.version=std::string(pt::update::CurrentVersion());
    GUID guid{};CoCreateGuid(&guid);wchar_t id[40];StringFromGUID2(guid,id,40);std::wstring wide(id);steps.unique_id=std::string(wide.begin()+1,wide.end()-1);
    steps.check_parents=CheckParents;steps.verify_integrity=VerifyIntegrity;steps.unpack=Unpack;steps.extract=Extract;steps.shortcut=Shortcut;
    steps.validate_runtime=[](const std::vector<InstalledFile>& files){RequireProgramFiles(files,{"amd_fidelityfx_vk.dll","nvngx_dlss.dll","libxess.dll"});};
    steps.payload_bytes=[]{auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(100),RT_RCDATA);if(!resource)throw std::runtime_error("Installer payload missing.");
        return PayloadBytes(static_cast<const unsigned char*>(LockResource(LoadResource(nullptr,resource))),SizeofResource(nullptr,resource));};
    return RunInstall(input,destination,shortcut,steps);
}
std::wstring Outcome(const InstallOutcome& outcome){
    std::wstring text;
    if(outcome.updated)text=Widen("Updated to version "+std::string(pt::update::CurrentVersion())+": "+std::to_string(outcome.swap.replaced)+" files replaced, "+
        std::to_string(outcome.swap.added)+" added, "+std::to_string(outcome.swap.removed)+" removed"+(outcome.archives_restored?", game archives restored":"")+". Your settings and saves are unchanged.");
    else text=L"Installed. Launch pt.exe or the desktop shortcut.";
    if(!outcome.notes.empty())text+=L" Your copy differs from the tested US release; install-notes.txt in the install folder lists how.";
    return text;
}
HWND Control(const wchar_t* kind,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id){
    HWND control=CreateWindowExW(kind==std::wstring(L"EDIT")?WS_EX_CLIENTEDGE:0,kind,text,WS_CHILD|WS_VISIBLE|style,int(x*dpi_scale),int(y*dpi_scale),int(w*dpi_scale),int(h*dpi_scale),window,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(body_font),TRUE);return control;
}
LRESULT CALLBACK WndProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
    if(message==WM_CTLCOLORSTATIC){SetBkColor(reinterpret_cast<HDC>(wp),RGB(255,255,255));return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));}
    if(message==WM_COMMAND){
        switch(LOWORD(wp)){
        case 10:{wchar_t path[32768]{};OPENFILENAMEW dialog{sizeof(dialog)};dialog.hwndOwner=hwnd;dialog.lpstrFilter=L"PS4 package (*.pkg)\0*.pkg\0\0";dialog.lpstrFile=path;dialog.nMaxFile=32768;dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;if(GetOpenFileNameW(&dialog))SetWindowTextW(pkg_edit,path);break;}
        case 17:{BROWSEINFOW browse{};browse.hwndOwner=hwnd;browse.lpszTitle=L"Select the CUSA01127 game folder from your console dump (it holds chunk1.psarc).";browse.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;auto item=SHBrowseForFolderW(&browse);wchar_t path[MAX_PATH];if(item && SHGetPathFromIDListW(item,path)){
            // a port install picked here (the folder players mean by "select your folder" when they update) is the update target, and
            // its own CUSA01127 the game source: an update chosen this way had gone into a second install in the default folder
            fs::path chosen(path);bool existing=false;try{existing=InspectInstall(chosen).found;}catch(...){}
            std::error_code error;const fs::path inner=chosen/L"CUSA01127";
            if(existing && fs::is_directory(inner,error)){SetWindowTextW(pkg_edit,inner.c_str());SetWindowTextW(dest_edit,chosen.c_str());}
            else SetWindowTextW(pkg_edit,path);}
            CoTaskMemFree(item);break;}
        case 11:{BROWSEINFOW browse{};browse.hwndOwner=hwnd;browse.lpszTitle=L"Select a parent directory. Setup creates a new P.T. folder inside it.";browse.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;auto item=SHBrowseForFolderW(&browse);wchar_t path[MAX_PATH];if(item && SHGetPathFromIDListW(item,path)){
            // a folder that already holds the port is updated in place; anything else gets a new P.T. folder inside it (an update
            // pointed at the install itself went into "<install>\P.T. PC Port" and the old shortcut kept starting 1.0.1)
            fs::path chosen(path);bool existing=false;try{existing=InspectInstall(chosen).found;}catch(...){}
            SetWindowTextW(dest_edit,(existing?chosen:chosen/kFolder).c_str());}CoTaskMemFree(item);break;}
        case 12:{if(busy)break;if(worker.joinable())worker.join();auto package=Text(pkg_edit),destination=Text(dest_edit);
            try{const auto question=UpdateQuestion(InspectInstall(destination),std::string(pt::update::CurrentVersion()));
                if(!question.empty() && MessageBoxW(hwnd,Widen(question).c_str(),kSetupTitle,MB_YESNO|MB_ICONQUESTION)!=IDYES)break;}
            catch(const std::exception& e){SetWindowTextW(status_label,Widen(e.what()).c_str());break;}bool shortcut=SendMessageW(shortcut_check,BM_GETCHECK,0,0)==BST_CHECKED;busy=true;cancel=false;for(int id:{10,11,12,14,15,16,17})EnableWindow(GetDlgItem(hwnd,id),FALSE);SetWindowTextW(cancel_button,L"Cancel");
            percent=-1;posted_permille=-1;BarMarquee(true);
            worker=std::thread([package,destination,shortcut]{std::wstring result;bool ok=false;try{result=Outcome(Install(package,destination,shortcut));ok=true;}catch(const std::exception& e){result=Widen(e.what());}PostMessageW(window,kFinished,ok,reinterpret_cast<LPARAM>(new std::wstring(result)));});break;}
        case 13:if(busy)cancel=true;else DestroyWindow(hwnd);break;
        }
        return 0;
    }
    if(message==kStatus){auto text=reinterpret_cast<std::wstring*>(lp);status_text=*text;delete text;ShowStatus();return 0;}
    if(message==kProgress){BarAt(int(wp));percent=int(wp)/10;ShowStatus();return 0;}
    if(message==kFinished){
        auto text=reinterpret_cast<std::wstring*>(lp);busy=false;percent=-1;status_text=*text;delete text;ShowStatus();
        if(marquee)BarMarquee(false);if(wp)BarAt(1000);
        SetWindowTextW(cancel_button,L"Close");for(int id:{10,11,14,15,16,17})EnableWindow(GetDlgItem(hwnd,id),TRUE);EnableWindow(install_button,!wp);if(wp)SetWindowTextW(install_button,L"Installed");return 0;
    }
    if(message==WM_CLOSE){if(busy){cancel=true;return 0;}DestroyWindow(hwnd);return 0;}
    if(message==WM_TIMER && wp==kUpdateTimer){
        if(updates.Done()){KillTimer(hwnd,kUpdateTimer);if(auto newer=updates.Newer())SetWindowTextW(update_label,Widen("Version "+newer->version+" is available: "+newer->url).c_str());}
        return 0;
    }
    if(message==WM_DESTROY){PostQuitMessage(0);return 0;}
    return DefWindowProcW(hwnd,message,wp,lp);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show){
    hooks.report=[](const std::string& text){ReportWide(Widen(text));};hooks.cancelled=[]{return cancel.load();};hooks.progress=ReportProgress;
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    bool preview=argc==3 && std::wstring(argv[1])==L"--preview";fs::path preview_file=preview?argv[2]:L"";
    if(argc>1 && !preview){
        int result=0;try{
            if(std::wstring(argv[1])==L"--install" && argc==5){window=nullptr;if(!ResultPathWritable(argv[4]))throw std::runtime_error("refusing to overwrite the result file");auto outcome=Install(argv[2],argv[3],false);std::ofstream out(argv[4]);
                out<<(outcome.updated?"PASS updated":"PASS installed");if(outcome.updated)out<<"\nfrom "<<(outcome.old_version.empty()?"older install":outcome.old_version)<<" replaced "<<outcome.swap.replaced<<" added "<<outcome.swap.added<<" removed "<<outcome.swap.removed<<" archives "<<(outcome.archives_restored?"restored":"kept");
                for(const auto& note:outcome.notes)out<<"\nnote: "<<note;out<<"\n"<<trace.Summary();}
            else if(std::wstring(argv[1])==L"--check-update" && argc==3){
                updates.Start();for(int i=0;i<200 && !updates.Done();++i)Sleep(50);auto newer=updates.Newer();
                std::ofstream(argv[2])<<"url "<<pt::update::ManifestUrl()<<"\nthis "<<pt::update::CurrentVersion()<<"\ndone "<<updates.Done()<<"\nnewer "<<(newer?newer->version+" "+newer->url:std::string("none"));
            }
            else if(std::wstring(argv[1])==L"--verify-integrity" && argc==3){if(!ResultPathWritable(argv[2]))throw std::runtime_error("refusing to overwrite the result file");VerifyIntegrity();std::ofstream(argv[2])<<"PASS integrity verified";}
            else if(std::wstring(argv[1])==L"--self-test" && argc==3){
                bool rejected=false;try{Contained(L"C:/test","../escape");}catch(...){rejected=true;}if(!rejected)throw std::runtime_error("Traversal accepted");
                // input formats: a dump folder found below the picked folder, other names found by content, an encrypted copy refused,
                // other regions installed with a note, another game refused
                GUID guid{};CoCreateGuid(&guid);wchar_t id[40];StringFromGUID2(guid,id,40);const fs::path root=fs::temp_directory_path()/(std::wstring(L"pt-setup-selftest-")+id);
                const fs::path game=root/L"dump"/L"CUSA01127-app";fs::create_directories(game);
                auto write=[](const fs::path& file,const std::string& bytes){std::ofstream(file,std::ios::binary)<<bytes;};
                std::string qar(0x40,'\0');qar[0x40-0x24+0x16]='a';qar[0x40-0x24+0x17]='q';
                write(game/L"chunk1.psarc","PSAR"+std::string(60,'\0'));write(game/L"texture.qar",qar);write(game/L"pathid_list_ps4.bin","/Assets/sh/level/pt14_hallway/"+std::string(40,'x'));
                std::string failures;
                try{auto found=ResolveSource(root);if(found.kind!=SourceKind::Folder || found.path!=game || !found.files.notes.empty())failures+=" folder-not-found";}catch(const std::exception& e){failures+=std::string(" folder:")+e.what();}
                fs::rename(game/L"chunk1.psarc",game/L"data_ps4.psarc");
                try{auto found=ResolveSource(game);if(found.files.psarc.filename()!=L"data_ps4.psarc" || found.files.notes.size()!=1)failures+=" renamed-not-found";}catch(const std::exception& e){failures+=std::string(" renamed:")+e.what();}
                write(game/L"data_ps4.psarc",std::string(64,'\x5a'));
                bool refused=false;try{ResolveSource(game);}catch(...){refused=true;}if(!refused)failures+=" encrypted-accepted";
                GameFiles europe;europe.title="CUSA01114";europe.pathid=game/L"pathid_list_ps4.bin";if(!ConfirmPt(europe) || europe.notes.empty() || europe.notes[0].find("Europe")==std::string::npos)failures+=" region-refused";
                failures+=SelfTestIcon(root/L"icon");
                write(game/L"other_list.bin","/Assets/other/level/"+std::string(40,'x'));GameFiles other;other.title="CUSA99999";other.pathid=game/L"other_list.bin";if(ConfirmPt(other))failures+=" other-game-accepted";
                fs::remove_all(root);
                failures+=SelfTestUpdate(root/L"update");
                failures+=SelfTestUnicodePaths(root);
                fs::remove_all(root);
                if(!failures.empty())throw std::runtime_error("Self test failed:"+failures);
                // the update check: version order and the manifest (docs/updates.md)
                using pt::update::CompareVersions;
                if(!(CompareVersions("0.10.0","0.9.2")>0 && CompareVersions("v0.2.0","0.2.0")==0 && CompareVersions("0.2.0-rc1","0.2.0")<0 && CompareVersions("0.1","0.1.0")==0))throw std::runtime_error("Version order failure");
                if(pt::update::CurrentVersion()=="0.0.0-dev" || pt::update::ManifestUrl().find("github.com/")==std::string::npos)throw std::runtime_error("Built without pt_version.h or with a placeholder manifest address");
                auto release=pt::update::ParseManifest(R"({"version":"0.2.0","notes":"x","url":"https://example.org/r","platforms":{"windows":{"url":"https://example.org/w.exe"},"linux":{"url":"https://example.org/l"}}})","windows");
                if(!release || release->version!="0.2.0" || release->url!="https://example.org/w.exe" || pt::update::ParseManifest("{\"notes\":1}","windows"))throw std::runtime_error("Manifest parse failure");
                std::ofstream(argv[2])<<"PASS path containment, input formats, update manifest and in-place update";
            }else result=2;
        }catch(const std::exception& e){result=1;if(ResultPathWritable(argv[argc-1]))std::ofstream(argv[argc-1])<<e.what();}
        LocalFree(argv);return result;
    }LocalFree(argv);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    dpi_scale=GetDpiForSystem()/96.0f;body_font=CreateFontW(int(-15*dpi_scale),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_PROGRESS_CLASS};InitCommonControlsEx(&common);
    WNDCLASSW cls{};cls.lpfnWndProc=WndProc;cls.hInstance=instance;cls.lpszClassName=L"PTSetup";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassW(&cls);
    DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;RECT bounds{0,0,int(620*dpi_scale),int(385*dpi_scale)};AdjustWindowRect(&bounds,style,FALSE);
    window=CreateWindowExW(0,cls.lpszClassName,kSetupTitle,style,CW_USEDEFAULT,CW_USEDEFAULT,bounds.right-bounds.left,bounds.bottom-bounds.top,nullptr,nullptr,instance,nullptr);
    auto title=Control(L"STATIC",kProduct,0,24,20,560,35,0);HFONT font=CreateFontW(int(-25*dpi_scale),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");SendMessageW(title,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    Control(L"STATIC",L"Select your P.T. fake PKG or dumped game folder. No game assets are included.",0,24,65,572,24,0);
    std::wstring line=L"Version "+Widen(std::string(pt::update::CurrentVersion()));Control(L"STATIC",line.c_str(),0,24,90,572,24,0);
    pkg_edit=Control(L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,24,128,376,28,14);Control(L"BUTTON",L"PKG...",WS_TABSTOP,410,128,90,28,10);Control(L"BUTTON",L"Folder...",WS_TABSTOP,506,128,90,28,17);
    PWSTR local=nullptr;SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local);std::wstring dest=local?(fs::path(local)/L"Programs"/kFolder).native():L"";CoTaskMemFree(local);
    // an earlier install that the desktop shortcut starts is the default, so an update lands on it without picking a folder
    if(const fs::path previous=ShortcutInstall();!previous.empty())dest=previous.native();
    dest_edit=Control(L"EDIT",dest.c_str(),ES_AUTOHSCROLL|WS_TABSTOP,24,166,472,28,15);Control(L"BUTTON",L"Location...",WS_TABSTOP,506,166,90,28,11);
    shortcut_check=Control(L"BUTTON",L"Create desktop shortcut with your game icon",BS_AUTOCHECKBOX|WS_TABSTOP,24,205,420,24,16);SendMessageW(shortcut_check,BM_SETCHECK,BST_CHECKED,0);
    progress_bar=Control(PROGRESS_CLASSW,L"",PBS_MARQUEE,24,242,572,15,0);status_label=Control(L"STATIC",L"Select your PKG or dumped game folder, and a new installation folder.",0,24,271,572,55,0);
    update_label=Control(L"STATIC",L"",0,24,337,364,30,0);
    install_button=Control(L"BUTTON",L"Install",BS_DEFPUSHBUTTON|WS_TABSTOP,398,337,94,30,12);cancel_button=Control(L"BUTTON",L"Close",WS_TABSTOP,502,337,94,30,13);
    if(preview){
        RECT rect{};GetClientRect(window,&rect);HDC screen=GetDC(window),memory=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,rect.right,rect.bottom);auto old=SelectObject(memory,bitmap);
        SendMessageW(window,WM_PRINT,reinterpret_cast<WPARAM>(memory),PRF_CLIENT|PRF_CHILDREN|PRF_ERASEBKGND);
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=rect.right;info.bmiHeader.biHeight=-rect.bottom;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;std::vector<char> pixels(size_t(rect.right)*rect.bottom*4);GetDIBits(memory,bitmap,0,rect.bottom,pixels.data(),&info,DIB_RGB_COLORS);
        BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(info.bmiHeader);header.bfSize=header.bfOffBits+DWORD(pixels.size());std::ofstream file(preview_file,std::ios::binary);file.write(reinterpret_cast<char*>(&header),sizeof(header));file.write(reinterpret_cast<char*>(&info.bmiHeader),sizeof(info.bmiHeader));file.write(pixels.data(),pixels.size());
        SelectObject(memory,old);DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(window,screen);DestroyWindow(window);DeleteObject(font);DeleteObject(body_font);CoUninitialize();return 0;
    }
    updates.Start();SetTimer(window,kUpdateTimer,500,nullptr);
    ShowWindow(window,show);MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    if(worker.joinable())worker.join();DeleteObject(font);DeleteObject(body_font);CoUninitialize();return 0;
}
