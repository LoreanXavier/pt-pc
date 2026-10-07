#include "game/save_data.h"
#include <cstdio>
#include <fstream>
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    const auto root=std::filesystem::absolute(argv[1]);
    if(std::filesystem::exists(root)) return 2;
    std::filesystem::create_directories(root);
    int failures=0;
    const auto check=[&](bool ok,const char* name){std::printf("%s: %s\n",name,ok?"PASS":"FAIL");failures+=!ok;};
    pt::game::SaveStore store;
    check(!store.Reset(),"disabled reset rejected");
    store.SetDirectory(root,"PT_Save_Data");
    check(store.Reset(),"empty slots count as successful reset");
    check(!store.Enabled(),"successful reset closes old save job");
    store.SetDirectory(root,"PT_Save_Data");
    pt::game::SaveFile file;file.progress.floor="f050";
    check(store.Save(file) && store.Save(file),"two slots written");
    {
        const uint32_t marker=0x3150474E;
        std::fstream slot(root/"PT_Save_Data1",std::ios::binary|std::ios::in|std::ios::out);
        slot.seekp(0x70);slot.write(reinterpret_cast<const char*>(&marker),4);slot.close();
        const auto plus=store.Load();
        check(plus.has_value() && store.Save(*plus),"Game+ load and resave");
        uint32_t persisted=0;
        std::ifstream saved(root/"PT_Save_Data0",std::ios::binary);
        saved.seekg(0x70);saved.read(reinterpret_cast<char*>(&persisted),4);
        check(persisted==marker,"Game+ completion marker survives save reload");
    }
    check(store.Reset(),"existing slots reset");
    check(!std::filesystem::exists(root/"PT_Save_Data0") && !std::filesystem::exists(root/"PT_Save_Data1"),"both old slots removed");
    store.SetDirectory(root,"PT_Save_Data");
    file.progress.floor="f000";
    check(store.Save(file) && store.Load()->progress.floor=="f000","new game saving works after reset");
    check(store.Reset(),"fresh slot reset");
    store.SetDirectory(root,"PT_Save_Data");
    std::filesystem::create_directory(root/"PT_Save_Data0");
    std::ofstream(root/"PT_Save_Data0"/"blocker")<<"owned test fixture";
    check(!store.Reset(),"deletion failure reported");
    check(store.Enabled(),"failed reset retains save configuration");
    std::filesystem::remove(root/"PT_Save_Data0"/"blocker");
    std::filesystem::remove(root/"PT_Save_Data0");
    check(store.Reset(),"retry after failed deletion succeeds");
    std::filesystem::remove(root);
    return failures?1:0;
}
