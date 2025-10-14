/*
 * @Author       : mark
 * @Date         : 2020-06-26
 * @copyleft Apache 2.0
 */
#include "httprequest.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <iomanip>

using namespace std;

const unordered_set<string> HttpRequest::DEFAULT_HTML{
    "/index", "/register", "/login",
    "/welcome", "/video", "/picture", "/filelist", "/upload", "/showlist", "logout"};

const unordered_map<string, int> HttpRequest::DEFAULT_HTML_TAG{
    {"/register.html", 0}, {"/login.html", 1}, {"/filelist.html", 2}, {"/upload", 3}};

HttpRequest::HttpRequest(const HttpConfig &config) : config_(config)
{
    RegisterBodyParsers();
    Init();
}

void HttpRequest::RegisterBodyParsers()
{
    body_parsers_["application/x-www-form-urlencoded"] = make_unique<UrlEncodedParser>();
    body_parsers_["multipart/form-data"] = make_unique<MultipartFormDataParser>();
    body_parsers_["application/json"] = make_unique<JsonBodyParser>();
}

void HttpRequest::Init()
{
    method_.clear();
    path_.clear();
    version_.clear();
    body_.clear();
    state_ = ParseResult::REQUEST_LINE;
    header_.clear();
    post_.clear();
    userID_ = -1;
    last_error_ = ParseError::NONE;
    last_error_message_.clear();
    LOG_INFO("http请求初始化成功");
}

bool HttpRequest::IsKeepAlive() const
{
    auto conn_header = GetHeader("connection");
    if (!conn_header)
        return version_ == "1.1";

    string v = conn_header.value();
    transform(v.begin(), v.end(), v.begin(), ::tolower);

    if (version_ == "1.1")
    {
        return v != "close";
    }
    else if (version_ == "1.0")
    {
        return v == "keep-alive";
    }
    return false;
}

ParseResult HttpRequest::parse(Buffer &buff, int &fd)
{
    const char CRLF[] = "\r\n";
    ParseResult result;

    if (!CheckSizeLimits(buff.ReadableBytes(), 0))
    {
        state_ = ParseResult::ERROR;
        last_error_ = ParseError::HEADER_TOO_LARGE;
        last_error_message_ = "Header too large";
        result.state = ParseResult::ERROR;
        result.error = last_error_;
        result.error_message = last_error_message_;
        return result;
    }

    while (true)
    {
        if (state_ == ParseResult::REQUEST_LINE)
        {
            const char *lineEnd = search(buff.Peek(), buff.BeginWriteConst(), CRLF, CRLF + 2);
            if (lineEnd == buff.BeginWriteConst())
            {
                result.state = ParseResult::AGAIN;
                return result;
            }

            string line(buff.Peek(), lineEnd);
            buff.RetrieveUntil(lineEnd + 2);
            if (!ParseRequestLine_(line))
            {
                state_ = ParseResult::ERROR;
                result.state = ParseResult::ERROR;
                result.error = last_error_;
                result.error_message = last_error_message_;
                return result;
            }
            ParsePath_();
            state_ = ParseResult::HEADERS;
        }
        else if (state_ == ParseResult::HEADERS)
        {
            const char *lineEnd = search(buff.Peek(), buff.BeginWriteConst(), CRLF, CRLF + 2);
            if (lineEnd == buff.BeginWriteConst())
            {
                result.state = ParseResult::AGAIN;
                return result;
            }

            string line(buff.Peek(), lineEnd);
            buff.RetrieveUntil(lineEnd + 2);

            if (!line.empty())
            {
                ParseHeader_(line);
                if (buff.ReadableBytes() > config_.max_header_size)
                {
                    state_ = ParseResult::ERROR;
                    last_error_ = ParseError::HEADER_TOO_LARGE;
                    last_error_message_ = "Header size exceeds limit";
                    result.state = ParseResult::ERROR;
                    result.error = last_error_;
                    result.error_message = last_error_message_;
                    return result;
                }
            }
            else
            {
                auto encoding_header = GetHeader("transfer-encoding");
                if (encoding_header && encoding_header.value() == "chunked")
                {
                    state_ = ParseResult::BODY_CHUNKED;
                }
                else
                {
                    auto length_header = GetHeader("Content-Length");
                    if (length_header)
                    {
                        try
                        {
                            size_t len = stoull(length_header.value());
                            if (len > config_.max_body_size)
                            {
                                state_ = ParseResult::ERROR;
                                last_error_ = ParseError::BODY_TOO_LARGE;
                                last_error_message_ = "Body size exceeds limit";
                                result.state = ParseResult::ERROR;
                                result.error = last_error_;
                                result.error_message = last_error_message_;
                                return result;
                            }
                            state_ = (len == 0) ? ParseResult::FINISH : ParseResult::BODY_LENGTH;
                        }
                        catch (...)
                        {
                            state_ = ParseResult::ERROR;
                            last_error_ = ParseError::INVALID_CONTENT_LENGTH;
                            last_error_message_ = "Invalid content-length";
                            result.state = ParseResult::ERROR;
                            result.error = last_error_;
                            result.error_message = last_error_message_;
                            return result;
                        }
                    }
                    else
                    {
                        state_ = ParseResult::FINISH;
                    }
                }
            }
        }
        else if (state_ == ParseResult::BODY_LENGTH)
        {
            auto length_header = GetHeader("Content-Length");
            LOG_INFO("body_length=%d",length_header);
            if (!length_header)
            {
                state_ = ParseResult::ERROR;
                last_error_ = ParseError::INVALID_CONTENT_LENGTH;
                last_error_message_ = "Missing content-length";
                result.state = ParseResult::ERROR;
                result.error = last_error_;
                result.error_message = last_error_message_;
                return result;
            }

            try
            {
                size_t need = stoull(length_header.value());
                if (buff.ReadableBytes() < need)
                {
                    result.state = ParseResult::AGAIN;
                    return result;
                }

                body_.assign(buff.Peek(), buff.Peek() + need);
                buff.Retrieve(need);

                if (!CheckSizeLimits(0, body_.size()))
                {
                    state_ = ParseResult::ERROR;
                    result.state = ParseResult::ERROR;
                    result.error = last_error_;
                    result.error_message = last_error_message_;
                    return result;
                }

                ParsePost_(fd);
                state_ = ParseResult::FINISH;
            }
            catch (...)
            {
                state_ = ParseResult::ERROR;
                last_error_ = ParseError::INVALID_CONTENT_LENGTH;
                last_error_message_ = "Invalid content-length value";
                result.state = ParseResult::ERROR;
                result.error = last_error_;
                result.error_message = last_error_message_;
                return result;
            }
        }
        else if (state_ == ParseResult::BODY_CHUNKED)
        {
            if (!ParseChunkedBody_(buff))
            {
                if (state_ == ParseResult::AGAIN)
                {
                    result.state = ParseResult::AGAIN;
                    return result;
                }
                result.state = ParseResult::ERROR;
                result.error = last_error_;
                result.error_message = last_error_message_;
                return result;
            }
            else
            {
                ParsePost_(fd);
                state_ = ParseResult::FINISH;
            }
        }
        else if (state_ == ParseResult::FINISH)
        {
            break;
        }
        else
        {
            result.state = ParseResult::ERROR;
            result.error = last_error_;
            result.error_message = last_error_message_;
            return result;
        }
    }

    LOG_INFO("解析完成: method=%s path=%s HTTP/%s keep-alive=%d body=%zu",
             method_.c_str(), path_.c_str(), version_.c_str(),
             IsKeepAlive(), body_.size());

    result.state = ParseResult::FINISH;
    return result;
}

bool HttpRequest::ParseChunkedBody_(Buffer &buff)
{
    const char CRLF[] = "\r\n";
    while (true)
    {
        const char *lineEnd = search(buff.Peek(), buff.BeginWriteConst(), CRLF, CRLF + 2);
        if (lineEnd == buff.BeginWriteConst())
        {
            state_ = ParseResult::AGAIN;
            return false;
        }

        string szline(buff.Peek(), lineEnd);
        buff.RetrieveUntil(lineEnd + 2);

        size_t semi = szline.find(';');
        if (semi != string::npos)
            szline = szline.substr(0, semi);

        size_t chunk_size = 0;
        try
        {
            chunk_size = stoul(szline, nullptr, 16);
        }
        catch (...)
        {
            state_ = ParseResult::ERROR;
            last_error_ = ParseError::CHUNKED_ENCODING_ERROR;
            last_error_message_ = "Invalid chunk size";
            return false;
        }

        if (chunk_size == 0)
        {
            return true;
        }

        if (buff.ReadableBytes() < chunk_size + 2)
        {
            state_ = ParseResult::AGAIN;
            return false;
        }

        if (body_.size() + chunk_size > config_.max_body_size)
        {
            state_ = ParseResult::ERROR;
            last_error_ = ParseError::BODY_TOO_LARGE;
            last_error_message_ = "Chunked body too large";
            return false;
        }

        body_.append(buff.Peek(), chunk_size);
        buff.Retrieve(chunk_size);

        if (buff.ReadableBytes() < 2 || strncmp(buff.Peek(), CRLF, 2) != 0)
        {
            state_ = ParseResult::ERROR;
            last_error_ = ParseError::CHUNKED_ENCODING_ERROR;
            last_error_message_ = "Missing chunk terminator";
            return false;
        }
        buff.Retrieve(2);
    }
}

void HttpRequest::ParsePath_()
{
    if (path_.empty())
    {
        path_ = "/index.html";
        return;
    }
    if (path_ == "/")
    {
        path_ = "/index.html";
        return;
    }

    path_ = SanitizePath(path_);

    for (const auto &item : DEFAULT_HTML)
    {
        if (item == path_)
        {
            path_ += ".html";
            break;
        }
    }
    LOG_INFO("规范化路径: %s", path_.c_str());
}

bool HttpRequest::ParseRequestLine_(const std::string &line)
{
    static const regex patten(R"(^([^ ]+)\s+([^ ]+)\s+HTTP/([^ ]+)$)");
    smatch m;
    if (!regex_match(line, m, patten))
    {
        last_error_ = ParseError::REQUEST_LINE_MALFORMED;
        last_error_message_ = "Bad request line: " + line;
        LOG_ERROR("Bad request line: %s", line.c_str());
        return false;
    }

    method_ = m[1];
    path_ = m[2];
    version_ = m[3];

    transform(method_.begin(), method_.end(), method_.begin(), ::toupper);

    if (!ValidateMethod(method_))
    {
        last_error_ = ParseError::UNSUPPORTED_METHOD;
        last_error_message_ = "Unsupported method: " + method_;
        return false;
    }

    return true;
}

void HttpRequest::ParseHeader_(const std::string &line)
{
    size_t p = line.find(':');
    if (p == string::npos)
        return;

    string key = line.substr(0, p);
    size_t i = p + 1;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    string val = (i < line.size() ? line.substr(i) : "");
    header_[key] = val;
}

void HttpRequest::ParseBody_(const string &line, int &fd)
{
    body_ = line;
    ParsePost_(fd);
    state_ = ParseResult::FINISH;
    LOG_INFO("Body:%s, len:%d", line.c_str(), line.size());
}

void HttpRequest::ParsePost_(int &fd)
{
    LOG_INFO("ParsePost_ called, method=%s body size=%zu",
        method_.c_str(), body_.size());

    if (method_ != "POST")
        return;

    auto content_type_header = GetHeader("Content-Type");
    if (!content_type_header)
    content_type_header = GetHeader("content-type");
    if (!content_type_header)
        return;

    string content_type = content_type_header.value();
    if (!ValidateContentType(content_type))
    {
        last_error_ = ParseError::UNSUPPORTED_CONTENT_TYPE;
        last_error_message_ = "Unsupported content type: " + content_type;
        return;
    }

    // 查找合适的解析器
    for (const auto &[type, parser] : body_parsers_)
    {
        if (content_type.find(type) != string::npos)
        {
            if (parser->Parse(content_type, body_, post_, fd))
            {
                return;
            }
            break;
        }
    }

    LOG_WARN("No suitable parser found for content type: %s", content_type.c_str());
}

// UrlEncodedParser 实现
bool UrlEncodedParser::Parse(const std::string &contentType,
                             const std::string &body,
                             std::unordered_map<std::string, std::string> &post,
                             int fd)
{
    // std::cerr << "UrlEncodedParser::Parse called, body='" << body << "'\n";
    if (body.empty())
        return true;
    
    auto UrlDecode = [](const std::string &s) -> std::string
    {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i)
        {
            char ch = s[i];
            if (ch == '+')
            {
                out.push_back(' ');
            }
            else if (ch == '%' && i + 2 < s.size() &&
                     isxdigit((unsigned char)s[i + 1]) &&
                     isxdigit((unsigned char)s[i + 2]))
            {
                auto hex = [](char c) -> int
                {
                    if (c >= '0' && c <= '9')
                        return c - '0';
                    if (c >= 'a' && c <= 'f')
                        return 10 + (c - 'a');
                    if (c >= 'A' && c <= 'F')
                        return 10 + (c - 'A');
                    return 0;
                };
                int v = (hex(s[i + 1]) << 4) | hex(s[i + 2]);
                out.push_back((char)v);
                i += 2;
            }
            else
            {
                out.push_back(ch);
            }
        }
        return out;
    };

    size_t start = 0;
    while (start < body.size())
    {
        size_t eq = body.find('=', start);
        if (eq == std::string::npos)
            break;

        size_t amp = body.find('&', eq + 1);
        std::string key = UrlDecode(body.substr(start, eq - start));
        std::string val = UrlDecode(body.substr(eq + 1, (amp == std::string::npos ? body.size() : amp) - (eq + 1)));

        if (!key.empty())
            post[key] = val;
        if (amp == std::string::npos)
            break;
        start = amp + 1;
    }

    return true;
}

// MultipartFormDataParser 实现
bool MultipartFormDataParser::Parse(const std::string &contentType,
                                    const std::string &body,
                                    std::unordered_map<std::string, std::string> &post,
                                    int fd)
{
    UploadedFile file;
    if (ParseMultipartFormData(contentType, body, file))
    {
        // 处理上传的文件
        // 这里可以添加文件保存逻辑
        post["filename"] = file.filename;
        post["content_type"] = file.contentType;
        return true;
    }
    return false;
}

bool MultipartFormDataParser::ParseMultipartFormData(const std::string &contentType,
                                                     const std::string &body,
                                                     UploadedFile &outFile)
{
    const std::string k = "boundary=";
    size_t p = contentType.find(k);
    if (p == std::string::npos)
    {
        LOG_ERROR("boundary not found");
        return false;
    }

    std::string b = contentType.substr(p + k.size());
    if (!b.empty() && b.front() == '"')
    {
        size_t q = b.find('"', 1);
        if (q == std::string::npos)
            return false;
        b = b.substr(1, q - 1);
    }

    std::string boundary = "--" + b;
    std::string endBoundary = boundary + "--";

    size_t idx = 0;
    while (true)
    {
        size_t partStart = body.find(boundary, idx);
        if (partStart == std::string::npos)
            break;
        partStart += boundary.size();

        if (partStart + 2 <= body.size() && body.compare(partStart, 2, "--") == 0)
        {
            break;
        }

        if (partStart + 2 > body.size() || body.compare(partStart, 2, "\r\n") != 0)
        {
            LOG_ERROR("Malformed boundary CRLF");
            return false;
        }
        partStart += 2;

        size_t partEnd = body.find(boundary, partStart);
        if (partEnd == std::string::npos)
        {
            return false;
        }

        std::string part = body.substr(partStart, partEnd - partStart);
        size_t headerEnd = part.find("\r\n\r\n");
        if (headerEnd == std::string::npos)
            return false;

        std::string headers = part.substr(0, headerEnd);
        std::string content = part.substr(headerEnd + 4);

        size_t fnPos = headers.find("filename=\"");
        if (fnPos == std::string::npos)
        {
            idx = partEnd;
            continue;
        }

        fnPos += 10;
        size_t fnEnd = headers.find("\"", fnPos);
        if (fnEnd == std::string::npos)
            return false;

        std::string raw = headers.substr(fnPos, fnEnd - fnPos);
        std::string filename = raw.substr(raw.find_last_of("/\\") + 1);
        if (filename.empty())
            return false;

        std::string file_type = "application/octet-stream";
        size_t ctype_pos = headers.find("Content-Type:");
        if (ctype_pos != std::string::npos)
        {
            size_t line_end = headers.find("\r\n", ctype_pos);
            std::string line = headers.substr(ctype_pos, (line_end == std::string::npos ? headers.size() : line_end) - ctype_pos);
            size_t sep = line.find(":");
            if (sep != std::string::npos)
            {
                file_type = line.substr(sep + 1);
                while (!file_type.empty() && isspace((unsigned char)file_type.front()))
                    file_type.erase(file_type.begin());
                while (!file_type.empty() && isspace((unsigned char)file_type.back()))
                    file_type.pop_back();
            }
        }

        if (content.size() >= 2 && content.compare(content.size() - 2, 2, "\r\n") == 0)
            content.erase(content.size() - 2);

        outFile.filename = filename;
        outFile.content = std::move(content);
        outFile.contentType = file_type;
        return true;
    }
    return false;
}

// JsonBodyParser 实现
bool JsonBodyParser::Parse(const std::string &contentType,
                           const std::string &body,
                           std::unordered_map<std::string, std::string> &post,
                           int fd)
{
    try
    {
        auto json = nlohmann::json::parse(body);
        if (json.is_object())
        {
            for (auto &[key, value] : json.items())
            {
                if (value.is_string())
                {
                    post[key] = value.get<std::string>();
                }
                else
                {
                    post[key] = value.dump();
                }
            }
        }
        return true;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("JSON parse error: %s", e.what());
        return false;
    }
}

// 辅助方法实现
std::string HttpRequest::SanitizePath(const std::string &path)
{
    if (path.find("../") != std::string::npos ||
        path.find("..\\") != std::string::npos)
    {
        LOG_WARN("Potential path traversal attack: %s", path.c_str());
        return "/index.html";
    }
    return path;
}

bool HttpRequest::CheckSizeLimits(size_t header_size, size_t body_size)
{
    if (header_size > config_.max_header_size)
    {
        last_error_ = ParseError::HEADER_TOO_LARGE;
        last_error_message_ = "Header size exceeds limit";
        return false;
    }
    if (body_size > config_.max_body_size)
    {
        last_error_ = ParseError::BODY_TOO_LARGE;
        last_error_message_ = "Body size exceeds limit";
        return false;
    }
    return true;
}

bool HttpRequest::ValidateMethod(const std::string &method)
{
    return config_.allowed_methods.find(method) != config_.allowed_methods.end();
}

bool HttpRequest::ValidateContentType(const std::string &content_type)
{
    for (const auto &allowed_type : config_.allowed_content_types)
    {
        if (content_type.find(allowed_type) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

bool HttpRequest::ValidateUploadedFile(const UploadedFile &file)
{
    static const std::set<std::string> allowed_types = {
        "image/jpeg", "image/png", "image/gif", "text/plain", "application/pdf"};

    if (allowed_types.find(file.contentType) == allowed_types.end())
    {
        LOG_WARN("Rejected file type: %s", file.contentType.c_str());
        return false;
    }

    if (file.content.size() > 10 * 1024 * 1024)
    {
        LOG_WARN("File too large: %zu bytes", file.content.size());
        return false;
    }

    return true;
}

// 其他方法实现
std::string HttpRequest::path() const { return path_; }
std::string &HttpRequest::path() { return path_; }
std::string HttpRequest::method() const { return method_; }
std::string HttpRequest::version() const { return version_; }
std::string &HttpRequest::body() { return body_; }
std::unordered_map<std::string, std::string> &HttpRequest::header() { return header_; }

std::optional<std::string> HttpRequest::GetHeader(const std::string &key) const
{
    if (auto it = header_.find(key); it != header_.end())
    {
        return it->second;
    }
    return std::nullopt;
}

std::optional<std::string> HttpRequest::GetPost(const std::string &key) const
{
    if (auto it = post_.find(key); it != post_.end())
    {
        return it->second;
    }
    return std::nullopt;
}

void HttpRequest::AddHeader(const std::string &k, const std::string &v)
{
    header_[k] = v;
}
void HttpRequest::getFileList(const std::string &dirPath, std::vector<std::string> &fileList)
{
    DIR *dir;
    struct dirent *ent;

    if ((dir = opendir(dirPath.c_str())) != nullptr)
    {
        while ((ent = readdir(dir)) != nullptr)
        {
            std::string filename = ent->d_name;
            if (filename != "." && filename != "..")
            {
                fileList.push_back(filename);
            }
        }
        closedir(dir);
    }
}

void HttpRequest::generateFileListPage(const std::string &templatePath,
                                       const std::string &outputPath,
                                       const std::string &fileDir)
{
    std::vector<std::string> files;
    getFileList(fileDir, files);

    std::ifstream templateFile(templatePath);
    std::ofstream outputFile(outputPath);
    std::string line;

    while (std::getline(templateFile, line))
    {
        outputFile << line << "\n";
        if (line.find("<!--filelist_label-->") != std::string::npos)
        {
            break;
        }
    }

    for (const auto &filename : files)
    {
        outputFile << "            <tr>"
                   << "<td class=\"col1\">" << filename << "</td>"
                   << "<td class=\"col2\"><a href=\"download/" << filename << "\">下载</a></td>"
                   << "<td class=\"col3\"><a href=\"delete/" << filename
                   << "\" onclick=\"return confirmDelete();\">删除</a></td>"
                   << "</tr>\n";
    }

    while (std::getline(templateFile, line))
    {
        outputFile << line << "\n";
    }
}

void HttpRequest::Updatepicturehtml(int user_id)
{
    MYSQL *sql = nullptr;
    SqlConnRAII sql_raii(&sql, SqlConnPool::Instance());

    std::string query = "SELECT stored_filename FROM uploaded_files WHERE uploader_id = " + std::to_string(user_id);
    if (mysql_query(sql, query.c_str()) != 0)
    {
        LOG_ERROR("查询用户上传记录失败: %s", mysql_error(sql));
        return;
    }

    MYSQL_RES *res = mysql_store_result(sql);
    if (!res)
    {
        LOG_ERROR("获取结果失败: %s", mysql_error(sql));
        return;
    }

    std::stringstream img_tags;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr)
    {
        std::string stored_filename = row[0];
        img_tags << "<div align=\"center\" width=\"906\" height=\"506\">\n"
                    "<img src=\"images/"
                 << stored_filename << "\" />\n"
                                       "</div>\n";
    }

    mysql_free_result(res);
    LOG_INFO("已根据用户 MySQL 数据更新 HTML 页面");
}