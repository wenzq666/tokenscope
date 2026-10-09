#include "credentials.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace tokenscope;
namespace fs=std::filesystem;
namespace {
void Require(bool condition,const char* text){if(!condition)throw std::runtime_error(text);}
void Write(const fs::path& path,const std::string& value){fs::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary);out.write(value.data(),static_cast<std::streamsize>(value.size()));Require(out.good(),"fixture write failed");}
std::string Read(const fs::path& path){std::ifstream in(path,std::ios::binary);Require(in.good(),"fixture read failed");return {std::istreambuf_iterator<char>(in),{}};}
void FixedError(const std::string& error,const std::string& secret){Require(!error.empty()&&error.find(secret)==std::string::npos,"credential error leaked sensitive input");}
void RoundTrip(const fs::path& root,const std::string& source,const std::string& expected){std::string token="previous-output",error="previous-error";Require(LoadConnectionCredential(root.wstring(),source,token,error)&&token==expected&&error.empty(),"current-user DPAPI roundtrip failed");if(!token.empty())SecureZeroMemory(token.data(),token.size());}
}
int main(){
    const auto temp=fs::current_path()/L"build";const auto root=temp/(L"TokenScope-credentials-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    try{
        const auto data=root/L"用户数据";const auto cursor=data/L"connections"/L"cursor"/L"credential.bin",trae=data/L"connections"/L"trae-code"/L"credential.bin";
        const std::string cursorToken="fixture-cursor-session-user%3A%3Aunique-secret-0123456789",traeToken="fixture.trae.JWT.unique-secret.abcdefghijklmnopqrstuvwxyz";std::string error,loaded;
        Require(!HasConnectionCredential(data.wstring(),"cursor")&&!fs::exists(data),"credential presence check created files");Require(RemoveConnectionCredential(data.wstring(),"cursor",error)&&error.empty()&&!fs::exists(data),"missing credential deletion created directories");
        const bool saved=SaveConnectionCredential(data.wstring(),"cursor",cursorToken,error);if(!saved)throw std::runtime_error("DPAPI credential save failed: "+error+" (Windows "+std::to_string(GetLastError())+")");Require(error.empty(),"successful save retained an error");Require(HasConnectionCredential(data.wstring(),"cursor"),"saved credential was not recognized");RoundTrip(data,"cursor",cursorToken);
        auto cipher=Read(cursor);Require(!cipher.empty()&&cipher.size()<=64*1024&&cipher.find(cursorToken)==std::string::npos,"persisted file contains plaintext or exceeds the blob limit");
        Require(SaveConnectionCredential(data.wstring(),"trae-code",traeToken,error),"second supported source save failed");RoundTrip(data,"trae-code",traeToken);const auto traeCipher=Read(trae);Require(traeCipher.find(traeToken)==std::string::npos,"TRAE persisted plaintext");
        Write(data/L"connections"/L"trae-code"/L"usage.json","source-cache-sentinel");Write(data/L"connections"/L"unrelated-source"/L"keep.txt","unrelated-source-sentinel");
        for(const std::string source:{"../trae-code","cursor/../trae-code","cursor\\..\\trae-code","Cursor","trae-work","","unsupported-agent"}){
            Require(!SaveConnectionCredential(data.wstring(),source,cursorToken,error),"unsupported source accepted a credential");FixedError(error,cursorToken);loaded=cursorToken;Require(!LoadConnectionCredential(data.wstring(),source,loaded,error)&&loaded.empty(),"invalid source load retained old output");Require(!HasConnectionCredential(data.wstring(),source),"invalid source presence accepted");Require(!RemoveConnectionCredential(data.wstring(),source,error),"invalid source deletion accepted");
        }
        Require(Read(cursor)==cipher&&Read(trae)==traeCipher&&Read(data/L"connections"/L"unrelated-source"/L"keep.txt")=="unrelated-source-sentinel","invalid source operation modified another source");
        for(const auto& token:{std::string(),std::string(16*1024+1,'X'),std::string("fixture\r\nheader"),std::string("fixture\0suffix",14)}){Require(!SaveConnectionCredential(data.wstring(),"cursor",token,error)&&!error.empty()&&Read(cursor)==cipher,"empty, oversize or control token replaced a valid credential");}
        Require(!SaveConnectionCredential(L"","cursor",cursorToken,error)&&!error.empty(),"empty data directory accepted");Require(!SaveConnectionCredential(std::wstring(L"bad\0directory",13),"cursor",cursorToken,error),"embedded-NUL directory accepted");
        const std::string largest(16*1024,'Q');Require(SaveConnectionCredential(data.wstring(),"cursor",largest,error),"maximum allowed token was rejected");RoundTrip(data,"cursor",largest);Require(Read(trae)==traeCipher,"cursor replacement changed TRAE credential");
        Require(SaveConnectionCredential(data.wstring(),"cursor",cursorToken,error),"atomic replacement failed");cipher=Read(cursor);RoundTrip(data,"cursor",cursorToken);for(const auto& file:fs::directory_iterator(cursor.parent_path()))Require(file.path().filename()==L"credential.bin","temporary credential file was left behind");
        const auto locked=CreateFileW(cursor.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);Require(locked!=INVALID_HANDLE_VALUE,"atomic failure fixture lock failed");const bool replacedLocked=SaveConnectionCredential(data.wstring(),"cursor","fixture-replacement-must-not-publish",error);CloseHandle(locked);Require(!replacedLocked&&!error.empty()&&Read(cursor)==cipher,"failed atomic publish damaged the old credential");for(const auto& file:fs::directory_iterator(cursor.parent_path()))Require(file.path().filename()==L"credential.bin","failed publish retained a credential temporary file");
        // A valid DPAPI blob copied between sources must fail even under the
        // same Windows user; the optional entropy and envelope bind its source.
        Write(trae,cipher);loaded=traeToken;Require(!LoadConnectionCredential(data.wstring(),"trae-code",loaded,error)&&loaded.empty()&&!HasConnectionCredential(data.wstring(),"trae-code"),"copied ciphertext crossed source identity");FixedError(error,cursorToken);Write(trae,traeCipher);
        auto tampered=cipher;tampered[tampered.size()/2]^=0x5a;Write(cursor,tampered);loaded=cursorToken;Require(!LoadConnectionCredential(data.wstring(),"cursor",loaded,error)&&loaded.empty()&&!HasConnectionCredential(data.wstring(),"cursor"),"tampered DPAPI blob accepted");FixedError(error,cursorToken);
        for(const auto& bytes:{std::string(),cipher.substr(0,16),std::string(64*1024+1,'Z')}){Write(cursor,bytes);loaded=cursorToken;Require(!LoadConnectionCredential(data.wstring(),"cursor",loaded,error)&&loaded.empty(),"empty, truncated or oversize blob accepted");FixedError(error,cursorToken);}
        Write(cursor,tampered);Require(RemoveConnectionCredential(data.wstring(),"cursor",error)&&error.empty()&&!fs::exists(cursor)&&!HasConnectionCredential(data.wstring(),"cursor"),"damaged credential could not be removed");RoundTrip(data,"trae-code",traeToken);Require(Read(data/L"connections"/L"trae-code"/L"usage.json")=="source-cache-sentinel"&&Read(data/L"connections"/L"unrelated-source"/L"keep.txt")=="unrelated-source-sentinel","credential removal touched another source or usage cache");
        loaded=cursorToken;Require(!LoadConnectionCredential(data.wstring(),"cursor",loaded,error)&&loaded.empty(),"missing load retained old secret");FixedError(error,cursorToken);Require(RemoveConnectionCredential(data.wstring(),"cursor",error),"credential removal was not idempotent");
        // A destination that is a directory must fail without replacing data.
        fs::create_directories(cursor);Write(cursor/L"keep.txt","directory-sentinel");Require(!SaveConnectionCredential(data.wstring(),"cursor",cursorToken,error)&&Read(cursor/L"keep.txt")=="directory-sentinel","credential save replaced a directory");Require(!RemoveConnectionCredential(data.wstring(),"cursor",error),"credential deletion removed a directory");
        const auto absolute=fs::absolute(root).lexically_normal();auto tempAbsolute=fs::absolute(temp).lexically_normal();while(tempAbsolute.filename().empty()&&tempAbsolute.has_parent_path()&&tempAbsolute!=tempAbsolute.parent_path())tempAbsolute=tempAbsolute.parent_path();const auto attributes=GetFileAttributesW(absolute.c_str());Require(absolute.parent_path()==tempAbsolute&&absolute.filename().wstring().starts_with(L"TokenScope-credentials-")&&attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"cleanup escaped owned test directory");fs::remove_all(absolute);
        std::cout<<"PASS credentials: real current-user DPAPI, ciphertext, source binding, atomic replacement, limits, tamper, clear-on-failure and isolated removal\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL credentials: "<<e.what()<<"\n";return 1;}
}
