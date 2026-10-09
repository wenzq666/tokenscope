// Exercise the actual native dialog and preferences code without wWinMain,
// real user directories, tray registration, credentials or network requests.
#include "../src/main.cpp"
#include <iostream>
#include <functional>
#include <new>
#include <stdexcept>

namespace {
void UiRequire(bool condition,const char* detail) {
    if(!condition)throw std::runtime_error(detail);
}
struct UiEnvironment {
    std::map<std::wstring,std::wstring> original;
    explicit UiEnvironment(const fs::path& root) {
        for(const auto* key:{L"USERPROFILE",L"APPDATA",L"LOCALAPPDATA",L"CODEX_HOME",L"CLAUDE_CONFIG_DIR",L"XDG_DATA_HOME",L"KIMI_SHARE_DIR",L"KIMI_CODE_HOME",L"QWEN_RUNTIME_DIR",L"QWEN_HOME",L"GEMINI_CLI_HOME",L"XDG_CONFIG_HOME",L"TOKENS_CONFIG_DIR",L"CODEBUFF_DATA_DIR",L"REASONIX_STATE_HOME",L"REASONIX_HOME",L"JCODE_HOME",L"GROK_HOME"}) {
            const DWORD size=GetEnvironmentVariableW(key,nullptr,0);
            std::wstring previous(size,L'\0');
            if(size){GetEnvironmentVariableW(key,previous.data(),size);previous.resize(size-1);}
            original[key]=previous;SetEnvironmentVariableW(key,nullptr);
        }
        SetEnvironmentVariableW(L"USERPROFILE",(root/L"测试用户").c_str());
        SetEnvironmentVariableW(L"APPDATA",(root/L"测试漫游").c_str());
        SetEnvironmentVariableW(L"LOCALAPPDATA",(root/L"测试本地").c_str());
        SetEnvironmentVariableW(L"CODEX_HOME",(root/L"测试用户"/L".codex").c_str());
        SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR",(root/L"测试用户"/L".claude").c_str());
        SetEnvironmentVariableW(L"TOKENS_CONFIG_DIR",(root/L"测试漫游"/L"tokens").c_str());
    }
    ~UiEnvironment(){for(const auto& [key,value]:original)SetEnvironmentVariableW(key.c_str(),value.empty()?nullptr:value.c_str());}
};
void UiWrite(const fs::path& path,const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path,std::ios::binary);file.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
    UiRequire(file.good(),"cannot write owned UI fixture");
}
std::string UiRead(const fs::path& path) {
    std::ifstream input(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
std::string UiUsage(const std::string& id,int tokens,const std::string& source="codex") {
    return "{\"schema\":\"tokenscope.usage.v1\",\"source\":\""+source+"\",\"id\":\""+id+"\",\"timestamp\":\"2026-10-08T10:00:00+08:00\",\"model\":\"fixture-model\",\"usage\":{\"input_tokens\":"+std::to_string(tokens)+",\"output_tokens\":2}}";
}
const SourceStatus& UiSource(const std::string& id) {
    auto found=std::find_if(state.snapshot.sources.begin(),state.snapshot.sources.end(),[&](const auto& source){return source.id==id;});
    UiRequire(found!=state.snapshot.sources.end(),"source status absent from actual collector result");return *found;
}
std::wstring UiOverride(const std::string& id) {
    for(const auto& entry:configuredImports)if(entry.first==id)return entry.second;
    return {};
}
bool UiOwnWindow(HWND window,const wchar_t* klass) {
    DWORD pid=0;GetWindowThreadProcessId(window,&pid);
    if(pid!=GetCurrentProcessId()||GetWindow(window,GW_OWNER)!=mainWindow)return false;
    wchar_t actual[128]{};GetClassNameW(window,actual,128);return wcscmp(actual,klass)==0;
}
struct UiFindDialog {const wchar_t* klass;HWND window=nullptr;};
BOOL CALLBACK UiEnumDialog(HWND window,LPARAM parameter) {
    auto* query=reinterpret_cast<UiFindDialog*>(parameter);
    if(UiOwnWindow(window,query->klass)){query->window=window;return FALSE;}return TRUE;
}
constexpr UINT UiConnectionDuringPrice=WM_APP+80,UiOnlineDuringPrice=WM_APP+81;
bool uiObservedQueuedRefresh=false;
LRESULT CALLBACK UiTestWindowProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    if(message==UiConnectionDuringPrice||message==UiOnlineDuringPrice) {
        WindowProc(window,message==UiConnectionDuringPrice?WM_CONNECTION:WM_ONLINE,wParam,lParam);
        uiObservedQueuedRefresh=priceDialogActive&&!state.collecting&&importRefreshPending;
        return 0;
    }
    return WindowProc(window,message,wParam,lParam);
}
// This driver touches only this test PID's dialogs owned by its hidden window.
// Timeout posts WM_QUIT to this test thread, so a broken modal cannot hang CI.
class UiDialogDriver {
    std::thread worker;
public:
    std::atomic_bool operated{false},masked{false},initialMatched{false},timedOut{false};
    UiDialogDriver(const wchar_t* klass,int edit,int command,std::wstring value={},std::wstring initial={},bool checkInitial=false,std::function<void(HWND)> beforeAction={}) {
        const DWORD uiThread=GetCurrentThreadId();
        worker=std::thread([this,klass,edit,command,value=std::move(value),initial=std::move(initial),checkInitial,uiThread,beforeAction=std::move(beforeAction)] {
            const ULONGLONG deadline=GetTickCount64()+15000;
            while(GetTickCount64()<deadline) {
                UiFindDialog query{klass};EnumWindows(UiEnumDialog,reinterpret_cast<LPARAM>(&query));
                if(query.window) {
                    const auto control=GetDlgItem(query.window,edit);
                    if(control) {
                        masked=(GetWindowLongPtrW(control,GWL_STYLE)&ES_PASSWORD)!=0;
                        initialMatched=!checkInitial||EditText(control)==initial;
                        if(command==IDOK)SetDlgItemTextW(query.window,edit,value.c_str());
                        if(beforeAction)beforeAction(query.window);
                        // IDOK is used only for local path/price data. Connection
                        // dialogs always receive IDCANCEL and never a token.
                        operated=PostMessageW(query.window,WM_COMMAND,MAKEWPARAM(command,BN_CLICKED),0)!=FALSE;
                        return;
                    }
                }
                Sleep(10);
            }
            timedOut=true;PostThreadMessageW(uiThread,WM_QUIT,1,0);
        });
    }
    void Join(){if(worker.joinable())worker.join();}
    ~UiDialogDriver(){Join();}
};
void UiPumpCollection() {
    const auto deadline=GetTickCount64()+15000;
    while(state.collecting&&GetTickCount64()<deadline) {
        MSG message{};
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            UiRequire(message.message!=WM_QUIT,"UI modal watchdog aborted the test");
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        if(state.collecting)MsgWaitForMultipleObjectsEx(0,nullptr,20,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    }
    UiRequire(!state.collecting&&!collector.joinable(),"actual collector did not finish through WM_IMPORTED");
    UiRequire(state.snapshot.error.empty(),"actual collector reported a database error");
}
void UiClass(const wchar_t* name,WNDPROC proc) {
    WNDCLASSEXW klass{};klass.cbSize=sizeof(klass);klass.hInstance=instance;klass.lpfnWndProc=proc;
    klass.lpszClassName=name;klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);klass.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    UiRequire(RegisterClassExW(&klass)!=0,"cannot register owned test window class");
}
void UiStop() {
    cancelled=true;
    if(mainWindow&&IsWindow(mainWindow))DestroyWindow(mainWindow);
    mainWindow=nullptr;
    for(auto* worker:{&collector,&networkWorker,&exchangeWorker,&connectionWorker})if(worker->joinable())worker->join();
    MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){}
    // Close the global production Store before deleting only our fixture tree;
    // reconstruct it so its eventual automatic destructor remains valid.
    store.~Store();new (&store) Store;
}
void UiRemoveOwned(const fs::path& root,const fs::path& build) {
    const auto absolute=fs::absolute(root).lexically_normal();const auto parent=fs::absolute(build).lexically_normal();
    const auto attrs=GetFileAttributesW(absolute.c_str());
    UiRequire(absolute.parent_path()==parent&&absolute.filename().wstring().starts_with(L"main-ui-fixture-")&&attrs!=INVALID_FILE_ATTRIBUTES&&!(attrs&FILE_ATTRIBUTE_REPARSE_POINT),"UI fixture cleanup escaped owned build directory");
    fs::remove_all(absolute);
}
}

int main() {
    const auto build=fs::absolute(fs::current_path()/L"build").lexically_normal();
    const auto root=build/(L"main-ui-fixture-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    bool com=false;
    std::unique_ptr<UiEnvironment> isolation;
    try {
        fs::create_directories(root);isolation=std::make_unique<UiEnvironment>(root);
        instance=GetModuleHandleW(nullptr);com=SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED));
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};UiRequire(InitCommonControlsEx(&controls)!=FALSE,"cannot initialize native controls");
        dataDirectory=(root/L"data").wstring();settingsPath=(fs::path(dataDirectory)/L"settings.ini").wstring();
        const auto legacy=root/L"legacy"/L"usage.json";UiWrite(legacy,UiUsage("legacy-account",10));
        UiWrite(root/L"测试用户"/L".codex"/L"sessions"/L"真实记录.json",UiUsage("default-account",20));
        // Existing pre-upgrade ANSI INI with an unrelated section must survive
        // migration, Unicode edits and removal of the obsolete import key.
        UiWrite(settingsPath,"[TokenScope]\r\nRange=1\r\nImportCodex="+WideToUtf8(legacy.wstring())+"\r\n[UnrelatedSection]\r\nKeepMe=retained value\r\n");
        std::string error;UiRequire(store.Open((fs::path(dataDirectory)/L"usage.sqlite3").wstring(),error),error.c_str());
        LoadPreferences();UiRequire(UiOverride("codex")==legacy.wstring(),"legacy import path was not migrated into current preferences");
        auto bytes=UiRead(settingsPath);UiRequire(bytes.size()>=2&&static_cast<unsigned char>(bytes[0])==0xFF&&static_cast<unsigned char>(bytes[1])==0xFE,"old ANSI settings were not atomically upgraded to UTF-16LE");
        wchar_t retained[128]{};GetPrivateProfileStringW(L"UnrelatedSection",L"KeepMe",L"",retained,128,settingsPath.c_str());
        UiRequire(std::wstring(retained)==L"retained value","INI encoding upgrade lost an unknown section");
        store.ScanSources(configuredImports,cancelled);state.snapshot=store.ReadSnapshot();
        UiRequire(state.snapshot.events==1&&UiSource("codex").customPath,"legacy import did not isolate the default account");
        UiClass(L"TokenScopeMainUiTest",UiTestWindowProc);UiClass(L"TokenScopeSourcePath",SourcePathDialogProc);UiClass(L"TokenScopeConnection",ConnectionDialogProc);UiClass(L"TokenScopePrice",PriceDialogProc);
        mainWindow=CreateWindowExW(0,L"TokenScopeMainUiTest",L"TokenScope isolated UI test",WS_OVERLAPPEDWINDOW,0,0,900,700,nullptr,nullptr,instance,nullptr);
        UiRequire(mainWindow!=nullptr,"cannot create hidden test owner");
        const auto missing=(root/L"另一个账户"/L"中文🔒目录"/L"不存在的用量.json").wstring();
        {
            UiDialogDriver driver(L"TokenScopeSourcePath",80,IDOK,missing,legacy.wstring(),true);
            ConfigureSourcePath("codex");driver.Join();
            UiRequire(driver.operated&&!driver.timedOut&&driver.initialMatched,"native source dialog did not show and save the legacy path");
        }
        UiPumpCollection();
        const auto& selected=UiSource("codex");
        UiRequire(UiOverride("codex")==missing&&selected.customPath&&selected.paths.size()==1&&selected.paths[0].path==missing&&!selected.paths[0].exists&&selected.status=="not-found","native dialog failed to preserve the selected Unicode missing path");
        UiRequire(state.snapshot.events==1&&selected.records==1,"missing selected path scanned defaults or removed history");
        configuredImports.clear();importCodex.clear();LoadPreferences();
        UiRequire(UiOverride("codex")==missing&&IniString(L"ImportCodex").empty(),"simulated restart lost Unicode path or revived obsolete imports");
        {
            UiDialogDriver driver(L"TokenScopeSourcePath",80,83,{},missing,true);
            ConfigureSourcePath("codex");driver.Join();
            UiRequire(driver.operated&&!driver.timedOut&&driver.initialMatched,"native source dialog restore-default action did not run");
        }
        UiPumpCollection();const auto& restored=UiSource("codex");
        UiRequire(UiOverride("codex").empty()&&!restored.customPath&&restored.paths.size()==2&&restored.paths[0].exists&&!restored.paths[1].exists&&state.snapshot.events==2&&restored.records==2,"restoring native defaults lost candidates, history or default usage");
        UiRequire(IniString(L"Import.codex").empty()&&IniString(L"ImportCodex").empty(),"restore-default left an import key on disk");
        configuredImports.clear();LoadPreferences();UiRequire(UiOverride("codex").empty(),"legacy import revived after restoring defaults and restarting");
        GetPrivateProfileStringW(L"UnrelatedSection",L"KeepMe",L"",retained,128,settingsPath.c_str());
        UiRequire(std::wstring(retained)==L"retained value","saving preferences removed another INI section");
        StartCollection();UiPumpCollection();UiRequire(state.snapshot.events==2,"restart-style collection duplicated actual usage");
        // Deliver a synthetic successful worker result containing only an
        // owned local usage fixture while the actual price modal is open.
        // This exercises the real WM_CONNECTION/WM_IMPORTED queue, without
        // credentials, a connection worker or a server request.
        const auto synced=root/L"sync-fixture"/L"cursor.json";UiWrite(synced,UiUsage("sync-while-price-open",30,"cursor"));
        auto syncPacket=std::make_unique<ConnectionPacket>();syncPacket->id="cursor";syncPacket->result.success=true;
        syncPacket->result.usageRecords=1;syncPacket->result.cachePath=synced.wstring();
        uiObservedQueuedRefresh=false;
        {
            UiDialogDriver driver(L"TokenScopePrice",30,IDCANCEL,{},{},false,[&](HWND) {
                if(PostMessageW(mainWindow,UiConnectionDuringPrice,0,reinterpret_cast<LPARAM>(syncPacket.get())))syncPacket.release();
            });
            EditModelPrice();driver.Join();UiRequire(driver.operated&&!driver.timedOut,"actual price cancel dialog did not close");
        }
        UiRequire(uiObservedQueuedRefresh,"connection completion during price editing silently discarded a refresh");
        UiPumpCollection();UiRequire(!importRefreshPending&&state.snapshot.events==3&&UiSource("cursor").customPath&&UiSource("cursor").records==1,"price cancel did not import the queued connection cache");
        UiWrite(root/L"测试用户"/L".codex"/L"sessions"/L"新增真实记录.json",UiUsage("catalogue-refresh-while-price-open",40));
        auto onlinePacket=std::make_unique<std::wstring>(L"owned fixture public catalogue completed");uiObservedQueuedRefresh=false;
        {
            UiDialogDriver driver(L"TokenScopePrice",30,IDCANCEL,{},{},false,[&](HWND) {
                if(PostMessageW(mainWindow,UiOnlineDuringPrice,0,reinterpret_cast<LPARAM>(onlinePacket.get())))onlinePacket.release();
            });
            EditModelPrice();driver.Join();UiRequire(driver.operated&&!driver.timedOut,"price modal cancel for catalogue completion did not close");
        }
        UiRequire(uiObservedQueuedRefresh,"catalogue completion during price editing silently discarded a refresh");
        UiPumpCollection();UiRequire(!importRefreshPending&&state.snapshot.events==4,"price cancel did not drain the catalogue refresh queue");
        UiWrite(root/L"测试用户"/L".codex"/L"sessions"/L"保存价格时新增.json",UiUsage("refresh-with-price-save",50));
        onlinePacket=std::make_unique<std::wstring>(L"owned fixture catalogue completed during price save");uiObservedQueuedRefresh=false;
        {
            UiDialogDriver driver(L"TokenScopePrice",30,IDOK,L"0",{},false,[&](HWND dialog) {
                for(int id=31;id<=33;++id)SetDlgItemTextW(dialog,id,L"0");
                if(PostMessageW(mainWindow,UiOnlineDuringPrice,0,reinterpret_cast<LPARAM>(onlinePacket.get())))onlinePacket.release();
            });
            EditModelPrice();driver.Join();UiRequire(driver.operated&&!driver.timedOut,"actual price save dialog did not close");
        }
        UiRequire(uiObservedQueuedRefresh,"price save discarded the pending catalogue refresh");
        UiPumpCollection();UiRequire(!importRefreshPending&&state.snapshot.events==5,"price save's WM_IMPORTED did not drain the queued collection");
        for(const std::string id:{"cursor","trae-code"}) {
            UiDialogDriver driver(L"TokenScopeConnection",91,IDCANCEL);
            ConnectSource(id);driver.Join();
            UiRequire(driver.operated&&!driver.timedOut&&driver.masked,"native connection input is not password-masked or cancel failed");
            UiRequire(!connectionBusy&&!connectionWorker.joinable()&&!HasConnectionCredential(dataDirectory,id),"cancelled connection started sync or persisted a credential");
        }
        UiRequire(!fs::exists(fs::path(dataDirectory)/L"connections")&&!networkWorker.joinable()&&!exchangeWorker.joinable(),"UI test unexpectedly started network or wrote connection data");
        UiStop();isolation.reset();if(com){CoUninitialize();com=false;}
        UiRemoveOwned(root,build);
        std::cout<<"PASS main UI: ANSI migration, Unicode path modal/restart, default restore, source isolation/history, price-modal refresh queues, masked authorization cancel; no network or credentials\n";
        return 0;
    } catch(const std::exception& e) {
        UiStop();isolation.reset();if(com)CoUninitialize();
        std::cerr<<"FAIL main UI: "<<e.what()<<"; owned fixtures: "<<WideToUtf8(root.wstring())<<"\n";return 1;
    }
}
