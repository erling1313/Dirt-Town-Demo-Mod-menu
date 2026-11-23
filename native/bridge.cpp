#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include "winmm_forwarder.h"
#include "standalone_support.h"
#include "preview_geometry.h"
#include "free_camera.h"
extern "C" BOOL WINAPI DirtTownMenuVerifyBuild(const wchar_t* filename){return standalone::exactBuild(filename)?TRUE:FALSE;}
extern "C" int WINAPI DirtTownMenuPreviewSelfTest(){return preview::selfTest();}
extern "C" int WINAPI DirtTownMenuCameraSelfTest(){return freecam::selfTest();}

struct Shared {
    uint32_t magic,request,completed,command;
    int32_t vehicle,material;
    float amount,x,z,yaw;
    uint32_t heartbeat,enabled;
    char model[64];
    char reply[8192];
};
static_assert(offsetof(Shared,reply)==112);
static Shared* shared;
static uintptr_t base;
static DWORD mainThread;
using Tick=bool(__fastcall*)(void*);
static Tick original;
static thread_local bool inside=false;
template<class T> T& at(void* p,size_t n){return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(p)+n);}
static bool readable(const void* ptr,size_t size){
    MEMORY_BASIC_INFORMATION m{};
    if(!ptr || !VirtualQuery(ptr,&m,sizeof(m)) || m.State!=MEM_COMMIT || (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    return uintptr_t(ptr)+size<=uintptr_t(m.BaseAddress)+m.RegionSize;
}
static std::string text(const char* s){
    if(!readable(s,128))return "?";
    size_t n=strnlen_s(s,127);std::string out;
    for(size_t i=0;i<n;i++){char c=s[i];if(c=='"'||c=='\\')out+='\\';if(c>=32&&c<127)out+=c;}
    return out;
}
static void reply(const std::string& value){strncpy_s(shared->reply,value.c_str(),_TRUNCATE);}
static bool worldOK(void* w){
    if(!readable(w,0x13200))return false;
    auto begin=at<uintptr_t>(w,0xE8C0),end=at<uintptr_t>(w,0xE8C8);
    return end>=begin && (end-begin)%8==0 && (end-begin)/8<256 && readable((void*)begin,size_t(end-begin));
}
static void* machine(void* w,int id){
    auto begin=at<uintptr_t>(w,0xE8C0),end=at<uintptr_t>(w,0xE8C8);
    if(id<0 || uint64_t(id)>=(end-begin)/8)return nullptr;
    void* m=reinterpret_cast<void**>(begin)[id];return readable(m,0xC00)?m:nullptr;
}
static bool materialOK(void* w,int id){
    auto begin=at<uintptr_t>(w,0x15B0),end=at<uintptr_t>(w,0x15B8);
    return end>=begin && (end-begin)%0x128==0 && id>=0 && size_t(id)<(end-begin)/0x128 && (end-begin)/0x128<128;
}
// Entfernt alle Demo- und Steuerungssperren von einem bestimmten oder allen Fahrzeugen
static int unlockMachine(void* w, int targetId = -1){
    auto specs=at<void**>(w,0xE8D8);
    if(!specs) return 0;
    size_t count=(at<uintptr_t>(w,0xE8C8)-at<uintptr_t>(w,0xE8C0))/8;
    int unlocked=0;
    for(size_t i=0;i<count;i++){
        if(targetId >= 0 && int(i) != targetId) continue;
        void* m=machine(w,(int)i);
        if(!m) continue;
        if(readable(specs+i,8)&&readable(specs[i],0x180)){
            auto spec=specs[i];
            auto len=at<uint64_t>(spec,0x170);
            auto str=at<char*>(spec,0x168);
            if(len>0&&len<512&&readable(str,size_t(len)+1)){
                str[0]=0;
                at<uint64_t>(spec,0x170)=0;
                unlocked++;
            }
            if(at<unsigned char>(spec,0x160)==1) at<unsigned char>(spec,0x160)=0;
        }
    }
    return unlocked;
}
static void snapshot(void* w){
    std::string out="{\"ok\":true,\"materials\":[";
    auto begin=at<uintptr_t>(w,0x15B0),end=at<uintptr_t>(w,0x15B8);
    size_t n=(end>=begin)?(end-begin)/0x128:0;
    if(n>128)throw "Materialtabelle inkompatibel";
    for(size_t i=0;i<n;i++){
        auto rec=(void*)(begin+i*0x128);if(!readable(rec,0x128))throw "Material nicht lesbar";
        if(i)out+=',';out+="{\"id\":"+std::to_string(i)+",\"name\":\""+text(at<char*>(rec,0))+"\"}";
    }
    out+="],\"vehicles\":[";
    size_t count=(at<uintptr_t>(w,0xE8C8)-at<uintptr_t>(w,0xE8C0))/8;bool first=true;
    for(size_t i=0;i<count;i++){
        void* m=machine(w,(int)i);if(!m)continue;
        std::string name=text(at<char*>(m,8));
        if(!first)out+=',';first=false;
        alignas(32) double position[4]{};
        auto vtable=at<void**>(m,0);
        using Position=void*(__fastcall*)(void*,void*);
        reinterpret_cast<Position>(vtable[0x88/8])(position,m);
        out+="{\"id\":"+std::to_string(i)+",\"name\":\""+name+"\",\"x\":"+std::to_string(position[0])+",\"z\":"+std::to_string(position[2])+"}";
    }
    out+="],\"selected\":"+std::to_string(at<int>(w,0xEAA0))+"}";reply(out);
}
struct GnuString{const char* data;uint64_t length;char storage[16];};
#include "steam_lobby_bridge.h"
#include "terrain_brush.h"
static void execute(void* w);
#include "integrated_menu.h"
static void execute(void* w){
    if(shared->request==shared->completed)return;
    uint32_t request=shared->request;
    try{
        if(shared->command==8){ingame::readOutbox();goto completed;}
        if(shared->command==9){ingame::acknowledge();goto completed;}
        if(shared->command==10){ingame::heartbeat();goto completed;}
        if(!worldOK(w))throw "Bitte lade zuerst einen Spielstand";
        if(shared->command>=2&&shared->command<=5)standalone::ensureBackup();
        if(shared->command==1)snapshot(w);
        else if(shared->command==2){
            void* m=machine(w,shared->vehicle);
            if(!m || text(at<char*>(m,8)).find("hauler")==std::string::npos)throw "Bitte einen Muldenkipper (Hauler) auswaehlen";
            if(!materialOK(w,shared->material))throw "Ungueltiges Material";
            if(!std::isfinite(shared->amount)||shared->amount<0||shared->amount>20)throw "Menge muss zwischen 0.1 und 20 m3 liegen (0 = voll)";
            using Fill=double(__fastcall*)(void*,void*,int,double);
            double added=reinterpret_cast<Fill>(base+0xC365B0)(w,m,shared->material,shared->amount==0?1e9:shared->amount);
            reply("{\"ok\":true,\"added\":"+std::to_string(added)+"}");
        }
        else if(shared->command==3){
            static const char* models[]={"dozer","hauler","bandvagn","truck","excavator","large_loader","medium_loader","tractor","dirtbike","snowmobile","hooklift","feeder"};
            bool valid=false;for(auto name:models)if(strcmp(shared->model,name)==0)valid=true;
            if(!valid)throw "Fahrzeugklasse wird von der Demo nicht unterstuetzt";
            if(!std::isfinite(shared->x)||!std::isfinite(shared->z)||fabs(shared->x)>10000||fabs(shared->z)>10000)throw "Ungueltige Position";
            GnuString s{shared->model,strlen(shared->model),{}};
            size_t before=(at<uintptr_t>(w,0xE8C8)-at<uintptr_t>(w,0xE8C0))/8;
            bool network=at<unsigned char>(w,0xEE18)!=0;
            std::vector<int> owners;
            auto start=at<int*>(w,0xEAF0),end=at<int*>(w,0xEAF8);
            bool hasOwners=(uintptr_t(end)>=uintptr_t(start)&&uintptr_t(end)-uintptr_t(start)==before*4&&readable(start,before*4));
            if(network){
                if(!hasOwners)throw "Fahrerregister inkompatibel";
                if(shared->vehicle<0||shared->vehicle>1024)throw "Ungueltiger Besitzer";
            }
            if(hasOwners) owners.assign(start,end);

            using Spawn=int(__fastcall*)(void*,GnuString*,float,float,float);
            auto gate=(unsigned char*)(base+0xA77180);
            unsigned char saved[7];memcpy(saved,gate,7);
            const unsigned char expectedGate[]={0x80,0xB9,0x30,0xB9,0,0,0};
            if(memcmp(saved,expectedGate,7))throw "Nativer Selektor hat sich geaendert";
            DWORD old;
            if(!VirtualProtect(gate,7,PAGE_EXECUTE_READWRITE,&old))throw "Selektor konnte nicht vorbereitet werden";
            gate[0]=0xE9;int32_t delta=int32_t(0xA779C0-0xA77180-5);memcpy(gate+1,&delta,4);gate[5]=gate[6]=0x90;FlushInstructionCache(GetCurrentProcess(),gate,7);
            int id=reinterpret_cast<Spawn>(base+0xA77150)(w,&s,shared->x,shared->z,shared->yaw);
            memcpy(gate,saved,7);DWORD ignored;VirtualProtect(gate,7,old,&ignored);FlushInstructionCache(GetCurrentProcess(),gate,7);
            if(id<0)throw "Das Spiel hat das Spawnen abgelehnt (siehe dirttown.log)";
            size_t after=(at<uintptr_t>(w,0xE8C8)-at<uintptr_t>(w,0xE8C0))/8;
            if(size_t(id)!=before||after!=before+1)throw "Unerwartete Erstellung; bitte nicht wiederholen";

            start=at<int*>(w,0xEAF0);end=at<int*>(w,0xEAF8);
            if(uintptr_t(end)>=uintptr_t(start)&&uintptr_t(end)-uintptr_t(start)==after*4&&readable(start,after*4)){
                if(!owners.empty()) memcpy(start,owners.data(),owners.size()*sizeof(int));
                start[id]=network ? shared->vehicle : 0;
            }
            unlockMachine(w, id);

            reply("{\"ok\":true,\"vehicle\":"+std::to_string(id)+"}");
        }
        else if(shared->command==4){
            int unlocked = unlockMachine(w, -1);
            reply("{\"ok\":true,\"unlocked\":"+std::to_string(unlocked)+"}");
        }
        else if(shared->command==5){
            if(!materialOK(w,shared->material))throw "Ungueltiges Material";
            if(shared->amount<0.25f||shared->amount>5||shared->yaw<0.1f||shared->yaw>3||!std::isfinite(shared->x)||!std::isfinite(shared->z))throw "Radius 0.25-5 m und Hoehe 0.1-3 m erforderlich";
            if(at<uintptr_t>(w,0x17648))throw "Gelaende wird gerade berechnet; bitte warten";
            alignas(32) unsigned char spec[0x420]{};
            for(size_t offset:{size_t(0),size_t(0x20),size_t(0x90),size_t(0xB8),size_t(0x130),size_t(0x150),size_t(0x178),size_t(0x198),size_t(0x1C0),size_t(0x220),size_t(0x270),size_t(0x2D8),size_t(0x2F8),size_t(0x368)}){
                at<char*>(spec,offset)=reinterpret_cast<char*>(spec+offset+16);
            }
            const char* type="pile";
            auto material=(void*)(at<uintptr_t>(w,0x15B0)+size_t(shared->material)*0x128);
            const char* mat=at<char*>(material,0);
            at<GnuString>(spec,0)=GnuString{type,4,{}};
            at<GnuString>(spec,0x20)=GnuString{mat,strlen(mat),{}};
            at<float>(spec,0x40)=shared->x;at<float>(spec,0x44)=shared->z;
            at<float>(spec,0x48)=shared->amount;at<float>(spec,0x4c)=shared->yaw;
            alignas(32) unsigned char event[0x1000]{};
            using Make=void*(__fastcall*)(void*,void*,void*);
            reinterpret_cast<Make>(base+0x388940)(event,w,spec);
            if(event[0x28]!=0x53)throw "Spiel hat die Haufen-Operation nicht erkannt";
            uint64_t before=at<uintptr_t>(w,0x1100)-at<uintptr_t>(w,0x10f8);
            using Apply=void(__fastcall*)(void*,void*);
            reinterpret_cast<Apply>(base+0xA02A80)(w,event);
            using Destroy=void(__fastcall*)(void*);
            reinterpret_cast<Destroy>(base+0x1318900)(event);
            uint64_t after=at<uintptr_t>(w,0x1100)-at<uintptr_t>(w,0x10f8);
            if(after<=before)throw "Spiel hat den Haufen abgelehnt (siehe dirttown.log)";
            reply("{\"ok\":true,\"queued\":true}");
        }
        else if(shared->command==6)lobbybridge::snapshot(w);
        else if(shared->command==7)lobbybridge::publish();
        else if(shared->command==11)ingame::terrainProbe(w);
        else throw "Unbekannter Befehl";
    }catch(const char* message){reply("{\"ok\":false,\"error\":\""+std::string(message)+"\"}");}
    catch(...){reply("{\"ok\":false,\"error\":\"Interner Fehler; Aktion nicht wiederholen\"}");}
completed:
    MemoryBarrier();InterlockedExchange((LONG*)&shared->completed,request);
}
static bool __fastcall hook(void* w){
    bool result=original(w);
    if(shared&&!inside&&GetCurrentThreadId()==mainThread){inside=true;shared->heartbeat=GetTickCount();if(worldOK(w)){ingame::world=w;terrain::activeWorld=w;ingame::worldSeen=GetTickCount();}execute(w);inside=false;}
    return result;
}
static void jump(unsigned char* p,const void* target){p[0]=0xFF;p[1]=0x25;memset(p+2,0,4);memcpy(p+6,&target,8);}
static DWORD WINAPI initialize(void* module){
    base=(uintptr_t)GetModuleHandleW(nullptr);
    wchar_t exe[MAX_PATH];GetModuleFileNameW(nullptr,exe,MAX_PATH);
    auto filename=wcsrchr(exe,L'\\');if(_wcsicmp(filename?filename+1:exe,L"dirttown.exe"))return 6;
    if(!standalone::exactBuild(exe))return 8;
    ingame::loadConfig((HINSTANCE)module);
    const unsigned char expected[]={0x48,0x83,0xEC,0x38,0x48,0xBA,0,0,0,0,2,0,0,0};
    auto target=(unsigned char*)(base+0x939DB0);
    Tick previous=nullptr;
    if(memcmp(target,expected,sizeof(expected))){
        if(target[0]!=0xFF||target[1]!=0x25)return 1;
        memcpy(&previous,target+6,8);MEMORY_BASIC_INFORMATION m{};
        if(!VirtualQuery((void*)previous,&m,sizeof(m))||(m.AllocationBase!=GetModuleHandleW(L"DirtTownBridge4.dll")&&m.AllocationBase!=GetModuleHandleW(L"DirtTownBridge5.dll")&&m.AllocationBase!=GetModuleHandleW(L"DirtTownBridge6.dll")&&m.AllocationBase!=GetModuleHandleW(L"DirtTownBridge8.dll")))return 1;
    }
    auto frameTarget=(unsigned char*)(base+0x1180950);
    const unsigned char frameBytes[]={0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x55,0x57,0x56,0x53,0x48,0x83,0xEC,0x58};
    if(memcmp(frameTarget,frameBytes,sizeof(frameBytes)))return 7;
    auto frameTrampoline=(unsigned char*)VirtualAlloc(nullptr,64,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!frameTrampoline)return 4;
    memcpy(frameTrampoline,frameBytes,sizeof(frameBytes));jump(frameTrampoline+sizeof(frameBytes),frameTarget+sizeof(frameBytes));
    DWORD frameProtect;VirtualProtect(frameTrampoline,64,PAGE_EXECUTE_READ,&frameProtect);FlushInstructionCache(GetCurrentProcess(),frameTrampoline,64);
    ingame::originalEnd=(ingame::EndFrame)frameTrampoline;
    auto cameraTarget=(unsigned char*)(base+0xC723E0);
    const unsigned char cameraBytes[]={0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x55,0x57,0x56,0x53,0x48,0x81,0xEC,0x78,0x02,0,0};
    if(memcmp(cameraTarget,cameraBytes,sizeof(cameraBytes)))return 9;
    auto cameraTrampoline=(unsigned char*)VirtualAlloc(nullptr,64,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!cameraTrampoline)return 4;
    memcpy(cameraTrampoline,cameraBytes,sizeof(cameraBytes));jump(cameraTrampoline+sizeof(cameraBytes),cameraTarget+sizeof(cameraBytes));
    DWORD cameraProtect;VirtualProtect(cameraTrampoline,64,PAGE_EXECUTE_READ,&cameraProtect);FlushInstructionCache(GetCurrentProcess(),cameraTrampoline,64);terrainOriginalUi=cameraTrampoline;
    wchar_t name[96];swprintf_s(name,L"Local\\DirtTownMenuBridge10-%lu",GetCurrentProcessId());
    HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Shared),name);
    if(!mapping)return 2;
    shared=(Shared*)MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared));if(!shared)return 3;
    memset(shared,0,sizeof(Shared));shared->magic=0x44544D33;
    auto trampoline=(unsigned char*)VirtualAlloc(nullptr,64,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!trampoline)return 4;
    memcpy(trampoline,expected,sizeof(expected));jump(trampoline+sizeof(expected),target+sizeof(expected));
    DWORD protect;VirtualProtect(trampoline,64,PAGE_EXECUTE_READ,&protect);FlushInstructionCache(GetCurrentProcess(),trampoline,64);
    original=previous?previous:(Tick)trampoline;
    HANDLE stopped[1024];size_t stoppedCount=0;
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);THREADENTRY32 entry{sizeof(entry)};bool failed=false;ULARGE_INTEGER oldest;oldest.QuadPart=~0ull;
    if(Thread32First(snap,&entry))do{if(entry.th32OwnerProcessID==GetCurrentProcessId()&&entry.th32ThreadID!=GetCurrentThreadId()){
        if(stoppedCount>=1024){failed=true;break;}
        HANDLE h=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_SET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,entry.th32ThreadID);
        if(!h||SuspendThread(h)==DWORD(-1)){if(h)CloseHandle(h);failed=true;break;}stopped[stoppedCount++]=h;
        FILETIME created,exited,kernel,user;
        if(GetThreadTimes(h,&created,&exited,&kernel,&user)){
            ULARGE_INTEGER time;time.LowPart=created.dwLowDateTime;time.HighPart=created.dwHighDateTime;
            if(time.QuadPart<oldest.QuadPart){oldest=time;mainThread=entry.th32ThreadID;}
        }
        CONTEXT context{};context.ContextFlags=CONTEXT_CONTROL;
        if(!GetThreadContext(h,&context)){failed=true;break;}
        if(context.Rip>=uintptr_t(target)&&context.Rip<uintptr_t(target)+sizeof(expected)){
            context.Rip=uintptr_t(trampoline)+context.Rip-uintptr_t(target);
            if(!SetThreadContext(h,&context)){failed=true;break;}
        }
        if(context.Rip>=uintptr_t(frameTarget)&&context.Rip<uintptr_t(frameTarget)+sizeof(frameBytes)){
            context.Rip=uintptr_t(frameTrampoline)+context.Rip-uintptr_t(frameTarget);if(!SetThreadContext(h,&context)){failed=true;break;}
        }
        if(context.Rip>=uintptr_t(cameraTarget)&&context.Rip<uintptr_t(cameraTarget)+sizeof(cameraBytes)){
            context.Rip=uintptr_t(cameraTrampoline)+context.Rip-uintptr_t(cameraTarget);if(!SetThreadContext(h,&context)){failed=true;break;}
        }
    }}while(Thread32Next(snap,&entry));else failed=true;CloseHandle(snap);
    if(!failed&&VirtualProtect(target,sizeof(expected),PAGE_EXECUTE_READWRITE,&protect)){
        if(!VirtualProtect(frameTarget,sizeof(frameBytes),PAGE_EXECUTE_READWRITE,&frameProtect))failed=true;
        if(!failed&&!VirtualProtect(cameraTarget,sizeof(cameraBytes),PAGE_EXECUTE_READWRITE,&cameraProtect)){DWORD ignored;VirtualProtect(frameTarget,sizeof(frameBytes),frameProtect,&ignored);failed=true;}
        if(!failed){jump(target,(void*)hook);jump(frameTarget,(void*)ingame::frame);memset(frameTarget+14,0x90,2);jump(cameraTarget,(void*)terrainUiHook);memset(cameraTarget+14,0x90,sizeof(cameraBytes)-14);DWORD ignored;VirtualProtect(cameraTarget,sizeof(cameraBytes),cameraProtect,&ignored);FlushInstructionCache(GetCurrentProcess(),cameraTarget,sizeof(cameraBytes));VirtualProtect(frameTarget,sizeof(frameBytes),frameProtect,&ignored);FlushInstructionCache(GetCurrentProcess(),frameTarget,sizeof(frameBytes));shared->enabled=1;}
        DWORD ignored;VirtualProtect(target,sizeof(expected),protect,&ignored);FlushInstructionCache(GetCurrentProcess(),target,sizeof(expected));
    }
    if(failed)shared->enabled=0;
    for(size_t i=0;i<stoppedCount;i++){ResumeThread(stopped[i]);CloseHandle(stopped[i]);}return shared->enabled?0:5;
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){DisableThreadLibraryCalls(instance);HANDLE thread=CreateThread(nullptr,0,initialize,instance,0,nullptr);if(thread)CloseHandle(thread);}return TRUE;
}
