#include "credentials.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dpapi.h>
#include <bcrypt.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <vector>

namespace tokenscope {
namespace {
namespace fs=std::filesystem;
constexpr size_t kMaxToken=16*1024,kMaxBlob=64*1024,kDigestSize=32;
constexpr char kMagic[]="TokenScope credential v1\n";
struct SensitiveBytes {
    std::vector<unsigned char> value;
    explicit SensitiveBytes(size_t size):value(size){}
    ~SensitiveBytes(){if(!value.empty())SecureZeroMemory(value.data(),value.size());}
};
struct LocalBlob {
    DATA_BLOB value{};
    ~LocalBlob(){if(value.pbData){SecureZeroMemory(value.pbData,value.cbData);LocalFree(value.pbData);}}
};
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    void Close(){if(value!=INVALID_HANDLE_VALUE){CloseHandle(value);value=INVALID_HANDLE_VALUE;}}
};
struct TemporaryFile {
    fs::path path;bool published=false;
    ~TemporaryFile(){if(!published&&!path.empty())DeleteFileW(path.c_str());}
};
void ClearToken(std::string& token){if(!token.empty())SecureZeroMemory(token.data(),token.size());token.clear();}
bool Allowed(const std::string& source){return source=="cursor"||source=="trae-code";}
bool ValidToken(const std::string& token){return !token.empty()&&token.size()<=kMaxToken&&token.find_first_of(std::string("\0\r\n",3))==std::string::npos;}
bool Missing(DWORD code){return code==ERROR_FILE_NOT_FOUND||code==ERROR_PATH_NOT_FOUND;}
bool Fail(std::string& error,const char* text){error=text;return false;}
bool Sha256(const unsigned char* bytes,size_t size,unsigned char* digest){
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;DWORD objectSize=0,received=0;bool ok=false;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
    if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&received,0)>=0&&objectSize<=1024*1024){
        SensitiveBytes object(objectSize);
        if(BCryptCreateHash(algorithm,&hash,object.value.data(),objectSize,nullptr,0,0)>=0){
            ok=BCryptHashData(hash,const_cast<PUCHAR>(bytes),static_cast<ULONG>(size),0)>=0&&BCryptFinishHash(hash,digest,kDigestSize,0)>=0;
            BCryptDestroyHash(hash);
        }
    }
    BCryptCloseAlgorithmProvider(algorithm,0);return ok;
}
std::string EnvelopePrefix(const std::string& source){return std::string(kMagic)+source+'\n';}
DATA_BLOB Entropy(std::string& bytes){return {static_cast<DWORD>(bytes.size()),reinterpret_cast<BYTE*>(bytes.data())};}
bool FilePath(const std::wstring& dataDirectory,const std::string& source,bool create,fs::path& file,std::string& error){
    if(!Allowed(source))return Fail(error,"此来源不支持保存连接凭据。");
    if(dataDirectory.empty()||dataDirectory.find(L'\0')!=std::wstring::npos)return Fail(error,"凭据存储目录无效。");
    std::error_code ec;auto base=fs::absolute(fs::path(dataDirectory),ec).lexically_normal();if(ec)return Fail(error,"凭据存储目录无效。");
    const auto folder=base/L"connections"/std::wstring(source.begin(),source.end());
    for(const auto& directory:{base,base/L"connections",folder}){
        DWORD attributes=GetFileAttributesW(directory.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES){
            if(!Missing(GetLastError()))return Fail(error,"无法访问凭据存储目录。");
            if(!create)continue;
            fs::create_directories(directory,ec);if(ec)return Fail(error,"无法创建凭据存储目录。");attributes=GetFileAttributesW(directory.c_str());
        }
        if(attributes!=INVALID_FILE_ATTRIBUTES&&(!(attributes&FILE_ATTRIBUTE_DIRECTORY)||(attributes&FILE_ATTRIBUTE_REPARSE_POINT)))return Fail(error,"凭据存储目录不能是文件或链接。");
        if(create&&attributes==INVALID_FILE_ATTRIBUTES)return Fail(error,"无法访问凭据存储目录。");
    }
    file=folder/L"credential.bin";
    const DWORD attributes=GetFileAttributesW(file.c_str());
    if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return Fail(error,"凭据文件不能是目录或链接。");
    if(attributes==INVALID_FILE_ATTRIBUTES&&!Missing(GetLastError()))return Fail(error,"无法访问凭据文件。");
    return true;
}
bool AtomicWrite(const fs::path& file,const DATA_BLOB& blob,std::string& error){
    TemporaryFile temporary;Handle output;
    for(int attempt=0;attempt<8;++attempt){
        std::array<unsigned char,16> random{};if(BCryptGenRandom(nullptr,random.data(),static_cast<ULONG>(random.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return Fail(error,"无法创建安全凭据临时文件。");
        std::wstring name=L"credential-";constexpr wchar_t digits[]=L"0123456789abcdef";for(auto byte:random){name+=digits[byte>>4];name+=digits[byte&15];}name+=L".tmp";
        const auto candidate=file.parent_path()/name;
        output.value=CreateFileW(candidate.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(output.value!=INVALID_HANDLE_VALUE){temporary.path=candidate;break;}
        if(GetLastError()!=ERROR_FILE_EXISTS&&GetLastError()!=ERROR_ALREADY_EXISTS)return Fail(error,"无法写入凭据文件。");
    }
    if(output.value==INVALID_HANDLE_VALUE)return Fail(error,"无法创建安全凭据临时文件。");
    DWORD offset=0;while(offset<blob.cbData){DWORD written=0;if(!WriteFile(output.value,blob.pbData+offset,blob.cbData-offset,&written,nullptr)||!written)return Fail(error,"无法写入凭据文件。");offset+=written;}
    if(!FlushFileBuffers(output.value))return Fail(error,"无法保存凭据文件。");output.Close();
    if(!MoveFileExW(temporary.path.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return Fail(error,"无法替换凭据文件。");
    temporary.published=true;return true;
}
}

bool SaveConnectionCredential(const std::wstring& dataDirectory,const std::string& sourceId,const std::string& token,std::string& error){
    error.clear();
    try{
        if(!Allowed(sourceId))return Fail(error,"此来源不支持保存连接凭据。");
        if(!ValidToken(token))return Fail(error,"连接令牌为空、过长或包含无效字符。");
        const auto prefix=EnvelopePrefix(sourceId);const size_t unsignedSize=prefix.size()+sizeof(uint32_t)+token.size();SensitiveBytes plain(unsignedSize+kDigestSize);
        std::memcpy(plain.value.data(),prefix.data(),prefix.size());const auto count=static_cast<uint32_t>(token.size());for(size_t i=0;i<sizeof(count);++i)plain.value[prefix.size()+i]=static_cast<unsigned char>(count>>(8*i));
        std::memcpy(plain.value.data()+prefix.size()+sizeof(count),token.data(),token.size());if(!Sha256(plain.value.data(),unsignedSize,plain.value.data()+unsignedSize))return Fail(error,"无法加密连接凭据。");
        DATA_BLOB input{static_cast<DWORD>(plain.value.size()),plain.value.data()};auto entropyBytes=EnvelopePrefix(sourceId);auto entropy=Entropy(entropyBytes);LocalBlob protectedData;
        if(!CryptProtectData(&input,L"TokenScope connection credential",&entropy,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&protectedData.value)||!protectedData.value.cbData||protectedData.value.cbData>kMaxBlob)return Fail(error,"无法加密连接凭据。");
        fs::path file;if(!FilePath(dataDirectory,sourceId,true,file,error))return false;
        return AtomicWrite(file,protectedData.value,error);
    }catch(...){return Fail(error,"无法保存连接凭据。");}
}
bool LoadConnectionCredential(const std::wstring& dataDirectory,const std::string& sourceId,std::string& token,std::string& error){
    ClearToken(token);error.clear();
    try{
        fs::path file;if(!FilePath(dataDirectory,sourceId,false,file,error))return false;
        Handle input;input.value=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(input.value==INVALID_HANDLE_VALUE)return Fail(error,Missing(GetLastError())?"尚未保存此来源的连接凭据。":"无法读取连接凭据。");
        BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
        if(!GetFileInformationByHandle(input.value,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||!GetFileSizeEx(input.value,&size)||size.QuadPart<=0||size.QuadPart>static_cast<LONGLONG>(kMaxBlob))return Fail(error,"连接凭据文件无效。");
        SensitiveBytes encrypted(static_cast<size_t>(size.QuadPart));DWORD offset=0;while(offset<encrypted.value.size()){DWORD read=0;if(!ReadFile(input.value,encrypted.value.data()+offset,static_cast<DWORD>(encrypted.value.size()-offset),&read,nullptr)||!read)return Fail(error,"无法读取连接凭据。");offset+=read;}
        DATA_BLOB cipher{static_cast<DWORD>(encrypted.value.size()),encrypted.value.data()};auto entropyBytes=EnvelopePrefix(sourceId);auto entropy=Entropy(entropyBytes);LocalBlob plain;
        if(!CryptUnprotectData(&cipher,nullptr,&entropy,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&plain.value))return Fail(error,"连接凭据无法解密或已损坏。");
        const auto prefix=EnvelopePrefix(sourceId);const size_t minimum=prefix.size()+sizeof(uint32_t)+kDigestSize;
        if(plain.value.cbData<minimum||std::memcmp(plain.value.pbData,prefix.data(),prefix.size()))return Fail(error,"连接凭据内容无效。");
        uint32_t count=0;for(size_t i=0;i<sizeof(count);++i)count|=static_cast<uint32_t>(plain.value.pbData[prefix.size()+i])<<(8*i);
        if(!count||count>kMaxToken||plain.value.cbData!=minimum+count)return Fail(error,"连接凭据内容无效。");
        SensitiveBytes digest(kDigestSize);const auto unsignedSize=static_cast<size_t>(plain.value.cbData)-kDigestSize;if(!Sha256(plain.value.pbData,unsignedSize,digest.value.data()))return Fail(error,"连接凭据校验失败。");
        unsigned char mismatch=0;for(size_t i=0;i<kDigestSize;++i)mismatch|=digest.value[i]^plain.value.pbData[unsignedSize+i];if(mismatch)return Fail(error,"连接凭据校验失败。");
        const auto* bytes=reinterpret_cast<const char*>(plain.value.pbData+prefix.size()+sizeof(count));for(uint32_t i=0;i<count;++i)if(!bytes[i]||bytes[i]=='\r'||bytes[i]=='\n')return Fail(error,"连接凭据内容无效。");
        token.assign(bytes,count);return true;
    }catch(...){ClearToken(token);return Fail(error,"无法读取连接凭据。");}
}
bool HasConnectionCredential(const std::wstring& dataDirectory,const std::string& sourceId){
    std::string token,error;const bool loaded=LoadConnectionCredential(dataDirectory,sourceId,token,error);ClearToken(token);return loaded;
}
bool RemoveConnectionCredential(const std::wstring& dataDirectory,const std::string& sourceId,std::string& error){
    error.clear();
    try{fs::path file;if(!FilePath(dataDirectory,sourceId,false,file,error))return false;if(DeleteFileW(file.c_str())||Missing(GetLastError()))return true;return Fail(error,"无法删除此来源的连接凭据。");}
    catch(...){return Fail(error,"无法删除此来源的连接凭据。");}
}
} // namespace tokenscope
