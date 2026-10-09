#include "../adapters.h"
#include <filesystem>
#include <cstdio>
#include <limits>
#include <algorithm>
#include <cwctype>

namespace tokenscope {
namespace {
std::wstring Environment(const wchar_t* key) {
    DWORD size=GetEnvironmentVariableW(key,nullptr,0);if(!size)return {};
    std::wstring value(size,L'\0');GetEnvironmentVariableW(key,value.data(),size);value.resize(size-1);return value;
}
}
std::vector<SourceSpec> CatalogSources() {
    namespace fs=std::filesystem;
    const fs::path home=Environment(L"USERPROFILE"), roaming=Environment(L"APPDATA");
    auto codex=Environment(L"CODEX_HOME");if(codex.empty())codex=(home/L".codex").wstring();
    auto claude=Environment(L"CLAUDE_CONFIG_DIR");if(claude.empty())claude=(home/L".claude").wstring();
    auto share=Environment(L"XDG_DATA_HOME");if(share.empty())share=(home/L".local"/L"share").wstring();
    auto kimi=Environment(L"KIMI_SHARE_DIR");if(kimi.empty())kimi=(home/L".kimi").wstring();
    auto kimiCode=Environment(L"KIMI_CODE_HOME");if(kimiCode.empty())kimiCode=(home/L".kimi-code").wstring();
    auto qwen=Environment(L"QWEN_RUNTIME_DIR");if(qwen.empty())qwen=Environment(L"QWEN_HOME");if(qwen.empty())qwen=(home/L".qwen").wstring();
    auto geminiHome=Environment(L"GEMINI_CLI_HOME");if(geminiHome.empty())geminiHome=home.wstring();
    auto config=Environment(L"XDG_CONFIG_HOME");if(config.empty())config=(home/L".config").wstring();
    // tokens-core uses the Windows roaming config directory. Keep the old
    // XDG/home candidate for existing caches, but an explicit isolated profile
    // must never discover another account's cache through a fallback root.
    std::vector<fs::path> tokensRoots;
    auto addTokensRoot=[&](const fs::path& supplied) {
        if(supplied.empty())return;
        std::error_code error;auto candidate=fs::absolute(supplied,error);
        if(error)candidate=supplied;
        candidate=candidate.lexically_normal();candidate.make_preferred();
        auto identity=candidate.wstring();std::transform(identity.begin(),identity.end(),identity.begin(),::towlower);
        for(const auto& existing:tokensRoots){auto key=existing.wstring();std::transform(key.begin(),key.end(),key.begin(),::towlower);if(key==identity)return;}
        tokensRoots.push_back(candidate);
    };
    const auto tokensOverride=Environment(L"TOKENS_CONFIG_DIR");
    if(!tokensOverride.empty())addTokensRoot(fs::path(tokensOverride));
    else {if(!roaming.empty())addTokensRoot(roaming/L"tokens");addTokensRoot(fs::path(config)/L"tokens");}
    auto tokensPaths=[&](const fs::path& relative){std::vector<std::wstring> paths;for(const auto& root:tokensRoots)paths.push_back((root/relative).wstring());return paths;};
    auto codebuff=Environment(L"CODEBUFF_DATA_DIR");if(codebuff.empty())codebuff=(fs::path(config)/L"manicode").wstring();
    auto reasonix=Environment(L"REASONIX_STATE_HOME");if(reasonix.empty())reasonix=Environment(L"REASONIX_HOME");if(reasonix.empty())reasonix=(home/L".reasonix").wstring();
    auto jcode=Environment(L"JCODE_HOME");if(jcode.empty())jcode=(home/L".jcode").wstring();
    auto grok=Environment(L"GROK_HOME");if(grok.empty())grok=(home/L".grok").wstring();
    auto editorTasks=[&](const wchar_t* extension) {std::vector<std::wstring> paths;for(auto editor:{L"Code",L"Code - Insiders",L"VSCodium",L"Cursor"})paths.push_back((roaming/editor/L"User"/L"globalStorage"/extension/L"tasks").wstring());return paths;};
    auto clineTasks=editorTasks(L"saoudrizwan.claude-dev");clineTasks.push_back((home/L".cline"/L"data"/L"sessions").wstring());
    using F=SourceFormat;
    return {
      {"chatgpt","ChatGPT",F::UsageExport,{},L"需要包含真实用量的授权导出；普通聊天导出不提供 Token。"},
      {"codex","Codex",F::Codex,{(fs::path(codex)/L"sessions").wstring(),(fs::path(codex)/L"archived_sessions").wstring()},L"本地 JSONL，自动增量读取。"},
      {"claude","Claude Code",F::Claude,{(fs::path(claude)/L"projects").wstring(),(fs::path(claude)/L"transcripts").wstring()},L"本地 JSONL，自动去重读取。"},
      {"cursor","Cursor",F::Extended,tokensPaths(L"cursor-cache"),L"读取同步缓存或 Dashboard CSV；可手动授权同步真实用量，不把 Credits 换算为 Token。"},
      {"copilot","GitHub Copilot",F::Extended,{(home/L".copilot"/L"otel").wstring(),(home/L".copilot"/L"session-state").wstring()},L"OTel 逐请求 chat / inference 与 assistant.usage；忽略 agent/session 汇总。"},
      {"trae-code","TRAE · TraeCode",F::Extended,tokensPaths(fs::path(L"trae-cache")/L"sessions"),L"国际版 TRAE IDE / SOLO 支持手动授权同步或读取已有用量缓存；CN 需真实用量导出。"},
      {"trae-work","TRAE · TraeWork",F::Domestic,{},L"TraeWork 是独立产品，Code 是其工作模式之一；当前需真实 Token 导出，不直连账号。"},
      {"manus","Manus",F::UsageExport,{},L"需真实 Token 导出；任务积分不换算为 Token。"},
      {"gemini-research","Gemini Deep Research",F::UsageExport,{},L"需研究任务实际 usage 导出；不按文本长度推测。"},
      {"kimi","Kimi",F::UsageExport,{},L"需实际用量导出；与 Kimi Code CLI 独立归属。"},
      {"minimax-code","MiniMax Code",F::Domestic,{},L"需实际模型调用用量导出，不从当前配置猜历史模型。"},
      {"minimax-design","MiniMax Design",F::Domestic,{},L"仅文本 Token；图像、视频计费单位不换算。"},
      {"coze","Coze 扣子",F::UsageExport,{},L"支持工作流 history Token 总量；分项未知时保留未分类。"},
      {"dify","Dify",F::UsageExport,{},L"导入 message_end usage 或 workflow_finished；不叠加父子总量。"},
      {"n8n","n8n",F::UsageExport,{},L"需模型调用 usage 导出；执行次数不当作 Token。"},
      {"bailian","阿里云百炼",F::UsageExport,{},L"导入实际模型响应 usage / 规范用量记录。"},
      {"glm-agents","智谱智能体平台",F::UsageExport,{},L"导入实际模型响应 usage / 规范用量记录。"},
      {"openai-agents","OpenAI Agents SDK / API",F::UsageExport,{},L"导入带调用身份和时间的 response usage；不读取凭据。"},
      {"workbuddy","WorkBuddy",F::Domestic,{(home/L".workbuddy"/L"projects").wstring()},L"读取 providerData.rawUsage；积分不作为 Token 或金额。"},
      {"deepseek-harness","DeepSeek Harness",F::Domestic,{(home/L".dsh"/L"sessions").wstring()},L"读取压缩会话，优先 v4，模型来自请求上下文。"},
      {"opencode","OpenCode",F::Cli,{(fs::path(share)/L"opencode"/L"opencode.db").wstring(),(fs::path(share)/L"opencode"/L"storage"/L"message").wstring()},L"只读 SQLite / 旧消息 JSON。"},
      {"codebuddy","CodeBuddy",F::Domestic,{(home/L".codebuddy"/L"projects").wstring()},L"兼容经验证的本地消息用量；无日志时需要导出。"},
      {"qoder","Qoder",F::Domestic,{(home/L".qoder-cn"/L"cache"/L"projects").wstring(),(home/L".qoder"/L"cache"/L"projects").wstring()},L"只读含 Token 的会话；当前缓存可能没有可计量字段。"},
      {"qwen-code","Qwen Code",F::Cli,{(fs::path(qwen)/L"projects").wstring(),(fs::path(qwen)/L"tmp").wstring()},L"读取 projects / 旧 tmp chats JSONL 的 usageMetadata。"},
      {"kimi-code","Kimi Code CLI",F::Cli,{(fs::path(kimiCode)/L"sessions").wstring(),(fs::path(kimi)/L"sessions").wstring()},L"新版 usage.record 与旧版 StatusUpdate；实际模型来自同一日志的请求，缺少时保留未知。"},
      {"gemini-cli","Gemini CLI",F::Cli,{(fs::path(geminiHome)/L".gemini"/L"tmp").wstring()},L"支持 chats JSONL 与旧版会话 JSON。"},
      {"amp","Amp",F::Extended,{(fs::path(share)/L"amp"/L"threads").wstring()},L"读取 usageLedger.events 的逐请求时间与实际用量；不从消息位置伪造日期。"},
      {"antigravity","Antigravity",F::Extended,tokensPaths(fs::path(L"antigravity-cache")/L"sessions"),L"只读已有同步缓存；不连接运行中的 IDE 或触发同步。"},
      {"antigravity-cli","Antigravity CLI",F::Extended,{(home/L".gemini"/L"antigravity-cli"/L"conversations").wstring()},L"只读生成记录；仅接受实际逐次时间，不按会话开始或文件修改时间分摊。"},
      {"cline","Cline",F::Extended,clineTasks,L"读取 IDE API 请求和 CLI metrics；分别按实际保存格式处理缓存。"},
      {"roo-code","Roo Code",F::Extended,editorTasks(L"rooveterinaryinc.roo-cline"),L"读取 ui_messages.json；总输入已含缓存，不重复叠加。"},
      {"kilo-code","Kilo Code",F::Extended,editorTasks(L"kilocode.kilo-code"),L"读取扩展请求用量；与 Kilo CLI 独立统计。"},
      {"kilo-cli","Kilo CLI",F::Extended,{(fs::path(share)/L"kilo"/L"kilo.db").wstring()},L"只读 OpenCode 兼容 message 表的真实逐请求用量。"},
      {"mimo-code","MiMo Code",F::Extended,{(fs::path(share)/L"mimocode").wstring()},L"只读 OpenCode 兼容数据库；不另计 Orca 沙箱副本。"},
      {"codebuff","Codebuff",F::Extended,{(fs::path(codebuff)/L"projects").wstring()},L"读取带调用身份、真实时间和实际 Token 的消息，不计 Credits。"},
      {"commandcode","Command Code",F::Extended,{(home/L".commandcode"/L"projects").wstring()},L"只统计 v3 usage；不按文本长度估算，历史真实调用在 rewind 后仍计入消耗。"},
      {"reasonix","Reasonix",F::Extended,{(fs::path(reasonix)/L"stats").wstring()},L"读取逐请求 stats；跳过 turn 标记和会话正文。"},
      {"pi","Pi",F::Extended,{(home/L".pi"/L"agent"/L"sessions").wstring(),(home/L".omp"/L"agent"/L"sessions").wstring()},L"读取助手消息 usage，真实 ID 跨副本去重；兼容 OMP 目录。"},
      {"openclaw","OpenClaw",F::Extended,{(home/L".openclaw"/L"agents").wstring()},L"读取 JSONL / transcript_events；Codex harness 镜像不完整时需导入原始 Codex 日志。"},
      {"junie","Junie",F::Extended,{(home/L".junie"/L"sessions").wstring()},L"读取 LlmResponseMetadataEvent 真实用量；无逐次时间时不估日期。"},
      {"grok-build","Grok Build",F::Extended,{(fs::path(grok)/L"logs"/L"unified.jsonl").wstring()},L"只统计 inference_done 逐调用用量；旧上下文快照不视为消耗。"},
      {"jcode","Jcode",F::Extended,{(fs::path(jcode)/L"sessions").wstring()},L"读取 assistant token_usage；会话当前模型不倒推到历史消息。"},
      {"devin-cli","Devin CLI",F::Extended,{(fs::path(share)/L"devin"/L"cli"/L"sessions.db").wstring()},L"只读 message_nodes 的真实 metrics 和逐次时间。"},
      {"zcode","ZCode",F::Extended,{(home/L".zcode"/L"cli"/L"db"/L"db.sqlite").wstring()},L"只读 model_usage；累计或缓存拆分不明时保留未知，不做文本估算。"},
      {"opencodereview","OpenCodeReview",F::Extended,{(home/L".opencodereview"/L"sessions").wstring()},L"读取真实 llm_response；无时间/身份或缓存口径不明时需规范导出。"},
      {"crush","Crush",F::UsageExport,{},L"需要实际逐日 Token 导出；本地上下文快照和费用不能当累计 Token。"},
      {"devin-desktop","Devin Desktop",F::UsageExport,{},L"需要含稳定调用 ID 与真实时间的用量导出；ACP 累计快照不按某一天全计。"},
      {"droid","Droid",F::UsageExport,{},L"需要逐请求用量导出；settings 的累计 Token 不按正文比例拆分日期。"},
      {"fx","Fx",F::UsageExport,{},L"需要逐调用用量导出；usage-v2 会话累计不假设为同一天。"},
      {"goose","Goose",F::UsageExport,{},L"需要逐请求用量导出；会话累计不按创建时间全计到一天。"},
      {"hermes","Hermes Agent",F::UsageExport,{},L"需要逐请求或逐日用量导出；session_model_usage 累计不伪造调用日期。"},
      {"kiro","Kiro",F::UsageExport,{},L"需要实际 Token 导出；不把上下文占比或字符估算当真实用量。"},
      {"mux","Mux",F::UsageExport,{},L"需要逐请求用量导出；byModel 累计和 lastRequest 时间不足以还原每日用量。"},
      {"orca","Orca",F::UsageExport,{},L"需要归属明确的实际用量导出；共享 Claude/Codex/MiMo hook 不重复算为新软件。"},
      {"warp","Warp",F::UsageExport,{},L"需要真实 Token 导出；requests/spend 不能换算为 Token。"},
      {"zed","Zed Agent",F::UsageExport,{},L"需要逐请求用量与时间导出；thread 累计不全算到最后更新时间。"}
    };
}
const SourceSpec* FindSource(const std::vector<SourceSpec>& sources,const std::string& id) {
    for(const auto& source:sources)if(source.id==id)return &source;return nullptr;
}
std::string JsonString(yyjson_val* object,const char* key) {
    auto value=yyjson_obj_get(object,key);const char* text=yyjson_get_str(value);
    return text?std::string(text,yyjson_get_len(value)):std::string();
}
bool JsonCounter(yyjson_val* object,const char* key,int64_t& value,bool required) {
    auto field=yyjson_obj_get(object,key);
    if(!field||yyjson_is_null(field))return !required;
    if(!yyjson_is_int(field))return false;
    constexpr int64_t maximum=INT64_MAX/16;
    if(yyjson_is_uint(field)&&yyjson_get_uint(field)>static_cast<uint64_t>(maximum))return false;
    value=yyjson_get_sint(field);return value>=0&&value<=maximum;
}
std::string IsoFromUnixMilliseconds(int64_t milliseconds) {
    if(milliseconds<0||milliseconds>253402300799999LL)return {};
    ULARGE_INTEGER ticks{};ticks.QuadPart=116444736000000000ULL+static_cast<uint64_t>(milliseconds)*10000;
    FILETIME file{ticks.LowPart,ticks.HighPart};SYSTEMTIME time{};
    if(!FileTimeToSystemTime(&file,&time))return {};
    char text[40];std::snprintf(text,sizeof(text),"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds);
    return text;
}
}
