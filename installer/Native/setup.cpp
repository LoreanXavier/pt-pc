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
#include "engine/platform/os.h"
#include "engine/platform/self_integrity.h"
#include "engine/platform/update_check.h"
#include "setup_core.h"

namespace fs=std::filesystem;
namespace {
using namespace pt::setup;
constexpr const wchar_t* kProduct=L"P.T. PC Port";constexpr const wchar_t* kSetupTitle=L"P.T. PC Port Setup";constexpr const wchar_t* kFolder=L"P.T. PC Port";
constexpr const wchar_t* kUninstallRegKey=L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PT_PC_Port";
constexpr UINT kStatus=WM_APP+1,kFinished=WM_APP+2,kProgress=WM_APP+3;
HWND window,pkg_edit,dest_edit,status_label,install_button,cancel_button,shortcut_check,progress_bar,update_label;
pt::update::Checker updates;constexpr UINT_PTR kUpdateTimer=1;
float dpi_scale=1.0f;HFONT body_font;
std::atomic<bool> cancel{false},busy{false};std::thread worker;
std::atomic<int> posted_permille{-1};
std::wstring status_text;int percent=-1;bool marquee=false;
ProgressTrace trace;
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
void BarAt(int permille){if(marquee)BarMarquee(false);SendMessageW(progress_bar,PBM_SETPOS,permille+1,0);SendMessageW(progress_bar,PBM_SETPOS,permille,0);}
void CheckParents(const fs::path& destination){
    for(auto p=fs::absolute(destination);!p.empty();){
        if(pt::os::IsLink(p))throw std::runtime_error("Choose a destination without junctions or symbolic links.");
        auto parent=p.parent_path();if(parent==p)break;p=parent;
    }
}
std::string HashPart(const fs::path& file,uint64_t limit){
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("SHA256 provider unavailable.");
    DWORD size=0,used=0;BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&used,0);
    std::vector<UCHAR> object(size);if(BCryptCreateHash(algorithm,&hash,object.data(),size,nullptr,0,0)<0){BCryptCloseAlgorithmProvider(algorithm,0);throw std::runtime_error("SHA256 initialization failed.");}
    HANDLE input=INVALID_HANDLE_VALUE;DWORD open_error=0;
    for(int attempt=0;attempt<100;++attempt){
        CheckCancel();
        input=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
        if(input!=INVALID_HANDLE_VALUE)break;
        open_error=GetLastError();
        if(open_error!=ERROR_SHARING_VIOLATION && open_error!=ERROR_LOCK_VIOLATION && open_error!=ERROR_ACCESS_DENIED)break;
        Sleep(100);
    }
    std::vector<char> buffer(1024*1024);bool ok=input!=INVALID_HANDLE_VALUE;DWORD read_error=0;
    uint64_t left=limit;
    while(ok && left){
        CheckCancel();DWORD got=0;
        if(!ReadFile(input,buffer.data(),DWORD(std::min<uint64_t>(buffer.size(),left)),&got,nullptr)){read_error=GetLastError();ok=false;break;}
        if(got==0)break;
        left-=got;if(BCryptHashData(hash,reinterpret_cast<PUCHAR>(buffer.data()),got,0)<0)ok=false;
    }
    if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);
    std::array<UCHAR,32> bytes{};ok=ok && BCryptFinishHash(hash,bytes.data(),bytes.size(),0)>=0;BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);
    if(!ok){
        const DWORD code=open_error && input==INVALID_HANDLE_VALUE?open_error:read_error;
        const std::string why=code==ERROR_SHARING_VIOLATION||code==ERROR_LOCK_VIOLATION?" (another program, often an antivirus, kept it open)":code==ERROR_ACCESS_DENIED?" (access denied: an antivirus or the folder's permissions)":"";
        throw std::runtime_error("Could not read or hash this file to verify its integrity: " + PathUtf8(file) + " (Windows error " + std::to_string(code) + ")" + why);
    }
    const char* hex="0123456789abcdef";std::string result;for(auto b:bytes){result+=hex[b>>4];result+=hex[b&15];}return result;
}
std::string Hash(const fs::path& file){return HashPart(file,UINT64_MAX);}
std::string HashBody(const fs::path& file){return HashPart(file,pt::trailer::BodySize(file));}
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
    using pt::os::QuoteWindowsArgument;
    std::wstring command=QuoteWindowsArgument(helper.native())+L" "+QuoteWindowsArgument(fs::absolute(package).native())+L" "+QuoteWindowsArgument(assets.native());
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    HANDLE log=CreateFileW((staging/L"install-extraction.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(log==INVALID_HANDLE_VALUE)throw std::runtime_error("Could not create extraction log.");
    HANDLE nul=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    STARTUPINFOW start{sizeof(start)};start.dwFlags=STARTF_USESTDHANDLES;start.hStdOutput=start.hStdError=log;start.hStdInput=nul;PROCESS_INFORMATION process{};
    BOOL launched=CreateProcessW(pt::os::ExtendedLengthPath(helper).c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,
                                 staging.native().size()<MAX_PATH-2?staging.c_str():nullptr,&start,&process);CloseHandle(log);if(nul!=INVALID_HANDLE_VALUE)CloseHandle(nul);
    if(!launched)throw std::runtime_error("Could not start the PKG extraction helper.");
    const uint64_t before=progress.done;
    while(WaitForSingleObject(process.hProcess,100)==WAIT_TIMEOUT){
        if(cancel){TerminateProcess(process.hProcess,1);WaitForSingleObject(process.hProcess,5000);break;}
        progress.At(before+FolderBytes(assets));
    }
    DWORD code=1;GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hThread);CloseHandle(process.hProcess);CheckCancel();
    if(code){std::ifstream input(staging/L"install-extraction.log");std::string details((std::istreambuf_iterator<char>(input)),{});throw std::runtime_error("PKG extraction failed. "+details.substr(0,600));}
}
fs::path ShortcutInstall(){
    fs::path result;PWSTR desktop=nullptr;
    if(FAILED(SHGetKnownFolderPath(FOLDERID_Desktop,0,nullptr,&desktop))){CoTaskMemFree(desktop);return result;}
    const fs::path name=fs::path(desktop)/(std::wstring(kProduct)+L".lnk");CoTaskMemFree(desktop);
    std::error_code error;if(!fs::is_regular_file(name,error))return result;
    const bool com=SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED));IShellLinkW* link=nullptr;
    if(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_IShellLinkW,reinterpret_cast<void**>(&link)))){
        IPersistFile* persist=nullptr;std::wstring target(32768,L'\0');
        if(SUCCEEDED(link->QueryInterface(IID_IPersistFile,reinterpret_cast<void**>(&persist)))){
            if(SUCCEEDED(persist->Load(name.c_str(),STGM_READ)) && SUCCEEDED(link->GetPath(target.data(),int(target.size()),nullptr,SLGP_RAWPATH))){
                target.resize(wcslen(target.c_str()));const fs::path dir=fs::path(target).parent_path();try{if(InspectInstall(dir).found)result=dir;}catch(...){}
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
        link->SetIconLocation((destination/L"pt.exe").c_str(),0);
        PWSTR desktop=nullptr;IPersistFile* persist=nullptr;
        if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop,0,nullptr,&desktop)) && SUCCEEDED(link->QueryInterface(IID_IPersistFile,reinterpret_cast<void**>(&persist)))){
            const fs::path name=fs::path(desktop)/(std::wstring(kProduct)+L".lnk");
            persist->Save(name.c_str(),TRUE);persist->Release();
        }
        CoTaskMemFree(desktop);link->Release();
    }
    CoUninitialize();
}
void RegisterUninstallEntry(const fs::path& destination){
    HKEY key=nullptr;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,kUninstallRegKey,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS)return;
    auto set_str=[&](const wchar_t* name,const std::wstring& value){
        RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),DWORD((value.size()+1)*sizeof(wchar_t)));
    };
    auto set_dword=[&](const wchar_t* name,DWORD value){
        RegSetValueExW(key,name,0,REG_DWORD,reinterpret_cast<const BYTE*>(&value),sizeof(value));
    };
    const fs::path root=FolderPath(destination);
    const std::wstring updater=(root/L"pt-updater.exe").native();
    const std::wstring quoted_updater=pt::os::QuoteWindowsArgument(updater);
    const std::wstring quoted_root=pt::os::QuoteWindowsArgument(root.native());
    set_str(L"DisplayName",kProduct);
    set_str(L"DisplayVersion",Widen(std::string(pt::update::CurrentVersion())));
    set_str(L"Publisher",kProduct);
    set_str(L"InstallLocation",root.native());
    set_str(L"DisplayIcon",(root/L"pt.exe").native()+L",0");
    set_str(L"UninstallString",quoted_updater+L" --uninstall "+quoted_root);
    set_str(L"QuietUninstallString",quoted_updater+L" --uninstall --silent "+quoted_root);
    set_dword(L"NoModify",1);
    set_dword(L"NoRepair",1);
    const uint64_t kb=FolderBytes(root)/1024;
    if(kb>0 && kb<=UINT32_MAX)set_dword(L"EstimatedSize",DWORD(kb));
    RegCloseKey(key);
}
void UnregisterUninstallEntry(){
    RegDeleteTreeW(HKEY_CURRENT_USER,kUninstallRegKey);
}
void RemoveMatchingShortcut(const fs::path& destination){
    const fs::path existing=ShortcutInstall();
    std::error_code ec;
    if(existing.empty() || !fs::equivalent(existing,FolderPath(destination),ec))return;
    PWSTR desktop=nullptr;
    if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop,0,nullptr,&desktop)) && desktop){
        fs::remove(fs::path(desktop)/(std::wstring(kProduct)+L".lnk"),ec);
    }
    CoTaskMemFree(desktop);
}
fs::path CurrentExePath(){
    std::wstring buf(32768,L'\0');
    const DWORD n=GetModuleFileNameW(nullptr,buf.data(),DWORD(buf.size()));
    buf.resize(n);
    return fs::path(buf);
}
bool PathIsInside(const fs::path& child,const fs::path& parent){
    std::error_code ec;
    const fs::path rel=fs::relative(fs::weakly_canonical(child,ec),fs::weakly_canonical(parent,ec),ec);
    if(ec || rel.empty())return false;
    const std::wstring s=rel.native();
    return !(s.size()>=2 && s[0]==L'.' && s[1]==L'.');
}
int RunUninstall(const fs::path& destination,bool silent,bool remove_data,bool self_delete){
    const fs::path root=FolderPath(destination);
    if(!InspectInstall(root).found){
        if(!silent)MessageBoxW(nullptr,(L"No P.T. PC Port installation was found in:\n"+root.native()).c_str(),kSetupTitle,MB_OK|MB_ICONERROR);
        return 1;
    }
    if(!silent){
        const std::wstring msg=L"Uninstall P.T. PC Port from:\n"+root.native()+
            (remove_data?L"\n\nProgram files, saves and settings will be removed. Continue?":L"\n\nProgram files and desktop shortcut will be removed.\nYour saves, settings (data/) and mods will be kept. Continue?");
        if(MessageBoxW(nullptr,msg.c_str(),kSetupTitle,MB_YESNO|MB_ICONQUESTION)!=IDYES)return 0;
    }
    const fs::path self=CurrentExePath();
    if(!self_delete && !self.empty() && PathIsInside(self,root)){
        std::error_code ec;
        const fs::path temp_exe=fs::temp_directory_path(ec)/(L"pt-uninstall-"+std::to_wstring(GetCurrentProcessId())+L".exe");
        if(fs::copy_file(self,temp_exe,fs::copy_options::overwrite_existing,ec)){
            std::wstring cmd=pt::os::QuoteWindowsArgument(temp_exe.native())+L" --uninstall --silent --self-delete";
            if(remove_data)cmd+=L" --remove-data";
            cmd+=L" "+pt::os::QuoteWindowsArgument(root.native());
            STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
            if(CreateProcessW(temp_exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi)){
                CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
                return 0;
            }
        }
    }
    if(self_delete){
        const fs::path locked=root/L"pt-updater.exe";
        for(int i=0;i<30;++i){
            std::error_code ec;
            if(!fs::exists(locked,ec) || fs::remove(locked,ec))break;
            Sleep(100);
        }
    }
    RemoveMatchingShortcut(root);
    const UninstallResult r=UninstallInstallFiles(root,remove_data);
    UnregisterUninstallEntry();
    if(!silent && !self_delete){
        const std::wstring done=L"P.T. PC Port has been uninstalled ("+std::to_wstring(r.removed_files)+L" files removed).";
        MessageBoxW(nullptr,done.c_str(),kSetupTitle,MB_OK|MB_ICONINFORMATION);
    }
    if(self_delete && !self.empty()){
        std::wstring cleanup=L"cmd.exe /c ping -n 2 127.0.0.1 >nul & del /f /q "+pt::os::QuoteWindowsArgument(self.native());
        STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
        if(CreateProcessW(nullptr,cleanup.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)){
            CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
        }
    }
    return 0;
}
constexpr size_t kMaxGamePath=250;
InstallOutcome Install(const fs::path& input,const fs::path& destination,bool shortcut,bool register_app=true){
    if(!destination.empty() && (FolderPath(destination)/L"pt.exe").native().size()>=kMaxGamePath)
        throw std::runtime_error("This installation folder's path is too long: the game cannot start when its path has 250 characters or more. Choose a folder with a shorter path.");
    InstallSteps steps;steps.version=std::string(pt::update::CurrentVersion());
    GUID guid{};CoCreateGuid(&guid);wchar_t id[40];StringFromGUID2(guid,id,40);std::wstring wide(id);steps.unique_id=std::string(wide.begin()+1,wide.end()-1);
    steps.check_parents=CheckParents;steps.verify_integrity=VerifyIntegrity;steps.unpack=Unpack;steps.extract=Extract;steps.shortcut=Shortcut;
    steps.validate_runtime=[](const std::vector<InstalledFile>& files){RequireProgramFiles(files,{"amd_fidelityfx_vk.dll","nvngx_dlss.dll","libxess.dll"});};
    steps.process_alive=[](uint64_t pid){return pt::os::ProcessAlive(pid);};
    steps.known_folders=[]{std::vector<fs::path> folders;for(const KNOWNFOLDERID& id:{FOLDERID_Downloads,FOLDERID_Desktop,FOLDERID_Documents}){PWSTR path=nullptr;if(SUCCEEDED(SHGetKnownFolderPath(id,0,nullptr,&path))&&path)folders.emplace_back(path);CoTaskMemFree(path);}return folders;};
    steps.payload_bytes=[]{auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(100),RT_RCDATA);if(!resource)throw std::runtime_error("Installer payload missing.");
        return PayloadBytes(static_cast<const unsigned char*>(LockResource(LoadResource(nullptr,resource))),SizeofResource(nullptr,resource));};
    auto payload=[]{auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(100),RT_RCDATA);if(!resource)throw std::runtime_error("Installer payload missing.");
        return std::pair{static_cast<const unsigned char*>(LockResource(LoadResource(nullptr,resource))),size_t(SizeofResource(nullptr,resource))};};
    steps.hash_file=Hash;
    steps.entries=[payload]{auto [data,size]=payload();return ListPayload(data,size);};
    steps.fetch=[payload](const fs::path& folder,const std::vector<PayloadEntry>& wanted){std::set<std::string> names;for(const auto& entry:wanted)names.insert(entry.path);
        auto [data,size]=payload();return UnpackPayload(data,size,folder,Hash,[&names](const std::string& name){return names.count(name)>0;});};
    if(pt::update::TestOverrides()){
        if(const char* at=std::getenv("PT_UPDATE_TEST_FAIL_AT"))steps.fail_at=std::atoi(at);
        if(const char* at=std::getenv("PT_UPDATE_TEST_PAUSE_AT"))steps.pause_at=std::atoi(at);
        if(const char* file=std::getenv("PT_UPDATE_TEST_CORRUPT"))steps.corrupt_after_swap=file;
    }
    const InstallOutcome outcome=RunInstall(input,destination,shortcut,steps);
    if(register_app)RegisterUninstallEntry(destination);
    return outcome;
}
std::wstring Outcome(const InstallOutcome& outcome){
    std::wstring text;
    if(outcome.updated)text=Widen("Updated to version "+std::string(pt::update::CurrentVersion())+": "+std::to_string(outcome.swap.replaced)+" files replaced, "+
        std::to_string(outcome.swap.added)+" added, "+std::to_string(outcome.swap.removed)+" removed"+(outcome.archives_restored?", game archives restored":"")+". Your settings and saves are unchanged.");
    else text=L"Installed. Launch pt.exe or the desktop shortcut.";
    if(!outcome.notes.empty())text+=L" Your copy differs from the tested US release; install-notes.txt in the install folder lists how.";
    if(outcome.title_picture_missing)text+=L" The start picture (sce_sys/pic1.png) is still missing: select your PKG or dump folder above and press Install again to add it.";
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
        case 17:{BROWSEINFOW browse{};browse.hwndOwner=hwnd;browse.lpszTitle=L"Select your P.T. game folder from your console dump (for example CUSA01127; it holds chunk1.psarc).";browse.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;auto item=SHBrowseForFolderW(&browse);wchar_t path[MAX_PATH];if(item && SHGetPathFromIDListW(item,path)){
            fs::path chosen(path);bool existing=false;try{existing=InspectInstall(chosen).found;}catch(...){}
            std::error_code error;const std::string installed_game=existing?InstalledGameFolder(chosen):std::string();
            const fs::path inner=chosen/Utf8Path(installed_game.empty()?std::string(kGameFolder):installed_game);
            if(existing && fs::is_directory(inner,error)){SetWindowTextW(pkg_edit,inner.c_str());SetWindowTextW(dest_edit,chosen.c_str());}
            else SetWindowTextW(pkg_edit,path);}
            CoTaskMemFree(item);break;}
        case 11:{BROWSEINFOW browse{};browse.hwndOwner=hwnd;browse.lpszTitle=L"Select a parent directory. Setup creates a new P.T. folder inside it.";browse.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;auto item=SHBrowseForFolderW(&browse);wchar_t path[MAX_PATH];if(item && SHGetPathFromIDListW(item,path)){
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
#include "updater_win.h"
#include "route_win.h"
#include "fg_route_win.h"
#include "route_queue_win.h"

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show){
    hooks.report=[](const std::string& text){ReportWide(Widen(text));};hooks.cancelled=[]{return cancel.load();};hooks.progress=ReportProgress;
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(const auto update=pt::setup::win_updater::Parse(argc,argv)){LocalFree(argv);return pt::setup::win_updater::Run(*update);}
    if(const auto queue=pt::setup::route_queue_win::ParseQueue(argc,argv)){LocalFree(argv);return pt::setup::route_queue_win::RunQueue(*queue);}
    if(const auto route=pt::setup::win_route::ParseRoute(argc,argv)){LocalFree(argv);return pt::setup::win_route::RunRoute(*route);}
    if(const auto fg=pt::setup::fg_route::ParseRoute(argc,argv)){LocalFree(argv);return pt::setup::fg_route::RunRoute(*fg);}
#ifdef PT_UPDATER_ONLY
    if(argc<2){LocalFree(argv);MessageBoxW(nullptr,L"This is the updater of P.T. PC Port. The game starts it when you choose Update the game in the PC settings.",kSetupTitle,MB_OK|MB_ICONINFORMATION);return 0;}
#endif
    bool preview=argc==3 && std::wstring(argv[1])==L"--preview";fs::path preview_file=preview?argv[2]:L"";
    if(argc>1 && !preview){
        int result=0;try{
            if(std::wstring(argv[1])==L"--uninstall"){
                bool silent=false,remove_data=false,self_delete=false;
                fs::path target;
                for(int i=2;i<argc;++i){
                    const std::wstring arg(argv[i]);
                    if(arg==L"--silent")silent=true;
                    else if(arg==L"--remove-data")remove_data=true;
                    else if(arg==L"--self-delete")self_delete=true;
                    else target=arg;
                }
                if(target.empty())target=CurrentExePath().parent_path();
                result=RunUninstall(target,silent,remove_data,self_delete);
            }
            else if(std::wstring(argv[1])==L"--install" && argc==5){window=nullptr;if(!ResultPathWritable(argv[4]))throw std::runtime_error("refusing to overwrite the result file");auto outcome=Install(argv[2],argv[3],false,false);std::ofstream out(argv[4]);
                out<<(outcome.updated?"PASS updated":"PASS installed");if(outcome.updated)out<<"\nfrom "<<(outcome.old_version.empty()?"older install":outcome.old_version)<<" replaced "<<outcome.swap.replaced<<" added "<<outcome.swap.added<<" removed "<<outcome.swap.removed<<" archives "<<(outcome.archives_restored?"restored":"kept")
                    <<"\nunchanged "<<outcome.unchanged<<" verified "<<outcome.verified<<" repaired "<<outcome.repaired<<" written "<<outcome.written_bytes<<" bytes";
                for(const auto& note:outcome.notes)out<<"\nnote: "<<note;if(outcome.title_picture_missing)out<<"\ntitle picture missing";if(outcome.recovered)out<<"\nrecovered an interrupted update";out<<"\n"<<trace.Summary();}
            else if(std::wstring(argv[1])==L"--check-update" && argc==3){
                updates.Start();for(int i=0;i<200 && !updates.Done();++i)Sleep(50);auto newer=updates.Newer();
                std::ofstream(argv[2])<<"url "<<pt::update::ManifestUrl()<<"\nthis "<<pt::update::CurrentVersion()<<"\ndone "<<updates.Done()<<"\nnewer "<<(newer?newer->version+" "+newer->url:std::string("none"));
            }
            else if(std::wstring(argv[1])==L"--range-test" && argc==6 && pt::update::TestOverrides()){if(!ResultPathWritable(argv[5]))throw std::runtime_error("refusing to overwrite the result file");
                const std::wstring url(argv[2]);std::ofstream(argv[5])<<RangeFetchTest(std::string(url.begin(),url.end()),argv[3],argv[4],Hash);}
            else if(std::wstring(argv[1])==L"--verify-integrity" && argc==3){if(!ResultPathWritable(argv[2]))throw std::runtime_error("refusing to overwrite the result file");VerifyIntegrity();std::ofstream(argv[2])<<"PASS integrity verified";}
            else if(std::wstring(argv[1])==L"--self-test" && argc==3){
                bool rejected=false;try{Contained(L"C:/test","../escape");}catch(...){rejected=true;}if(!rejected)throw std::runtime_error("Traversal accepted");
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
                failures+=SelfTestTitlePictureUpdate(root/L"title-picture",Hash);
                failures+=SelfTestJournalRecovery(root/L"journal");
                failures+=SelfTestFileUpdate(root/L"files",Hash);
                failures+=SelfTestPortableUpdate(root/L"portable",Hash);
                failures+=SelfTestFgUnlockFiles(root/L"fg-unlock");
                failures+=SelfTestUnicodePaths(root);
                failures+=SelfTestUninstall(root/L"uninstall");
                fs::remove_all(root);
                if(!failures.empty())throw std::runtime_error("Self test failed:"+failures);
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
