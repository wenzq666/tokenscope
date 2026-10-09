#include "app.h"
#include "adapters.h"
#include "../vendor/sqlite3.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace tokenscope;
namespace fs=std::filesystem;
namespace {
void Require(bool condition,const char* detail){if(!condition)throw std::runtime_error(detail);}
struct EnvironmentGuard {
    std::map<std::wstring,std::wstring> saved;
    explicit EnvironmentGuard(const fs::path& root){
        for(const auto* name:{L"USERPROFILE",L"APPDATA",L"CODEX_HOME",L"CLAUDE_CONFIG_DIR",L"XDG_DATA_HOME",L"KIMI_SHARE_DIR",L"KIMI_CODE_HOME",L"QWEN_RUNTIME_DIR",L"QWEN_HOME",L"GEMINI_CLI_HOME",L"XDG_CONFIG_HOME",L"TOKENS_CONFIG_DIR",L"CODEBUFF_DATA_DIR",L"REASONIX_STATE_HOME",L"REASONIX_HOME",L"JCODE_HOME",L"GROK_HOME"}){
            DWORD size=GetEnvironmentVariableW(name,nullptr,0);std::wstring value(size,L'\0');if(size){GetEnvironmentVariableW(name,value.data(),size);value.resize(size-1);}saved[name]=value;SetEnvironmentVariableW(name,nullptr);
        }
        SetEnvironmentVariableW(L"USERPROFILE",(root/L"用户目录").c_str());SetEnvironmentVariableW(L"APPDATA",(root/L"漫游配置").c_str());
    }
    ~EnvironmentGuard(){for(const auto& [name,value]:saved)SetEnvironmentVariableW(name.c_str(),value.empty()?nullptr:value.c_str());}
};
void Write(const fs::path& path,const std::string& bytes){fs::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary);out<<bytes;Require(out.good(),"fixture write failed");}
std::string Usage(const std::string& source,const std::string& id,int input){return "{\"schema\":\"tokenscope.usage.v1\",\"source\":\""+source+"\",\"id\":\""+id+"\",\"timestamp\":\"2026-10-08T10:00:00+08:00\",\"model\":\"actual-model\",\"usage\":{\"input_tokens\":"+std::to_string(input)+",\"output_tokens\":5}}";}
const SourceStatus& Source(const Snapshot& snapshot,const std::string& id){auto found=std::find_if(snapshot.sources.begin(),snapshot.sources.end(),[&](const SourceStatus& status){return status.id==id;});Require(found!=snapshot.sources.end(),"source status missing");return *found;}
void AllSources(const Snapshot& snapshot){const auto catalog=CatalogSources();Require(snapshot.sources.size()==catalog.size(),"source status list is incomplete");for(const auto& spec:catalog){const auto& status=Source(snapshot,spec.id);Require(status.name==spec.name,"source status name changed");}}
int64_t Sum(const Snapshot& snapshot){int64_t total=0;for(const auto& row:snapshot.rows)total+=row.total();return total;}
using Overrides=std::vector<std::pair<std::string,std::wstring>>;
void Scan(Store& store,const Overrides& overrides,const std::atomic_bool& cancelled){auto result=store.ScanSources(overrides,cancelled);Require(result.error.empty(),result.error.c_str());AllSources(store.ReadSnapshot());}
struct FixtureSqlite {
    sqlite3* db=nullptr;
    FixtureSqlite(const fs::path& path,int flags){Require(sqlite3_open_v2(WideToUtf8(path.wstring()).c_str(),&db,flags,nullptr)==SQLITE_OK,"cannot open owned database fixture");}
    ~FixtureSqlite(){if(db)sqlite3_close(db);}
};
std::string SqlValue(const fs::path& path,const char* sql) {
    FixtureSqlite fixture(path,SQLITE_OPEN_READONLY);sqlite3_stmt* statement=nullptr;
    Require(sqlite3_prepare_v2(fixture.db,sql,-1,&statement,nullptr)==SQLITE_OK,"cannot prepare owned metadata query");
    const int result=sqlite3_step(statement);std::string value;
    if(result==SQLITE_ROW){const auto* bytes=sqlite3_column_text(statement,0);if(bytes)value=reinterpret_cast<const char*>(bytes);}
    sqlite3_finalize(statement);Require(result==SQLITE_ROW||result==SQLITE_DONE,"owned metadata query failed");return value;
}
void NativeCopyLocations(const fs::path& root) {
    const auto database=root/L"native-copies.sqlite3",original=root/L"原生日志",copy=root/L"原生日志副本";
    const std::string codex="{\"type\":\"session_meta\",\"payload\":{\"id\":\"native-copy-session\"}}\n"
      "{\"type\":\"turn_context\",\"payload\":{\"model\":\"fixture-model\"}}\n"
      "{\"type\":\"event_msg\",\"timestamp\":\"2026-10-08T10:00:00+08:00\",\"payload\":{\"type\":\"token_count\",\"info\":{\"total_token_usage\":{\"input_tokens\":10,\"output_tokens\":5}}}}\n";
    const std::string claude="{\"type\":\"assistant\",\"timestamp\":\"2026-10-08T10:00:00+08:00\",\"message\":{\"id\":\"native-copy-message\",\"model\":\"fixture-model\",\"usage\":{\"input_tokens\":10,\"output_tokens\":5}}}\n";
    Write(original/L"codex.jsonl",codex);Write(copy/L"codex.jsonl",codex);Write(original/L"claude.jsonl",claude);Write(copy/L"claude.jsonl",claude);
    std::atomic_bool cancelled=false;std::string error;
    {
        Store store;Require(store.Open(database.wstring(),error),error.c_str());
        for(const std::string id:{"codex","claude"}) {
            const auto filename=Utf8ToWide(id)+L".jsonl";
            const auto first=store.ImportFolder((original/filename).wstring(),id,cancelled);
            const auto duplicate=store.ImportFolder((copy/filename).wstring(),id,cancelled);
            Require(first.error.empty()&&first.added==1&&duplicate.error.empty()&&duplicate.added==0&&Source(store.ReadSnapshot(),id).status=="ready","native duplicate-only file lost its observed usage or added tokens");
            const auto name=id=="codex"?"Codex":"Claude Code";
            const auto query="SELECT origin FROM events WHERE software='"+std::string(name)+"'";
            Require(SqlValue(database,query.c_str())==WideToUtf8((original/filename).wstring()),"observing a duplicate rewrote the original event audit path");
        }
        Require(store.ReadSnapshot().events==2&&Sum(store.ReadSnapshot())==30,"native copy mapping changed token totals");
        Require(SqlValue(database,"SELECT COUNT(*) FROM event_origins")=="4","native original and duplicate locations were not both recorded");
    }
    const char* checkpointQuery="SELECT group_concat(path||':'||source||':'||offset||':'||prev_input,'|') FROM (SELECT * FROM checkpoints ORDER BY source,path)";
    const auto checkpoints=SqlValue(database,checkpointQuery);
    {
        FixtureSqlite legacy(database,SQLITE_OPEN_READWRITE);
        Require(sqlite3_exec(legacy.db,"DROP TABLE event_origins;DELETE FROM meta WHERE key='event_origins_v1';",nullptr,nullptr,nullptr)==SQLITE_OK,"cannot simulate pre-location metadata database");
    }
    {
        Store migrated;Require(migrated.Open(database.wstring(),error),error.c_str());
        Require(SqlValue(database,checkpointQuery)==checkpoints,"origin migration cleared or altered cumulative native checkpoints");
        Require(SqlValue(database,"SELECT COUNT(*) FROM event_origins")=="2","legacy original audit locations were not backfilled");
        auto replay=migrated.ImportFolder((original/L"codex.jsonl").wstring(),"codex",cancelled);
        Require(replay.error.empty()&&replay.added==0&&Source(migrated.ReadSnapshot(),"codex").status=="ready"&&Sum(migrated.ReadSnapshot())==30,"legacy unchanged native file lost its location or was counted twice");
    }
}
}
int main(){
    wchar_t buffer[MAX_PATH];Require(GetTempPathW(MAX_PATH,buffer)>0,"temporary directory unavailable");const auto temp=fs::path(buffer);const auto root=temp/(L"TokenScope-source-config-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    try{
        {
        EnvironmentGuard isolated(root);const auto database=root/L"数据库"/L"usage.sqlite3";const auto defaults=root/L"用户目录"/L".codex";
        Write(defaults/L"sessions"/L"默认甲.json",Usage("codex","default-a",10));
        std::atomic_bool cancelled=false;Store store;std::string error;Require(store.Open(database.wstring(),error),error.c_str());
        Scan(store,{},cancelled);auto snapshot=store.ReadSnapshot();const auto& first=Source(snapshot,"codex");
        Require(first.paths.size()==2&&first.paths[0].exists&&!first.paths[1].exists&&!first.customPath,"every default candidate must expose its existence");Require(first.path==first.paths[0].path&&first.records==1,"preferred path or historical count incorrect");
        const auto& cloud=Source(snapshot,"manus");Require(cloud.paths.empty()&&cloud.path.empty()&&cloud.status=="needs-export"&&cloud.detail.find("未配置导出路径")!=std::string::npos,"cloud source falsely claims a default local path");
        Write(defaults/L"archived_sessions"/L"默认乙.json",Usage("codex","default-b",20));Scan(store,{},cancelled);snapshot=store.ReadSnapshot();const auto& both=Source(snapshot,"codex");Require(both.paths.size()==2&&both.paths[0].exists&&both.paths[1].exists&&both.path==both.paths[0].path&&both.records==2&&Sum(snapshot)==40,"multiple default candidates were lost or double-counted");
        const auto custom=root/L"另一个账户"/L"中文会话目录";Write(custom/L"真实记录.json",Usage("codex","custom-c",30));Overrides overrides{{"codex",custom.wstring()}};
        Scan(store,overrides,cancelled);snapshot=store.ReadSnapshot();Require(snapshot.events==3&&Sum(snapshot)==75,"custom scan removed history or omitted usage");const auto& selected=Source(snapshot,"codex");Require(selected.customPath&&selected.paths.size()==1&&selected.paths[0].exists&&selected.path==custom.wstring()&&selected.records==3,"override appended defaults instead of replacing them");
        const auto duplicate=root/L"相同用量副本";Write(duplicate/L"真实记录.json",Usage("codex","custom-c",30));
        auto duplicateOverrides=overrides;duplicateOverrides[0].second=duplicate.wstring();const auto duplicateScan=store.ScanSources(duplicateOverrides,cancelled);snapshot=store.ReadSnapshot();
        Require(duplicateScan.error.empty()&&duplicateScan.added==0&&snapshot.events==3&&Sum(snapshot)==75&&Source(snapshot,"codex").status=="ready","valid duplicate-only custom path was mislabeled empty or double-counted");
        Write(defaults/L"sessions"/L"稍后默认记录.json",Usage("codex","default-later",40));Scan(store,overrides,cancelled);snapshot=store.ReadSnapshot();Require(snapshot.events==3&&Sum(snapshot)==75,"selected other account still scanned the default account");
        const auto missing=root/L"不存在的目录";auto missingResult=store.ScanSources({{"codex",missing.wstring()}},cancelled);snapshot=store.ReadSnapshot();AllSources(snapshot);const auto& missingStatus=Source(snapshot,"codex");Require(!missingResult.error.empty()&&missingStatus.customPath&&missingStatus.paths.size()==1&&!missingStatus.paths[0].exists&&missingStatus.status=="not-found"&&missingStatus.records==3&&snapshot.events==3,"missing override hid history or fell back to defaults");
        const auto invalid=root/L"无效记录"/L"bad.json";Write(invalid,Usage("codex","invalid-negative",-1));auto invalidResult=store.ScanSources({{"codex",invalid.wstring()}},cancelled);snapshot=store.ReadSnapshot();Require(invalidResult.invalid==1&&Source(snapshot,"codex").status=="error"&&Source(snapshot,"codex").paths[0].exists&&snapshot.events==3,"invalid existing custom record was treated as successful or removed history");
        const auto empty=root/L"空的会话目录";fs::create_directories(empty);auto emptyOverrides=overrides;emptyOverrides[0].second=empty.wstring();emptyOverrides.emplace_back("claude",empty.wstring());Scan(store,emptyOverrides,cancelled);snapshot=store.ReadSnapshot();Require(Source(snapshot,"claude").status=="empty"&&Source(snapshot,"claude").customPath&&Source(snapshot,"claude").paths[0].exists,"empty custom directory marked as collected usage");Require(Source(snapshot,"codex").status=="empty"&&Source(snapshot,"codex").records==3&&snapshot.events==3,"old history falsely marked an empty current path as collected");
        const auto manuscript=root/L"云端用量"/L"任务.json";Write(manuscript,Usage("manus","cloud-task",50));overrides.emplace_back("manus",manuscript.wstring());Scan(store,overrides,cancelled);snapshot=store.ReadSnapshot();Require(Source(snapshot,"manus").customPath&&Source(snapshot,"manus").paths.size()==1&&Source(snapshot,"manus").status=="ready"&&Source(snapshot,"manus").records==1&&snapshot.events==4,"cloud source cannot configure an actual export");
        const auto qoder=root/L"导出"/L"qoder.json",work=root/L"导出"/L"trae-work.json";Write(qoder,Usage("qoder","qoder-export",60));Write(work,Usage("trae-work","work-export",70));overrides.emplace_back("qoder",qoder.wstring());overrides.emplace_back("trae-work",work.wstring());Scan(store,overrides,cancelled);snapshot=store.ReadSnapshot();Require(snapshot.events==6&&Source(snapshot,"qoder").status=="ready"&&Source(snapshot,"trae-work").status=="ready","native-only exceptions suppressed explicit real exports");const auto total=Sum(snapshot);
        {Store reopened;Require(reopened.Open(database.wstring(),error),error.c_str());auto replay=reopened.ScanSources(overrides,cancelled);Require(replay.error.empty()&&replay.added==0,"restarted configured scan did not preserve deduplication");const auto retained=reopened.ReadSnapshot();AllSources(retained);Require(retained.events==6&&Sum(retained)==total&&Source(retained,"codex").records==3,"restart lost historical source counts");
        replay=reopened.ScanSources(duplicateOverrides,cancelled);const auto duplicateRetained=reopened.ReadSnapshot();Require(replay.error.empty()&&replay.added==0&&duplicateRetained.events==6&&Sum(duplicateRetained)==total&&Source(duplicateRetained,"codex").status=="ready","restart/checkpoint lost the observed usage at a duplicate path");}
        auto unknown=overrides;unknown.emplace_back("unverified-agent",custom.wstring());const auto rejected=store.ScanSources(unknown,cancelled);Require(!rejected.error.empty()&&store.ReadSnapshot().events==6,"unknown override was accepted or damaged data");
        cancelled=true;auto cancelledOverrides=overrides;cancelledOverrides[0].second=missing.wstring();store.ScanSources(cancelledOverrides,cancelled);snapshot=store.ReadSnapshot();AllSources(snapshot);Require(Source(snapshot,"codex").customPath&&Source(snapshot,"codex").paths.size()==1&&!Source(snapshot,"codex").paths[0].exists&&Source(snapshot,"codex").records==3&&Source(snapshot,"manus").paths[0].exists&&snapshot.events==6,"cancellation erased source metadata or history");cancelled=false;
        overrides.erase(overrides.begin());Scan(store,overrides,cancelled);snapshot=store.ReadSnapshot();const auto& restored=Source(snapshot,"codex");Require(!restored.customPath&&restored.paths.size()==2&&restored.records==4&&snapshot.events==7&&Sum(snapshot)==total+45,"restore defaults removed custom history or failed to resume defaults");
        auto replay=store.ScanDefaultSources(cancelled);Require(replay.error.empty()&&replay.added==0&&store.ReadSnapshot().events==7,"legacy default scan changed historical deduplication");AllSources(store.ReadSnapshot());
        NativeCopyLocations(root);
        }
        const auto absolute=fs::absolute(root).lexically_normal();auto tempAbsolute=fs::absolute(temp).lexically_normal();while(tempAbsolute.filename().empty()&&tempAbsolute.has_parent_path()&&tempAbsolute!=tempAbsolute.parent_path())tempAbsolute=tempAbsolute.parent_path();const auto flags=GetFileAttributesW(absolute.c_str());Require(absolute.parent_path()==tempAbsolute&&absolute.filename().wstring().starts_with(L"TokenScope-source-config-")&&flags!=INVALID_FILE_ATTRIBUTES&&!(flags&FILE_ATTRIBUTE_REPARSE_POINT),"fixture cleanup escaped owned directory");
        fs::remove_all(absolute);
        std::cout<<"PASS source configuration: all candidates, Chinese paths, override isolation, exports, errors, cancellation, restore/restart deduplication\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"; fixtures: "<<WideToUtf8(root.wstring())<<"\n";return 1;}
}
