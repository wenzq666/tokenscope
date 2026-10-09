#include "../src/adapters.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
using namespace tokenscope;
namespace {
void Require(bool valid,const char* message){if(!valid)throw std::runtime_error(message);}
void Parse(const std::string& source,const std::string& text,std::vector<ParsedUsage>& output,int64_t& invalid) {
    auto doc=yyjson_read(text.data(),text.size(),0);Require(doc,"fixture JSON invalid");
    ParseDomesticRecord(source,yyjson_doc_get_root(doc),"fixture-session",[&](const ParsedUsage& u){output.push_back(u);},invalid);
    yyjson_doc_free(doc);
}
void Write(const fs::path& path,const std::string& text) {
    std::ofstream out(path,std::ios::binary);Require(bool(out),"fixture open failed");out<<text;Require(bool(out),"fixture write failed");
}
const std::string wb=R"({"type":"function_call","id":"item-a","timestamp":1791432000000,"providerData":{"conversationRequestId":"request-a","messageId":"message-a","model":"recorded-model","rawUsage":{"prompt_tokens":100,"completion_tokens":20,"prompt_cache_hit_tokens":60,"prompt_cache_write_tokens":5,"completion_tokens_details":{"reasoning_tokens":7},"credit":9000}},"message":{"usage":{"input_tokens":99999,"output_tokens":99999}}})";
const std::string header=R"({"type":"session","version":4,"id":"session-a","createdAt":1791432000000,"isSeeded":false})" "\n";
const std::string route=R"({"type":"request/header","seq":0,"time":1791432000000,"data":{"header":{"config":{"model":"actual-before","provider":"recorded-provider"}}}})" "\n";
const std::string usage=R"({"type":"assistant/message","seq":1,"time":1791432001000,"data":{"turn":1,"step":1,"message":{"id":"assistant-a","content":"PRIVATE-BODY-IGNORE"},"usage":{"inputTokens":10,"outputTokens":5,"cacheReadTokens":40,"cacheWriteTokens":20,"reasoningTokens":2}}})" "\n";
}
int main() {
    fs::path temp;
    try {
        std::vector<ParsedUsage> rows;int64_t invalid=0;
        Parse("workbuddy",wb,rows,invalid);
        Require(rows.size()==1 && !invalid,"WorkBuddy usage lost");
        Require(rows[0].key=="request-a:message-a" && rows[0].model=="recorded-model" && rows[0].provider.empty(),"WorkBuddy model/identity guessed");
        Require(rows[0].input==100 && rows[0].output==20 && rows[0].cachedInput==60 && rows[0].cacheWrite==5 && rows[0].reasoning==7,"WorkBuddy duplicated raw/normalized usage");
        rows.clear();
        Parse("workbuddy",R"({"type":"message","role":"user","timestamp":1791432000000,"providerData":{"rawUsage":{"prompt_tokens":1,"completion_tokens":2}}})",rows,invalid);
        Parse("workbuddy",R"({"type":"message","role":"assistant","id":"credit-only","timestamp":1791432000000,"providerData":{"credit":25}})",rows,invalid);
        Require(rows.empty() && !invalid,"credits or user messages treated as model calls");
        Parse("workbuddy",R"({"type":"function_call","id":"bad","timestamp":1791432000000,"providerData":{"rawUsage":{"prompt_tokens":5,"completion_tokens":2,"prompt_cache_hit_tokens":8}}})",rows,invalid);
        Parse("workbuddy",R"({"type":"function_call","id":"negative","timestamp":1791432000000,"providerData":{"rawUsage":{"prompt_tokens":-1,"completion_tokens":2}}})",rows,invalid);
        Require(rows.empty() && invalid==2,"invalid WorkBuddy counters accepted");
        Parse("workbuddy",R"({"type":"function_call","id":"fallback","timestamp":1791432000000,"providerData":{"model":"model-fallback"},"message":{"usage":{"input_tokens":3,"output_tokens":0}}})",rows,invalid);
        Require(rows.size()==1 && rows[0].input==3 && rows[0].output==0,"persisted normalized WorkBuddy fallback lost");rows.clear();
        Parse("deepseek-harness",usage,rows,invalid);
        Require(rows.size()==1 && rows[0].input==70 && rows[0].output==5 && rows[0].cachedInput==40 && rows[0].cacheWrite==20 && rows[0].reasoning==2,"DSH disjoint cache buckets summed incorrectly");
        Require(rows[0].key=="message:assistant-a" && rows[0].model.empty(),"DSH guessed model or unstable message identity");rows.clear();
        Parse("deepseek-harness",R"({"type":"assistant/attempt","seq":2,"time":1791432002000,"data":{"turn":1,"step":2,"stream":[{"type":"chunk","time":1791432001000,"chunk":{"type":"usage","usage":{"inputTokens":4,"outputTokens":2}}},{"type":"chunk","time":1791432002000,"chunk":{"type":"usage","usage":{"inputTokens":5,"outputTokens":3}}}]}})",rows,invalid);
        Require(rows.size()==1 && rows[0].input==5 && rows[0].output==3,"DSH stream final usage not selected once");rows.clear();
        Parse("deepseek-harness",R"({"type":"assistant/message","seq":3,"time":1791432002000,"data":{"message":{"id":"bad"},"usage":{"inputTokens":5,"outputTokens":1,"reasoningTokens":2}}})",rows,invalid);
        Require(rows.empty() && invalid==3,"DSH reasoning double count or inconsistent usage accepted");
        wchar_t directory[MAX_PATH];Require(GetTempPathW(MAX_PATH,directory)>0,"temp directory unavailable");
        temp=fs::path(directory)/(L"TokenScope-domestic-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        fs::create_directories(temp/L"sessions"/L"one");fs::create_directories(temp/L"workbuddy");
        Write(temp/L"sessions"/L"one"/L"session.v4.jsonl",header+route+usage+
            R"({"type":"request/context","seq":2,"time":1791432002000,"data":{"model":"actual-after","provider":"provider-after"}})" "\n"+
            R"({"type":"assistant/message","seq":3,"time":1791432003000,"data":{"message":{"id":"assistant-b"},"usage":{"inputTokens":2,"outputTokens":1}}})" "\n");
        // Older translated backups must never be counted alongside canonical v4 files.
        Write(temp/L"sessions"/L"one"/L"session.jsonl",header+route+usage);
        std::atomic_bool cancelled=false;
        auto r=CollectDomestic("deepseek-harness",(temp/L"sessions").wstring(),cancelled,[&](const ParsedUsage& u){rows.push_back(u);});
        Require(r.files==1 && r.invalid==0 && r.error.empty() && rows.size()==2,"DSH source traversal or canonical generation selection wrong");
        Require(rows[0].model=="actual-before" && rows[0].provider=="recorded-provider" && rows[1].model=="actual-after" && rows[1].provider=="provider-after","historical DSH model switch lost");
        rows.clear();
        Write(temp/L"sessions"/L"one"/L"session.v4.jsonl",R"({"type":"session","version":5,"id":"future"})" "\n"+route+usage);
        r=CollectDomestic("deepseek-harness",(temp/L"sessions").wstring(),cancelled,[&](const ParsedUsage& u){rows.push_back(u);});
        Require(rows.empty() && !r.error.empty(),"future DSH format guessed");
        Write(temp/L"sessions"/L"one"/L"session.v4.jsonl",R"({"type":"session","version":4,"id":"fork","isSeeded":true})" "\n"+route+usage+
            R"({"type":"session/end-seed","seq":2,"time":1791432002000,"data":{"inherited":true}})" "\n"+
            R"({"type":"assistant/message","seq":3,"time":1791432003000,"data":{"message":{"id":"fork-new"},"usage":{"inputTokens":2,"outputTokens":1}}})" "\n");
        r=CollectDomestic("deepseek-harness",(temp/L"sessions").wstring(),cancelled,[&](const ParsedUsage& u){rows.push_back(u);});
        Require(rows.size()==1 && rows[0].key=="message:fork-new","fork inherited prefix counted twice");rows.clear();
        Write(temp/L"workbuddy"/L"log.jsonl",wb+"\n"+wb.substr(0,50));
        r=CollectDomestic("workbuddy",(temp/L"workbuddy").wstring(),cancelled,[&](const ParsedUsage& u){rows.push_back(u);});
        Require(r.files==1 && !r.invalid && rows.size()==1,"live WorkBuddy partial tail consumed");rows.clear();
        cancelled=true;r=CollectDomestic("workbuddy",(temp/L"workbuddy").wstring(),cancelled,[&](const ParsedUsage& u){rows.push_back(u);});
        Require(!r.files && rows.empty(),"collector cancellation ignored");cancelled=false;
        r=CollectDomestic("trae-code",(temp/L"sessions").wstring(),cancelled,[&](const ParsedUsage& u){rows.push_back(u);});
        Require(!r.files && rows.empty(),"unsupported native source falsely reported as collected");
        // A synthetic official-format Zstandard fixture validates the actual packaged decoder.
        const auto compressed=fs::path(__FILE__).parent_path()/L"fixtures"/L"dsh-session.v4.jsonl.zstd";
        if(fs::exists(compressed)) {
            fs::create_directories(temp/L"compressed");fs::copy_file(compressed,temp/L"compressed"/L"session.v4.jsonl.zstd");
            r=CollectDomestic("deepseek-harness",(temp/L"compressed").wstring(),cancelled,[&](const ParsedUsage& u){rows.push_back(u);});
            Require(r.files==1 && r.error.empty() && !r.invalid && rows.size()==1 && rows[0].input==70,"packaged Zstandard stream decoder failed");
        } else throw std::runtime_error("compressed fixture missing");
        fs::remove_all(temp);
        std::cout<<"domestic adapters: usage normalization, identity, model history, forks, canonical format, compressed/live logs passed\n";
        return 0;
    }catch(const std::exception& e){if(!temp.empty())std::wcerr<<L"Fixture retained at "<<temp.wstring()<<L"\n";std::cerr<<e.what()<<"\n";return 1;}
}
