/*
 * @Author       : mark
 * @Date         : 2020-06-27
 * @copyleft Apache 2.0
 */
#include "httpresponse.h"
#include <filesystem>
#include <ctime>
#include <sstream>

using namespace std;

const unordered_map<string, string> HttpResponse::SUFFIX_TYPE = {
    {".html", "text/html; charset=utf-8"},
    {".htm", "text/html; charset=utf-8"},
    {".xml", "text/xml; charset=utf-8"},
    {".xhtml", "application/xhtml+xml"},
    {".txt", "text/plain; charset=utf-8"},
    {".rtf", "application/rtf"},
    {".pdf", "application/pdf"},
    {".doc", "application/msword"},
    {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".png", "image/png"},
    {".gif", "image/gif"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".svg", "image/svg+xml"},
    {".ico", "image/x-icon"},
    {".mp3", "audio/mpeg"},
    {".mp4", "video/mp4"},
    {".mpeg", "video/mpeg"},
    {".mpg", "video/mpeg"},
    {".avi", "video/x-msvideo"},
    {".webm", "video/webm"},
    {".gz", "application/x-gzip"},
    {".tar", "application/x-tar"},
    {".zip", "application/zip"},
    {".css", "text/css; charset=utf-8"},
    {".js", "application/javascript; charset=utf-8"},
    {".json", "application/json; charset=utf-8"},
    {".woff", "font/woff"},
    {".woff2", "font/woff2"},
    {".ttf", "font/ttf"},
    {".eot", "application/vnd.ms-fontobject"},
    {".otf", "font/otf"}};

const unordered_map<int, string> HttpResponse::CODE_STATUS = {
    {200, "OK"},
    {201, "Created"},
    {204, "No Content"},
    {301, "Moved Permanently"},
    {302, "Found"},
    {304, "Not Modified"},
    {400, "Bad Request"},
    {401, "Unauthorized"},
    {403, "Forbidden"},
    {404, "Not Found"},
    {405, "Method Not Allowed"},
    {413, "Payload Too Large"},
    {415, "Unsupported Media Type"},
    {500, "Internal Server Error"},
    {503, "Service Unavailable"}};

const unordered_map<int, string> HttpResponse::CODE_PATH = {
    {400, "/400.html"},
    {401, "/401.html"},
    {403, "/403.html"},
    {404, "/404.html"},
    {500, "/500.html"}};

HttpResponse::HttpResponse()
{
    code_ = -1;
    path_.clear();
    srcDir_.clear();
    isKeepAlive_ = false;
    responseType_ = FILE_RESPONSE;
    mmFile_ = nullptr;
    mmFileStat_ = {0};
}

HttpResponse::~HttpResponse()
{
    UnmapFile();
}

void HttpResponse::InitFromRequest(HttpRequest &request, const std::string &srcDir)
{
    if (mmFile_)
    {
        UnmapFile();
    }
    srcDir_ = srcDir;
    path_ = request.path();
    body_ = request.body();
    isKeepAlive_ = request.IsKeepAlive();
    code_ = -1; // 将在 MakeResponse 中最终确定
    responseType_ = FILE_RESPONSE;

    // 可以按需透传一些请求头到响应
    if (auto userAgent = request.GetHeader("User-Agent"))
    {
        headers_["X-User-Agent"] = userAgent.value();
    }
    if (auto accept = request.GetHeader("Accept"))
    {
        headers_["X-Accept"] = accept.value();
    }
    LOG_INFO("从请求初始化响应: 路径=%s, KeepAlive=%d", path_.c_str(), isKeepAlive_);
}

void HttpResponse::Init(const string &srcDir, const string &path, const string &body,
                        const unordered_map<string, string> &header, bool isKeepAlive, int code)
{
    if (mmFile_)
    {
        UnmapFile();
    }
    code_ = code;
    isKeepAlive_ = isKeepAlive;
    path_ = path;
    body_ = body;
    headers_ = header;
    srcDir_ = srcDir;
    responseType_ = body.empty() ? FILE_RESPONSE : TEXT_RESPONSE;
    mmFile_ = nullptr;
    mmFileStat_ = {0};

    LOG_INFO("初始化响应: 状态码=%d, 路径=%s", code_, path_.c_str());
}

void HttpResponse::MakeResponse(Buffer &buff)
{
    // 根据当前设置自动判定状态码
    if (code_ == -1)
    {
        if (responseType_ == JSON_RESPONSE || responseType_ == HTML_RESPONSE || responseType_ == TEXT_RESPONSE)
        {
            code_ = 200;
        }
        else if (!ShouldServeFile_())
        {
            code_ = 404;
        }
        else if (!(mmFileStat_.st_mode & S_IROTH))
        {
            code_ = 403;
        }
        else
        {
            code_ = 200;
        }
    }

    // 如果是文件响应但状态码为错误，转为错误页面
    if (code_ >= 400 && responseType_ == FILE_RESPONSE)
    {
        ErrorHtml_();
    }

    AddStateLine_(buff);
    AddHeader_(buff);
    AddContent_(buff);
}

bool HttpResponse::ShouldServeFile_()
{
    if (responseType_ != FILE_RESPONSE)
        return false;

    string fullPath = srcDir_ + path_;
    if (stat(fullPath.data(), &mmFileStat_) < 0)
    {
        LOG_WARN("文件不存在: %s", fullPath.c_str());
        return false;
    }
    if (S_ISDIR(mmFileStat_.st_mode))
    {
        LOG_WARN("路径是目录: %s", fullPath.c_str());
        return false;
    }
    return true;
}

char *HttpResponse::File()
{
    return mmFile_;
}

size_t HttpResponse::FileLen() const
{
    return static_cast<size_t>(mmFileStat_.st_size);
}

void HttpResponse::ErrorHtml_()
{
    if (CODE_PATH.count(code_) == 1)
    {
        string errorPath = CODE_PATH.find(code_)->second;
        string fullPath = srcDir_ + errorPath;

        if (stat(fullPath.data(), &mmFileStat_) == 0 && !S_ISDIR(mmFileStat_.st_mode))
        {
            path_ = errorPath;
        }
        else
        {
            // 回退为内置错误页面
            responseType_ = HTML_RESPONSE;
            const string &status = CODE_STATUS.count(code_) ? CODE_STATUS.find(code_)->second : string("Error");
            body_.clear();
            body_.reserve(256);
            body_ = "<html><head><title>";
            body_ += to_string(code_) + " " + status;
            body_ += "</title></head><body><h1>";
            body_ += to_string(code_) + " " + status;
            body_ += "</h1><p>";
            body_ += status;
            body_ += "</p></body></html>";
        }
    }
    LOG_INFO("错误页面: %s", path_.c_str());
}

static inline string HttpDateNowGMT_()
{
    // 生成 RFC 7231 日期头（GMT）
    char buf[64]{0};
    std::time_t t = std::time(nullptr);
    std::tm gmt{};
#if defined(_WIN32)
    gmtime_s(&gmt, &t);
#else
    gmt = *std::gmtime(&t);
#endif
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &gmt);
    return string(buf);
}

void HttpResponse::AddStateLine_(Buffer &buff)
{
    string status;
    if (CODE_STATUS.count(code_) == 1)
    {
        status = CODE_STATUS.find(code_)->second;
    }
    else
    {
        code_ = 500;
        status = CODE_STATUS.find(500)->second;
    }
    buff.Append("HTTP/1.1 " + to_string(code_) + " " + status + "\r\n");
}

void HttpResponse::AddHeader_(Buffer &buff)
{
    // 基本连接头
    buff.Append("Connection: ");
    if (isKeepAlive_)
    {
        buff.Append("keep-alive\r\n");
        buff.Append("Keep-Alive: timeout=120, max=100\r\n");
    }
    else
    {
        buff.Append("close\r\n");
    }

    // 日期头（有利于代理与浏览器缓存行为）
    buff.Append("Date: " + HttpDateNowGMT_() + "\r\n");

    // 类型头
    buff.Append("Content-Type: ");
    switch (responseType_)
    {
    case JSON_RESPONSE:
        buff.Append("application/json; charset=utf-8\r\n");
        break;
    case HTML_RESPONSE:
        buff.Append("text/html; charset=utf-8\r\n");
        break;
    case TEXT_RESPONSE:
        buff.Append("text/plain; charset=utf-8\r\n");
        break;
    case FILE_RESPONSE:
        buff.Append(GetFileType_() + "\r\n");
        break;
    default:
        buff.Append("text/plain; charset=utf-8\r\n");
        break;
    }

    // 自定义头
    for (const auto &[key, value] : headers_)
    {
        buff.Append(key + ": " + value + "\r\n");
    }

    // Cookie 头（多条）
    for (const auto &[name, value] : cookies_)
    {
        (void)name;
        buff.Append("Set-Cookie: " + value + "\r\n");
    }

    // 安全相关头
    buff.Append("X-Content-Type-Options: nosniff\r\n");
    buff.Append("X-Frame-Options: DENY\r\n");
    buff.Append("X-XSS-Protection: 1; mode=block\r\n");

    // 注意：Content-Length 与空行在 AddContent_ 中写入
}

void HttpResponse::AddContent_(Buffer &buff)
{
    switch (responseType_)
    {
    case FILE_RESPONSE:
    {
        if (!ShouldServeFile_())
        {
            ErrorContent(buff, "File not found");
            return;
        }

        int srcFd = open((srcDir_ + path_).data(), O_RDONLY);
        if (srcFd < 0)
        {
            ErrorContent(buff, "Cannot open file");
            return;
        }

        void *mmRet = mmap(0, mmFileStat_.st_size, PROT_READ, MAP_PRIVATE, srcFd, 0);
        if (mmRet == MAP_FAILED)
        {
            ErrorContent(buff, "Cannot map file to memory");
            close(srcFd);
            return;
        }

        mmFile_ = static_cast<char *>(mmRet);
        close(srcFd);

        buff.Append("Content-Length: " + to_string(mmFileStat_.st_size) + "\r\n\r\n");
        // 对于 sendfile/分块发送，可在上层使用 iov/或直接从 File() 读取
        break;
    }
    case JSON_RESPONSE:
    case HTML_RESPONSE:
    case TEXT_RESPONSE:
    {
        buff.Append("Content-Length: " + to_string(body_.size()) + "\r\n\r\n");
        buff.Append(body_);
        break;
    }
    case ERROR_RESPONSE:
    {
        ErrorContent(buff, body_);
        break;
    }
    }
}

void HttpResponse::UnmapFile()
{
    if (mmFile_)
    {
        munmap(mmFile_, mmFileStat_.st_size);
        mmFile_ = nullptr;
    }
}

string HttpResponse::GetFileType_() const
{
    size_t idx = path_.find_last_of('.');
    if (idx == string::npos)
    {
        return "application/octet-stream";
    }
    string suffix = path_.substr(idx);
    auto it = SUFFIX_TYPE.find(suffix);
    if (it != SUFFIX_TYPE.end())
        return it->second;
    return "application/octet-stream";
}

void HttpResponse::ErrorContent(Buffer &buff, const string &message)
{
    const string status = CODE_STATUS.count(code_) ? CODE_STATUS.find(code_)->second : "Error";
    string body;
    body.reserve(256 + message.size());
    body = "<!DOCTYPE html><html><head><title>";
    body += to_string(code_) + " " + status;
    body += "</title><style>body{font-family:Arial,sans-serif;margin:40px;text-align:center}</style></head>";
    body += "<body><h1>";
    body += to_string(code_) + " " + status;
    body += "</h1><p>";
    body += message;
    body += "</p><hr><small>TinyWebServer</small></body></html>";

    buff.Append("Content-Length: " + to_string(body.size()) + "\r\n\r\n");
    buff.Append(body);
}

void HttpResponse::SetJsonResponse(const nlohmann::json &json, int code)
{
    responseType_ = JSON_RESPONSE;
    body_ = json.dump(4); // pretty print
    code_ = code;
    UnmapFile();
}

void HttpResponse::SetJsonResponse(const string &jsonStr, int code)
{
    responseType_ = JSON_RESPONSE;
    body_ = jsonStr;
    code_ = code;
    UnmapFile();
}

void HttpResponse::SetHtmlResponse(const string &content, int code)
{
    responseType_ = HTML_RESPONSE;
    body_ = content;
    code_ = code;
    UnmapFile();
}

void HttpResponse::SetTextResponse(const string &content, int code)
{
    responseType_ = TEXT_RESPONSE;
    body_ = content;
    code_ = code;
    UnmapFile();
}

void HttpResponse::AddHeader(const string &key, const string &value)
{
    headers_[key] = value;
}

void HttpResponse::SetCookie(const string &name, const string &value,
                             const string &path, int maxAge, bool httpOnly, bool secure)
{
    string cookie = name + "=" + value;
    if (!path.empty())
        cookie += "; Path=" + path;
    if (maxAge > 0)
        cookie += "; Max-Age=" + to_string(maxAge);
    if (httpOnly)
        cookie += "; HttpOnly";
    if (secure)
        cookie += "; Secure";
    cookie += "; SameSite=Lax";
    cookies_[name] = cookie;
}

void HttpResponse::RemoveCookie(const string &name)
{
    // 过期 cookie
    cookies_[name] = name + "=; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Path=/";
}

HttpResponse HttpResponse::CreateErrorResponse(int code, const string &message, const HttpRequest *request)
{
    HttpResponse response;
    response.code_ = code;
    response.responseType_ = ERROR_RESPONSE;
    response.body_ = message;
    if (request)
        response.isKeepAlive_ = request->IsKeepAlive();
    return response;
}

HttpResponse HttpResponse::CreateJsonResponse(const nlohmann::json &json, int code, const HttpRequest *request)
{
    HttpResponse response;
    response.SetJsonResponse(json, code);
    if (request)
        response.isKeepAlive_ = request->IsKeepAlive();
    return response;
}

HttpResponse HttpResponse::CreateRedirectResponse(const std::string &location,
                                                  int code,
                                                  const HttpRequest *request)
{
    HttpResponse response;

    // 兜底状态码：非标准3xx都改成302
    if (code != 301 && code != 302 && code != 303 && code != 307 && code != 308)
    {
        code = 302;
    }
    response.code_ = code;
    response.responseType_ = HTML_RESPONSE;

    // 组装 Location 头
    response.headers_["Location"] = location;

    // 状态文字
    const std::string &status = CODE_STATUS.count(code)
                                    ? CODE_STATUS.find(code)->second
                                    : std::string("Redirect");

    // 构建一个简单的HTML body
    std::ostringstream oss;
    oss << "<!doctype html><html><head><meta charset='utf-8'>"
        << "<title>" << code << " " << status << "</title></head>"
        << "<body><h1>" << code << " " << status << "</h1>"
        << "<p>Redirecting to <a href=\"" << location << "\">" << location
        << "</a></p></body></html>";
    response.body_ = oss.str();

    // Content-Length 和 Connection 头（保证浏览器能正确处理响应）
    // response.headers_["Content-Length"] = std::to_string(response.body_.size());
    if (request && request->IsKeepAlive())
    {
        response.isKeepAlive_ = true;
        response.headers_["Connection"] = "keep-alive";
    }
    else
    {
        response.isKeepAlive_ = false;
        response.headers_["Connection"] = "close";
    }

    // 调试信息
    LOG_INFO("=== REDIRECT DEBUG === Creating redirect response Location: %s Code: %d Request keep-alive: %s ======================", 
             location.c_str(), code, (request ? request->IsKeepAlive() : false) ? "true" : "false");

    return response;
}
