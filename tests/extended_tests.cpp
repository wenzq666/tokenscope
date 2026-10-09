#include "adapters/extended.h"
#include "sqlite3.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <algorithm>
#include <cwctype>

using namespace tokenscope;
namespace fs=std::filesystem;
namespace {
void Require(bool good,const char* text){if(!good)throw std::runtime_error(text);}
void Write(const fs::path& path,const std::string& bytes,bool append=false){fs::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary|(append?std::ios::app:std::ios::trunc));out<<bytes;Require(out.good(),"fixture write failed");}
std::vector<ParsedUsage> Parse(const std::string& source,const std::string& bytes,int64_t& invalid){auto doc=yyjson_read(bytes.data(),bytes.size(),YYJSON_READ_NOFLAG);Require(doc,"invalid fixture JSON");std::vector<ParsedUsage> rows;ParseExtendedRecord(source,yyjson_doc_get_root(doc),"C:/copied/session-actual/events.jsonl",[&](const ParsedUsage& u){rows.push_back(u);},invalid);yyjson_doc_free(doc);return rows;}
std::vector<ParsedUsage> Collect(const std::string& source,const fs::path& path,int64_t& invalid){std::atomic_bool cancelled=false;std::vector<ParsedUsage> rows;auto scan=CollectExtended(source,path.wstring(),cancelled,[&](const ParsedUsage& u){rows.push_back(u);});invalid+=scan.invalid;Require(scan.error.empty(),scan.error.c_str());return rows;}
void Sql(sqlite3* db,const std::string& sql){char* error=nullptr;int code=sqlite3_exec(db,sql.c_str(),nullptr,nullptr,&error);std::string message=error?error:"fixture SQL failed";sqlite3_free(error);Require(code==SQLITE_OK,message.c_str());}
void Database(const fs::path& path,const std::string& sql){fs::create_directories(path.parent_path());sqlite3* db=nullptr;Require(sqlite3_open(WideToUtf8(path.wstring()).c_str(),&db)==SQLITE_OK,"fixture database open failed");Sql(db,sql);sqlite3_close(db);}
void Blob(sqlite3* db,const char* sql,const std::string& bytes){sqlite3_stmt* stmt=nullptr;Require(sqlite3_prepare_v2(db,sql,-1,&stmt,nullptr)==SQLITE_OK,"fixture protobuf prepare failed");sqlite3_bind_blob(stmt,1,bytes.data(),static_cast<int>(bytes.size()),SQLITE_TRANSIENT);Require(sqlite3_step(stmt)==SQLITE_DONE,"fixture protobuf insert failed");sqlite3_finalize(stmt);}
std::string Var(uint64_t n){std::string s;do{unsigned char byte=n&127;n>>=7;if(n)byte|=128;s+=static_cast<char>(byte);}while(n);return s;}
std::string Scalar(unsigned id,uint64_t n){return Var(static_cast<uint64_t>(id)*8)+Var(n);}
std::string Nested(unsigned id,const std::string& bytes){return Var(static_cast<uint64_t>(id)*8+2)+Var(bytes.size())+bytes;}
int64_t Sum(const Snapshot& snapshot){int64_t sum=0;for(const auto& row:snapshot.rows)sum+=row.total();return sum;}
void Import(Store& store,const fs::path& path,const std::string& source){std::atomic_bool cancelled=false;auto result=store.ImportFolder(path.wstring(),source,cancelled);Require(result.error.empty(),result.error.c_str());}
struct EnvironmentGuard {
    std::map<std::wstring,std::wstring> saved;
    EnvironmentGuard(std::initializer_list<const wchar_t*> names){for(auto name:names){DWORD size=GetEnvironmentVariableW(name,nullptr,0);std::wstring value(size,L'\0');if(size){GetEnvironmentVariableW(name,value.data(),size);value.resize(size-1);}saved[name]=value;}}
    ~EnvironmentGuard(){for(const auto& [name,value]:saved)SetEnvironmentVariableW(name.c_str(),value.empty()?nullptr:value.c_str());}
};
void CacheCatalogRoots(const fs::path& ownedRoot){
    EnvironmentGuard guard{L"USERPROFILE",L"APPDATA",L"XDG_CONFIG_HOME",L"TOKENS_CONFIG_DIR"};
    const auto home=ownedRoot/L"cache-profile",roaming=ownedRoot/L"cache-roaming",xdg=ownedRoot/L"cache-xdg",isolated=ownedRoot/L"cache-isolated";
    SetEnvironmentVariableW(L"USERPROFILE",home.c_str());SetEnvironmentVariableW(L"APPDATA",roaming.c_str());SetEnvironmentVariableW(L"XDG_CONFIG_HOME",nullptr);SetEnvironmentVariableW(L"TOKENS_CONFIG_DIR",nullptr);
    const std::map<std::string,fs::path> suffixes{{"cursor",L"cursor-cache"},{"trae-code",fs::path(L"trae-cache")/L"sessions"},{"antigravity",fs::path(L"antigravity-cache")/L"sessions"}};
    auto catalog=CatalogSources();
    for(const auto& [id,suffix]:suffixes){const auto source=FindSource(catalog,id);Require(source&&source->paths.size()==2,"Windows cache roots missing roaming or legacy candidate");Require(source->paths[0]==(roaming/L"tokens"/suffix).wstring()&&source->paths[1]==(home/L".config"/L"tokens"/suffix).wstring(),"Windows cache roots do not match the writer and legacy location");}
    SetEnvironmentVariableW(L"XDG_CONFIG_HOME",xdg.c_str());catalog=CatalogSources();
    for(const auto& [id,suffix]:suffixes){const auto source=FindSource(catalog,id);Require(source->paths.size()==2&&source->paths[1]==(xdg/L"tokens"/suffix).wstring(),"XDG compatibility cache root ignored");}
    auto duplicate=(roaming/L".").wstring();std::transform(duplicate.begin(),duplicate.end(),duplicate.begin(),::towupper);SetEnvironmentVariableW(L"XDG_CONFIG_HOME",duplicate.c_str());catalog=CatalogSources();
    for(const auto& [id,suffix]:suffixes){const auto source=FindSource(catalog,id);Require(source->paths.size()==1&&source->paths[0]==(roaming/L"tokens"/suffix).wstring(),"Equivalent Windows cache roots were not deduplicated");}
    SetEnvironmentVariableW(L"TOKENS_CONFIG_DIR",isolated.c_str());catalog=CatalogSources();
    for(const auto& [id,suffix]:suffixes){const auto source=FindSource(catalog,id);Require(source->paths.size()==1&&source->paths[0]==(isolated/suffix).wstring(),"Explicit tokens profile scanned other accounts");}
    // Copies in the modern and legacy roots retain the same actual usage ID.
    const auto modern=roaming/L"tokens"/L"trae-cache"/L"sessions",legacy=home/L".config"/L"tokens"/L"trae-cache"/L"sessions";
    const std::string usage=R"([{"session_id":"same-account-session","usage_time":1791367200,"model_name":"actual-model","extra_info":{"input_token":10,"output_token":8}}])";
    Write(modern/L"usage-1.json",usage);Write(legacy/L"usage-2.json",usage);
    Store store;std::string error;Require(store.Open((ownedRoot/L"cache-roots.sqlite3").wstring(),error),error.c_str());Import(store,modern,"trae-code");Import(store,legacy,"trae-code");Require(store.ReadSnapshot().events==1&&Sum(store.ReadSnapshot())==18,"Copied modern/legacy cache roots doubled usage");
}
const std::string span=R"({"type":"span","traceId":"trace-real","spanId":"span-real","name":"chat actual-model","startTime":[1791367200,123000000],"attributes":{"gen_ai.operation.name":"chat","gen_ai.response.id":"response-real","gen_ai.response.model":"actual-model","gen_ai.provider.name":"actual-provider","gen_ai.usage.input_tokens":100,"gen_ai.usage.output_tokens":30,"gen_ai.usage.cache_read.input_tokens":60,"gen_ai.usage.cache_write.input_tokens":10,"gen_ai.usage.reasoning_tokens":20}})";
const std::string inference=R"({"type":"log","traceId":"trace-real","spanId":"span-real","timeUnixNano":"1791367200123000000","attributes":{"event.name":"gen_ai.client.inference.operation.details","gen_ai.response.id":"response-real","gen_ai.response.model":"actual-model","gen_ai.provider.name":"actual-provider","gen_ai.usage.input_tokens":100,"gen_ai.usage.output_tokens":30,"gen_ai.usage.cache_read_input_tokens":60,"gen_ai.usage.cache_creation_input_tokens":10,"gen_ai.usage.reasoning_tokens":20}})";
const std::string sdk=R"({"type":"assistant.usage","id":"sdk-event","timestamp":"2026-10-07T10:00:00.123Z","data":{"apiCallId":"response-real","model":"actual-model","modelProvider":"actual-provider","inputTokens":100,"outputTokens":30,"cacheReadTokens":60,"cacheWriteTokens":10,"reasoningTokens":20}})";
void CopilotDurable(const fs::path& root){
    auto file=root/L"Copilot"/L"usage.jsonl";auto db=root/L"copilot-usage.sqlite3";std::string error;
    Write(file,inference+"\n"+sdk+"\n");
    {Store store;Require(store.Open(db.wstring(),error),error.c_str());Import(store,file,"copilot");auto s=store.ReadSnapshot();Require(s.events==1&&Sum(s)==130,"Copilot SDK/inference response identity doubled");}
    // A first span snapshot has no response ID; its shared trace/span already
    // supersedes the child, while a later snapshot enriches the response scope.
    auto missingResponse=span;const std::string field="\"gen_ai.response.id\":\"response-real\",";missingResponse.erase(missingResponse.find(field),field.size());
    Write(file,missingResponse+"\n",true);
    {Store store;Require(store.Open(db.wstring(),error),error.c_str());Import(store,file,"copilot");auto s=store.ReadSnapshot();Require(s.events==1&&Sum(s)==130,"Copilot cross-scan span failed to supersede inference");}
    Write(file,span+"\n",true);
    {Store store;Require(store.Open(db.wstring(),error),error.c_str());Import(store,file,"copilot");auto s=store.ReadSnapshot();Require(s.events==1&&Sum(s)==130,"Copilot late response ID changed stable span identity");Import(store,file,"copilot");Require(Sum(store.ReadSnapshot())==130,"Copilot replay doubled usage");}
    const auto reverseDb=root/L"copilot-parent-first.sqlite3";Write(file,span+"\n");
    {Store store;Require(store.Open(reverseDb.wstring(),error),error.c_str());Import(store,file,"copilot");Write(file,inference+"\n"+sdk+"\n",true);Import(store,file,"copilot");auto s=store.ReadSnapshot();Require(s.events==1&&Sum(s)==130,"Copilot parent-first did not suppress exported children");}
}
}
int main(){
    wchar_t temp[MAX_PATH];Require(GetTempPathW(MAX_PATH,temp)>0,"temp directory missing");const fs::path root=fs::path(temp)/(L"TokenScope-extended-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    try{
        int64_t invalid=0;auto rows=Parse("trae-code",R"([{"session_id":"actual-session","usage_time":1791367200,"model_name":"Actual Model","extra_info":{"input_token":10,"output_token":8,"cache_read_token":20,"cache_write_token":5}}])",invalid);
        Require(rows.size()==1&&rows[0].input==35&&rows[0].output==8&&rows[0].cachedInput==20&&rows[0].cacheWrite==5,"TRAE disjoint cache buckets incorrect");Require(rows[0].provider.empty()&&rows[0].model=="Actual Model","TRAE inferred historical route");
        rows=Parse("trae-code",R"([{"session_id":"auto-session","usage_time":1791367200,"mode":"Auto","model_name":"","extra_info":{"input_token":10,"output_token":8}}])",invalid);Require(rows.size()==1&&rows[0].model.empty(),"TRAE invented Auto model");
        rows=Parse("cursor",R"({"usageEventsDisplay":[{"requestId":"shared-request","conversationId":"conversation","timestamp":"1791367200123","model":"actual-model","chargedCents":0,"tokenUsage":{"inputTokens":10,"outputTokens":8,"cacheReadTokens":20,"cacheWriteTokens":5}},{"conversationId":"conversation","timestamp":1791367200123,"model":"auto","tokenUsage":{"inputTokens":2,"outputTokens":1}},{"conversationId":"conversation","timestamp":1791367200123,"model":"auto","tokenUsage":{"inputTokens":2,"outputTokens":1}}]})",invalid);
        Require(rows.size()==3&&rows[0].key=="cursor:request:shared-request"&&rows[0].input==35,"Cursor API cache counters incorrect");Require(rows[1].model.empty()&&rows[1].key!=rows[2].key,"Cursor Auto model or equal requests lost");
        auto csv=root/L"Cursor"/L"usage.csv";Write(csv,"Date,Model,Input (w/ Cache Write),Input (w/o Cache Write),Cache Read,Output Tokens,Total Tokens,Request ID\n2026-10-07T10:00:00.123Z,actual-model,15,10,20,8,43,shared-request\n");auto csvRows=Collect("cursor",csv,invalid);Require(csvRows.size()==1&&csvRows[0].key==rows[0].key,"Cursor CSV/API request IDs do not deduplicate");
        auto cache=root/L"cursor-cache";Write(cache/L"usage.csv","Date,Model,Input (w/ Cache Write),Input (w/o Cache Write),Cache Read,Output Tokens\n2026-10-07T10:00:00.123Z,actual-model,15,10,20,8\n");
        Write(cache/L"usage.json",R"({"usageEventsDisplay":[{"conversationId":"c","timestamp":1791367200123,"model":"actual-model","tokenUsage":{"inputTokens":10,"outputTokens":8,"cacheReadTokens":20,"cacheWriteTokens":5}}]})");
        Write(cache/L"archive"/L"usage-old.csv","Date,Model,Input (w/ Cache Write),Input (w/o Cache Write),Cache Read,Output Tokens\n2026-10-07T10:00:00.123Z,actual-model,15,10,20,8\n");
        auto cachedRows=Collect("cursor",cache,invalid);Require(cachedRows.size()==1&&cachedRows[0].input==35,"Cursor API/legacy/archived cache channels doubled usage");
        auto renamed=root/L"Cursor"/L"renamed-export.csv";fs::copy_file(csv,renamed);Require(Collect("cursor",renamed,invalid).size()==1,"Explicit renamed Cursor CSV refused");
        rows=Parse("copilot",span,invalid);Require(rows.size()==1&&rows[0].input==100&&rows[0].output==30&&rows[0].reasoning==20&&rows[0].rollup&&rows[0].rollupScopes.size()==2,"Copilot OTel cache/reasoning double counted");
        rows=Parse("copilot",inference,invalid);Require(rows.size()==1&&!rows[0].rollup&&rows[0].timestamp=="2026-10-07T10:00:00.123Z","Copilot nanosecond log timestamp wrong");
        rows=Parse("copilot",R"({"type":"span","name":"invoke_agent","traceId":"trace","spanId":"span","startTime":[1791367200,0],"attributes":{"gen_ai.operation.name":"invoke_agent","gen_ai.usage.input_tokens":999,"gen_ai.usage.output_tokens":999}})",invalid);Require(rows.empty(),"Copilot agent cumulative summary counted");
        rows=Parse("antigravity",R"({"type":"usage","sessionId":"actual-session","responseId":"r","timestamp":1791367200123,"input":10,"output":8,"cacheRead":20,"cacheWrite":5,"reasoning":3})",invalid);Require(rows.size()==1&&rows[0].input==35&&rows[0].output==11&&rows[0].reasoning==3&&rows[0].model.empty(),"Antigravity cache format inaccurate");
        rows=Parse("commandcode",R"({"type":"message","id":"entry-id","timestamp":"2026-10-07T10:00:00Z","model":"gateway/actual-model","message":{"role":"assistant","content":"PRIVATE-CONTENT"},"usage":{"inputTokens":100,"outputTokens":30,"cacheReadTokens":60,"cacheWriteTokens":10}})",invalid);Require(rows.size()==1&&rows[0].input==100&&rows[0].model=="actual-model"&&rows[0].provider=="gateway","CommandCode inclusive input or model incorrect");
        rows=Parse("reasonix",R"({"ts":"2026-10-07T10:00:00Z","model":"gateway/actual-model","prompt":100,"completion":30,"reasoning":20,"cache_hit":60,"total":130,"requests":1})",invalid);Require(rows.size()==1&&rows[0].input==100&&rows[0].output==30&&rows[0].reasoning==20,"Reasonix subsets incorrectly additive");
        rows=Parse("reasonix",R"({"ts":"2026-10-07T10:00:00Z","model":"m","prompt":999,"completion":999,"total":999,"requests":1,"turn":true})",invalid);Require(rows.empty(),"Reasonix turn marker counted");
        rows=Parse("junie",R"({"sessionId":"s","timestampMs":1791367200123,"event":{"agentEvent":{"kind":"LlmResponseMetadataEvent","modelUsage":[{"model":"actual","inputTokens":10,"outputTokens":20},{"model":"actual","inputTokens":10,"outputTokens":20}]}}})",invalid);Require(rows.size()==2&&rows[0].key!=rows[1].key,"Junie distinct same-event requests collapsed");
        rows=Parse("grok-build",R"({"sid":"s","ts":"2026-10-07T10:00:00Z","msg":"shell.turn.inference_done","ctx":{"loop_index":1,"prompt_tokens":100,"cached_prompt_tokens":60,"completion_tokens":30,"reasoning_tokens":20}})",invalid);Require(rows.size()==1&&rows[0].input==100&&rows[0].output==30&&rows[0].model.empty(),"Grok snapshot/model guessed");
        rows=Parse("jcode",R"({"model":"later-session-model","messages":[{"id":"msg","role":"assistant","timestamp":"2026-10-07T10:00:00Z","token_usage":{"input_tokens":10,"output_tokens":8,"cache_read_input_tokens":20}}]})",invalid);Require(rows.size()==1&&rows[0].input==30&&rows[0].model.empty(),"Jcode cache shape or mutable session model incorrect");
        rows=Parse("opencodereview",R"({"type":"llm_response","sessionId":"s","timestamp":"2026-10-07T10:00:00Z","model":"actual","usage":{"prompt_tokens":100,"completion_tokens":30}})",invalid);Require(rows.size()==1&&rows[0].input==100&&rows[0].output==30,"OpenCodeReview real request not parsed");
        const auto beforeInvalid=invalid;rows=Parse("commandcode",R"({"type":"message","id":"bad","timestamp":"2026-02-31T10:00:00Z","message":{"role":"assistant"},"usage":{"inputTokens":10,"outputTokens":20}})",invalid);Require(rows.empty()&&invalid==beforeInvalid+1,"Impossible native date accepted");
        rows=Parse("opencodereview",R"({"type":"llm_response","id":"ambiguous","timestamp":"2026-10-07T10:00:00Z","usage":{"prompt_tokens":100,"completion_tokens":30,"cache_read_tokens":60}})",invalid);Require(rows.empty(),"Ambiguous review cache guessed");
        auto logs=root/L"Reasonix"/L"stats.jsonl";const std::string stat=R"({"ts":"2026-10-07T10:00:00Z","model":"m","prompt":10,"completion":20,"total":30,"requests":1})";
        Write(logs,stat+"\n"+stat+"\n"+stat);rows=Collect("reasonix",logs,invalid);Require(rows.size()==2&&rows[0].key!=rows[1].key,"Reasonix equal calls or partial line policy incorrect");Write(logs,"\n",true);auto replay=Collect("reasonix",logs,invalid);Require(replay.size()==3&&replay[0].key==rows[0].key,"Reasonix append replay unstable");
        const auto openDb=root/L"Kilo"/L"kilo.db";Database(openDb,"CREATE TABLE message(id TEXT PRIMARY KEY,data TEXT);INSERT INTO message VALUES('id','{\"role\":\"assistant\",\"modelID\":\"real\",\"providerID\":\"gateway\",\"time\":{\"created\":1791367200123},\"tokens\":{\"input\":10,\"output\":30,\"reasoning\":20,\"cache\":{\"read\":80,\"write\":15}}}');");
        rows=Collect("kilo-cli",openDb,invalid);Require(rows.size()==1&&rows[0].input==105&&rows[0].output==30,"Kilo SQLite request format failed");auto mimo=Collect("mimo-code",openDb,invalid);Require(mimo.size()==1&&mimo[0].key==rows[0].key,"MiMo shared schema changed identity");
        const auto devinDb=root/L"Devin"/L"sessions.db";Database(devinDb,"CREATE TABLE message_nodes(row_id INTEGER,session_id TEXT,chat_message TEXT,created_at INTEGER);INSERT INTO message_nodes VALUES(1,'s','{\"role\":\"assistant\",\"metadata\":{\"generation_model\":\"real-model\",\"metrics\":{\"input_tokens\":10,\"output_tokens\":30,\"cache_read_tokens\":80,\"cache_creation_tokens\":15}}}',1791367200);");
        rows=Collect("devin-cli",devinDb,invalid);Require(rows.size()==1&&rows[0].input==105&&rows[0].model=="real-model"&&rows[0].timestamp=="2026-10-07T10:00:00.000Z","Devin SQLite real usage missing");
        const auto zcodeDb=root/L"Zcode"/L"db.sqlite";Database(zcodeDb,"CREATE TABLE model_usage(id TEXT,session_id TEXT,model_id TEXT,started_at INTEGER,completed_at INTEGER,input_tokens INTEGER,output_tokens INTEGER,reasoning_tokens INTEGER,cache_read_input_tokens INTEGER,cache_creation_input_tokens INTEGER,computed_total_tokens INTEGER);INSERT INTO model_usage VALUES('r','s','actual',1791367200123,1791367201000,100,30,20,60,10,130);INSERT INTO model_usage VALUES('old','s','actual',1791367200123,1791367201000,100,30,20,60,10,220);");
        rows=Collect("zcode",zcodeDb,invalid);Require(rows.size()==2&&rows[0].input==100&&rows[0].output==30&&rows[1].input==170&&rows[1].output==50,"ZCode versioned totals not disambiguated");
        const auto protoDb=root/L"Antigravity"/L"conversation.db";sqlite3* db=nullptr;fs::create_directories(protoDb.parent_path());Require(sqlite3_open(WideToUtf8(protoDb.wstring()).c_str(),&db)==SQLITE_OK,"protobuf DB failed");Sql(db,"CREATE TABLE gen_metadata(idx INTEGER,data BLOB);CREATE TABLE steps(step_type INTEGER,metadata BLOB);");
        const auto usage=Scalar(1,2)+Scalar(2,10)+Scalar(5,20)+Scalar(9,8)+Scalar(10,3)+Nested(11,"response-1");const auto time=Scalar(1,1791367200)+Scalar(2,123000000);const auto chat=Nested(4,usage)+Nested(9,Nested(4,time))+Nested(19,"recorded-model");Blob(db,"INSERT INTO gen_metadata VALUES(1,?)",Nested(1,chat));
        const auto usage2=Scalar(2,10)+Scalar(9,8)+Nested(11,"response-2");Blob(db,"INSERT INTO gen_metadata VALUES(2,?)",Nested(1,Nested(4,usage2)));Blob(db,"INSERT INTO steps VALUES(15,?)",Nested(1,time)+Nested(9,Nested(11,"response-2"))+Nested(20,Scalar(3,2)));
        Blob(db,"INSERT INTO gen_metadata VALUES(3,?)",Nested(1,Nested(4,usage+std::string("\x80",1))+Nested(9,Nested(4,time))));sqlite3_close(db);
        rows=Collect("antigravity-cli",protoDb,invalid);Require(rows.size()==2&&rows[0].input==32&&rows[0].output==11&&rows[1].timestamp==rows[0].timestamp,"Antigravity protobuf fields/timestamps/truncation incorrect");
        std::atomic_bool cancelled=true;size_t calls=0;auto scan=CollectExtended("antigravity-cli",protoDb.wstring(),cancelled,[&](const ParsedUsage&){++calls;});Require(scan.files==0&&calls==0,"cancelled scan emitted usage");
        const auto catalog=CatalogSources();Require(catalog.size()>=56&&FindSource(catalog,"warp")&&FindSource(catalog,"trae-code")&&FindSource(catalog,"chatgpt"),"existing or reference clients removed");
        CacheCatalogRoots(root);
        const auto geminiOld=[](){wchar_t buffer[32768];DWORD n=GetEnvironmentVariableW(L"GEMINI_CLI_HOME",buffer,32768);return n?std::wstring(buffer,n):std::wstring();}();SetEnvironmentVariableW(L"GEMINI_CLI_HOME",L"E:\\relocated-home");auto custom=CatalogSources();Require(FindSource(custom,"gemini-cli")->paths[0]==L"E:\\relocated-home\\.gemini\\tmp","Gemini official home override semantics broken");SetEnvironmentVariableW(L"GEMINI_CLI_HOME",geminiOld.empty()?nullptr:geminiOld.c_str());
        CopilotDurable(root);
        const auto absolute=fs::absolute(root).lexically_normal();auto tempAbsolute=fs::absolute(fs::path(temp)).lexically_normal();
        while(tempAbsolute.filename().empty()&&tempAbsolute.has_parent_path()&&tempAbsolute!=tempAbsolute.parent_path())tempAbsolute=tempAbsolute.parent_path();
        const auto flags=GetFileAttributesW(absolute.c_str());
        Require(absolute.parent_path()==tempAbsolute&&absolute.filename().wstring().starts_with(L"TokenScope-extended-")&&flags!=INVALID_FILE_ATTRIBUTES&&!(flags&FILE_ATTRIBUTE_REPARSE_POINT),"fixture cleanup escaped owned temp directory");fs::remove_all(absolute);
        std::cout<<"PASS extended: cached APIs, OTel durable preference, strict native JSONL, readonly SQLite/protobuf, replay/cancellation/catalog\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"; fixtures: "<<WideToUtf8(root.wstring())<<"\n";return 1;}
}
