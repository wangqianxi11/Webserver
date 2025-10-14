/*
 * @Author       : mark
 * @Date         : 2020-06-25
 * @copyleft Apache 2.0
 */
#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <unordered_map>
#include <unordered_set>
#include <set>
#include <string>
#include <regex>
#include <errno.h>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <optional>
#include <variant>
#include <memory>
#include <mysql/mysql.h>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <cpprest/http_listener.h>
#include <cpprest/json.h>
#include "../buffer/buffer.h"
#include "../log/log.h"
#include "../pool/sqlconnpool.h"
#include "../pool/sqlconnRAII.h"
#include "../processing/uploaded_file.h"

// 配置结构体
struct HttpConfig
{
    size_t max_header_size = 64 * 1024;      // 头部最大 64KB
    size_t max_body_size = 16 * 1024 * 1024; // Body 最大 16MB
    bool allow_chunked_encoding = true;
    std::set<std::string> allowed_methods = {"GET", "POST", "HEAD", "PUT", "DELETE"};
    std::set<std::string> allowed_content_types = {
        "application/x-www-form-urlencoded",
        "multipart/form-data",
        "application/json"};
};

// 错误类型枚举
enum class ParseError
{
    NONE,
    REQUEST_LINE_MALFORMED,
    HEADER_TOO_LARGE,
    BODY_TOO_LARGE,
    CHUNKED_ENCODING_ERROR,
    INVALID_CONTENT_LENGTH,
    UNSUPPORTED_METHOD,
    UNSUPPORTED_CONTENT_TYPE,
    PATH_TRAVERSAL_DETECTED
};

// 解析结果结构体
struct ParseResult
{
    enum PARSE_STATE
    {
        REQUEST_LINE,
        HEADERS,
        BODY_LENGTH,
        BODY_CHUNKED,
        FINISH,
        ERROR,
        AGAIN
    };

    PARSE_STATE state;
    ParseError error;
    std::string error_message;
    size_t error_offset;
};

// Body解析器接口
class BodyParser
{
public:
    virtual ~BodyParser() = default;
    virtual bool Parse(const std::string &contentType,
                       const std::string &body,
                       std::unordered_map<std::string, std::string> &post,
                       int fd) = 0;
    virtual std::string GetName() const = 0;
};

class HttpRequest
{
public:
    explicit HttpRequest(const HttpConfig &config = {});
    ~HttpRequest() = default;

    void Init();
    ParseResult parse(Buffer &buff, int &fd);

    std::string path() const;
    std::string &path();
    std::string method() const;
    std::string version() const;
    std::optional<std::string> GetHeader(const std::string &key) const;
    std::optional<std::string> GetPost(const std::string &key) const;
    std::string &body();
    std::unordered_map<std::string, std::string> &header();
    bool IsKeepAlive() const;
    void SetUserID(int id) { userID_ = id; }
    int GetUserID() const { return userID_; }
    void AddHeader(const std::string &key, const std::string &value);

    // 文件操作相关方法
    void getFileList(const std::string &dirPath, std::vector<std::string> &fileList);
    bool HandleDeleteFile(int user_id);
    void generateFileListPage(const std::string &templatePath,
                              const std::string &outputPath,
                              const std::string &fileDir);
    void Updatepicturehtml(int id);
    void TraverseDirectory(
        const std::string &directory_path,
        std::function<void(const std::string &)> file_handler,
        bool include_hidden = false,
        const std::vector<std::string> &extensions = {});

    static const std::unordered_set<std::string> DEFAULT_HTML;
    static const std::unordered_map<std::string, int> DEFAULT_HTML_TAG;

    // 获取解析错误信息
    ParseError GetLastError() const { return last_error_; }
    std::string GetLastErrorMessage() const { return last_error_message_; }

private:
    bool ParseRequestLine_(const std::string &line);
    void ParseHeader_(const std::string &line);
    void ParseBody_(const std::string &line, int &fd);
    void ParsePath_();
    void ParsePost_(int &fd);
    void ParseFromUrlencoded_();
    bool ParseMultipartForm_(int &fd);
    bool ParseChunkedBody_(Buffer &buff);
    bool UserVerify(const std::string &name, const std::string &pwd, bool isLogin);

    // 辅助方法
    std::string SanitizePath(const std::string &path);
    bool CheckSizeLimits(size_t header_size, size_t body_size);
    bool ValidateMethod(const std::string &method);
    bool ValidateContentType(const std::string &content_type);
    bool ValidateUploadedFile(const UploadedFile &file);

    // Body解析器注册
    void RegisterBodyParsers();

    HttpConfig config_;
    ParseResult::PARSE_STATE state_;
    ParseError last_error_;
    std::string last_error_message_;

    std::string method_, path_, version_, body_;
    std::unordered_map<std::string, std::string> header_;
    std::unordered_map<std::string, std::string> post_;

    std::unordered_map<std::string, std::unique_ptr<BodyParser>> body_parsers_;

    int userID_ = -1;
};

// 具体Body解析器实现
class UrlEncodedParser : public BodyParser
{
public:
    bool Parse(const std::string &contentType,
               const std::string &body,
               std::unordered_map<std::string, std::string> &post,
               int fd) override;
    std::string GetName() const override { return "UrlEncodedParser"; }
};

class MultipartFormDataParser : public BodyParser
{
public:
    bool Parse(const std::string &contentType,
               const std::string &body,
               std::unordered_map<std::string, std::string> &post,
               int fd) override;
    std::string GetName() const override { return "MultipartFormDataParser"; }

private:
    bool ParseMultipartFormData(const std::string &contentType,
                                const std::string &body,
                                UploadedFile &outFile);
};

class JsonBodyParser : public BodyParser
{
public:
    bool Parse(const std::string &contentType,
               const std::string &body,
               std::unordered_map<std::string, std::string> &post,
               int fd) override;
    std::string GetName() const override { return "JsonBodyParser"; }
};

#endif // HTTP_REQUEST_H