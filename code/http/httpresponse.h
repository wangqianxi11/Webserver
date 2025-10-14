/*
 * @Author       : mark
 * @Date         : 2020-06-25
 * @copyleft Apache 2.0
 */
#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <unordered_map>
#include <fcntl.h>    // open
#include <unistd.h>   // close
#include <sys/stat.h> // stat
#include <sys/mman.h> // mmap, munmap
#include <nlohmann/json.hpp>
#include "../buffer/buffer.h"
#include "../log/log.h"
#include "httprequest.h"
#include "../processing/uploadservice.h"

class HttpResponse
{
public:
    HttpResponse();
    ~HttpResponse();

    void InitFromRequest(HttpRequest &request, const std::string &srcDir);
    void Init(const std::string &srcDir, const std::string &path, const std::string &body,
              const std::unordered_map<std::string, std::string> &header,
              bool isKeepAlive = false, int code = -1);

    void MakeResponse(Buffer &buff);
    void UnmapFile();
    char *File();
    size_t FileLen() const;
    void ErrorContent(Buffer &buff, const std::string &message);
    int Code() const { return code_; }

    void SetJsonResponse(const nlohmann::json &json, int code = 200);
    void SetJsonResponse(const std::string &jsonStr, int code = 200);
    void SetHtmlResponse(const std::string &content, int code = 200);
    void SetTextResponse(const std::string &content, int code = 200);

    void AddHeader(const std::string &key, const std::string &value);
    void SetCookie(const std::string &name, const std::string &value,
                   const std::string &path = "/", int maxAge = 3600,
                   bool httpOnly = true, bool secure = false);
    void RemoveCookie(const std::string &name);

    // 静态响应生成方法
    static HttpResponse CreateErrorResponse(int code, const std::string &message,
                                            const HttpRequest *request = nullptr);
    static HttpResponse CreateJsonResponse(const nlohmann::json &json, int code = 200,
                                           const HttpRequest *request = nullptr);
    static HttpResponse CreateRedirectResponse(const std::string &location, int code = 302,
                                               const HttpRequest *request = nullptr);

    char *mmFile_;
    struct stat mmFileStat_;

private:
    enum ResponseType
    {
        FILE_RESPONSE, // 文件响应
        JSON_RESPONSE, // JSON响应
        HTML_RESPONSE, // HTML内容响应
        TEXT_RESPONSE, // 纯文本响应
        ERROR_RESPONSE // 错误响应
    };
    void AddStateLine_(Buffer &buff);
    void AddHeader_(Buffer &buff);
    void AddContent_(Buffer &buff);
    void ErrorHtml_();
    std::string GetFileType_() const;
    bool ShouldServeFile_();

    int code_;
    bool isKeepAlive_;
    ResponseType responseType_;
    std::string body_;
    std::string path_;
    std::string srcDir_;
    std::unordered_map<std::string, std::string> headers_;
    std::unordered_map<std::string, std::string> cookies_;

    static const std::unordered_map<std::string, std::string> SUFFIX_TYPE;
    static const std::unordered_map<int, std::string> CODE_STATUS;
    static const std::unordered_map<int, std::string> CODE_PATH;
};

#endif // HTTP_RESPONSE_H