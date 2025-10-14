#include "httpconn.h"
#include <nlohmann/json.hpp>
#include <iostream>

using namespace std;

const char *HttpConn::srcDir;
std::atomic<int> HttpConn::userCount;
bool HttpConn::isET;

HttpConn::HttpConn() : request_(HttpConfig{})
{
    fd_ = -1;
    addr_ = {0};
    isClose_ = true;
    redis_ = std::make_shared<sw::redis::Redis>("tcp://127.0.0.1:6379");
    authService_ = std::make_unique<AuthService>(redis_);
}

HttpConn::~HttpConn()
{
    Close();
}

void HttpConn::init(int fd, const sockaddr_in &addr)
{
    userCount++;
    addr_ = addr;
    fd_ = fd;
    writeBuff_.RetrieveAll();
    readBuff_.RetrieveAll();
    isClose_ = false;
    LOG_INFO("Client[%d](%s:%d) in, userCount:%d", fd_, GetIP(), GetPort(), (int)userCount);
}

void HttpConn::Close()
{
    response_.UnmapFile();
    if (!isClose_)
    {
        isClose_ = true;
        userCount--;
        close(fd_);
        LOG_INFO("Client[%d](%s:%d) quit, UserCount:%d", fd_, GetIP(), GetPort(), (int)userCount);
    }
}

int HttpConn::GetFd() const
{
    return fd_;
}

struct sockaddr_in HttpConn::GetAddr() const
{
    return addr_;
}

const char *HttpConn::GetIP() const
{
    return inet_ntoa(addr_.sin_addr);
}

int HttpConn::GetPort() const
{
    return addr_.sin_port;
}

ssize_t HttpConn::read(int *saveErrno)
{
    ssize_t len = 0;
    ssize_t totalLen = 0;

    while (true)
    {
        len = readBuff_.ReadFd(fd_, saveErrno);
        if (len < 0)
        {
            if (*saveErrno == EAGAIN || *saveErrno == EWOULDBLOCK)
            {
                break;
            }
            return -1;
        }
        else if (len == 0)
        {
            break;
        }
        totalLen += len;
    }

    if (totalLen > 0)
    {
        LOG_DEBUG("Read %zd bytes from client[%d]", totalLen, fd_);
    }

    return totalLen;
}

ssize_t HttpConn::write(int *saveErrno)
{
    ssize_t len = -1;

    do
    {
        len = writev(fd_, iov_, iovCnt_);
        if (len <= 0)
        {
            *saveErrno = errno;
            break;
        }

        if (ToWriteBytes() == 0)
        {
            break;
        }
        else if (static_cast<size_t>(len) > iov_[0].iov_len)
        {
            iov_[1].iov_base = static_cast<uint8_t *>(iov_[1].iov_base) + (len - iov_[0].iov_len);
            iov_[1].iov_len -= (len - iov_[0].iov_len);

            if (iov_[0].iov_len)
            {
                writeBuff_.RetrieveAll();
                iov_[0].iov_len = 0;
            }
        }
        else
        {
            iov_[0].iov_base = static_cast<uint8_t *>(iov_[0].iov_base) + len;
            iov_[0].iov_len -= len;
            writeBuff_.Retrieve(len);
        }
    } while (isET || ToWriteBytes() > 10240);

    return len;
}

HttpConn::PROCESS_STATE HttpConn::process()
{
    // 先把 socket 中可读数据读进 readBuff_
    char buff[4096];
    for (;;)
    {
        ssize_t len = recv(fd_, buff, sizeof(buff), 0);
        if (len > 0)
        {
            readBuff_.Append(buff, len);
        }
        else if (len == 0)
        {
            // 客户端关闭连接
            LOG_INFO("Client [%d] closed connection", fd_);
            return ERROR;
        }
        else
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // 没有更多数据了
                break;
            }
            LOG_WARN("Recv error from client[%d]: %s", fd_, strerror(errno));
            return ERROR;
        }
    }

    // 解析HTTP请求（可能需要多次）
    ParseResult result = request_.parse(readBuff_, fd_);
    if (result.state == ParseResult::AGAIN)
    {
        // 数据还不够，等下一次epoll可读再调process()
        return AGAIN;
    }
    if (result.state == ParseResult::ERROR)
    {
        response_ = HttpResponse::CreateErrorResponse(400, "Bad Request", &request_);
        return FINISH;
    }

    // 走到这里说明完整请求已经解析完了，包括body_
    RouteRequest();

    // 在process()里根据response_统一序列化

    writeBuff_.Clear();
    response_.MakeResponse(writeBuff_);
    // 准备响应
    iov_[0].iov_base = const_cast<char *>(writeBuff_.Peek());
    iov_[0].iov_len = writeBuff_.ReadableBytes();
    iovCnt_ = 1;

    if (response_.FileLen() > 0 && response_.File())
    {
        iov_[1].iov_base = response_.File();
        iov_[1].iov_len = response_.FileLen();
        iovCnt_ = 2;
    }

    LOG_DEBUG("Response size: %zu, IOV count: %d, Total bytes: %zu",
              response_.FileLen(), iovCnt_, ToWriteBytes());

    // 处理完重置 request_ 以便下一次请求
    request_.Init();
    return FINISH;
}

void HttpConn::RouteRequest()
{
    const auto &method = request_.method();
    const auto &path = request_.path();

    LOG_INFO("Processing %s %s from client[%d]", method.c_str(), path.c_str(), fd_);

    // API路由处理
    if (path.find("/api/") == 0)
    {
        HandleApiRequest();
        return;
    }

    // 特定功能路由
    if (method == "GET")
    {
        if (path.find("/showlist") != string::npos)
        {
            HandleFileList();
        }
        else if (path.find("/logout") != string::npos)
        {
            HandleLogout();
        }
        else
        {
            HandleStaticFile();
        }
    }
    else if (method == "POST")

    {
        LOG_INFO("开始处理post路由");
        if (path.find("/login") == 0 || path.find("/register") == 0)
        {
            LOG_INFO("login路由开始处理");
            HandleUserAuth();
        }
        else if (path.find("/upload") == 0)
        {
            HandleUpload();
        }
        else
        {
            response_ = HttpResponse::CreateErrorResponse(404, "Not Found", &request_);
        }
    }
    else if (method == "DELETE" && path.find("/delete") == 0)
    {
        HandleDelete();
    }
    else
    {
        response_ = HttpResponse::CreateErrorResponse(405, "Method Not Allowed", &request_);
    }
}

void HttpConn::HandleApiRequest()
{
    const auto &path = request_.path();

    if (path == "/api/auth/status")
    {
        // 检查认证状态
        nlohmann::json response;
        response["authenticated"] = (request_.GetUserID() > 0);
        response["user_id"] = request_.GetUserID();
        response_ = HttpResponse::CreateJsonResponse(response, 200, &request_);
    }
    else
    {
        response_ = HttpResponse::CreateErrorResponse(404, "API endpoint not found", &request_);
    }
}

void HttpConn::HandleStaticFile()
{
    response_.InitFromRequest(request_, srcDir);
    response_.MakeResponse(writeBuff_);
}

void HttpConn::HandleUserAuth()
{
    bool isLogin = (request_.path().find("/login") != string::npos);
    LOG_INFO("Header:\n");
    for (auto &h : request_.header())
    {
        LOG_INFO("%s:%s",h.first.c_str(),h.second.c_str());
    }
    LOG_INFO("=== HandleUserAuth 调试信息 ===" );

    LOG_INFO("请求体内容:%s",request_.body().c_str());
    LOG_INFO("请求体长度:%zu",request_.body().size());


    const string &username = request_.GetPost("username").value_or("");
    const string &password = request_.GetPost("password").value_or("");


    LOG_INFO("User auth: %s, username: %s", isLogin ? "login" : "register", username.c_str());

    int userID = 0;
    string token;
    bool success = false;

    if (isLogin)
    {
        LOG_INFO("Debug:登录成功");
        success = authService_->Login(username, password, token, userID);
    }
    else
    {
        success = authService_->Register(username, password, userID);
    }

    if (success)
    {
        if (isLogin)
        {
            // 登录成功，设置cookie并重定向
            ForceLoginUser(userID);
            
            LOG_INFO("Debug:登录和验证成功" );
            // 方法1：使用纯重定向（推荐）
            response_ = HttpResponse::CreateRedirectResponse("/welcome", 302, &request_);
            // 添加Cookie设置
            response_.AddHeader("Set-Cookie",
                                "session_token=" + token +
                                    "; Path=/" +
                                    "; HttpOnly" +     // 防止XSS攻击
                                    "; Max-Age=3600" + // 1小时过期
                                    "; SameSite=Lax"); // CSRF保护

            response_.MakeResponse(writeBuff_);

            // 或者方法2：使用HTML重定向页面
            /*
            std::string html =
                "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta http-equiv='refresh' content='0;url=/welcome.html'>"
                "<title>登录成功</title></head>"
                "<body><p>登录成功，正在跳转…</p></body></html>";

            response_.SetHtmlResponse(html, 200);
            response_.AddHeader("Refresh", "0;url=/welcome.html");
            */
        }
        else
        {
            // 注册成功，重定向到登录页
            response_ = HttpResponse::CreateRedirectResponse("/login", 302, &request_);
            response_.MakeResponse(writeBuff_);

            // 或者显示成功消息后重定向
            /*
            std::string html =
                "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta http-equiv='refresh' content='2;url=/login.html'>"
                "<title>注册成功</title></head>"
                "<body><p>注册成功，2秒后跳转到登录页面…</p></body></html>";

            response_.SetHtmlResponse(html, 200);
            */
        }
    }
    else
    {
        // 认证失败，返回JSON错误响应
        nlohmann::json errorResponse;
        errorResponse["error"] = isLogin ? "登录失败" : "注册失败";
        errorResponse["message"] = "用户名或密码错误";

        // 检查请求的Accept头，决定返回HTML还是JSON
        auto acceptHeader = request_.GetHeader("Accept");
        if (acceptHeader && acceptHeader->find("application/json") != string::npos)
        {
            response_ = HttpResponse::CreateJsonResponse(errorResponse, 401, &request_);
        }
        else
        {
            // 认证失败，返回HTML错误页面
            std::string html =
                "<!doctype html><html><head><meta charset='utf-8'>"
                "<title>认证失败</title></head>"
                "<body><h1>认证失败</h1><p>用户名或密码错误</p>"
                "<p><a href='" +
                (isLogin ? std::string("/login.html") : std::string("/register.html")) + "'>返回</a></p></body></html>";

            response_.SetHtmlResponse(html, 401);
        }
    }
}

bool HttpConn::ExtractFileContentFromBody(const std::string &contentType,
                                          const std::string &body,
                                          UploadedFile &file)
{
    // 简单的实现 - 需要根据实际的multipart格式来完善
    size_t filename_pos = body.find("filename=\"");
    if (filename_pos == std::string::npos)
    {
        return false;
    }

    // 查找文件内容的开始位置（通常是两个CRLF之后）
    size_t content_start = body.find("\r\n\r\n", filename_pos);
    if (content_start == std::string::npos)
    {
        return false;
    }
    content_start += 4; // 跳过 "\r\n\r\n"

    // 查找文件内容的结束位置（boundary之前）
    size_t boundary_pos = body.find("\r\n--", content_start);
    if (boundary_pos == std::string::npos)
    {
        boundary_pos = body.length();
    }

    // 提取文件内容
    file.content = body.substr(content_start, boundary_pos - content_start);

    LOG_INFO("提取的文件内容大小: %zu 字节", file.content.size());

    return true;
}

void HttpConn::HandleUpload()
{
    if (!ExtractLoginFromCookie())
    {
        nlohmann::json errorResponse{
            {"error", "未登录"},
            {"message", "请先登录后再上传文件"}};
        response_ = HttpResponse::CreateJsonResponse(errorResponse, 403, &request_);
        return;
    }

    // 安全获取 Content-Type
    auto ctOpt = request_.GetHeader("Content-Type");
    if (!ctOpt)
    {
        nlohmann::json errorResponse{{"error", "缺少 Content-Type"}};
        response_ = HttpResponse::CreateJsonResponse(errorResponse, 400, &request_);
        return;
    }
    const std::string &contentType = *ctOpt;

    // 检查是否是 multipart/form-data
    if (contentType.find("multipart/form-data") == std::string::npos)
    {
        nlohmann::json errorResponse{{"error", "不支持的 Content-Type"}};
        response_ = HttpResponse::CreateJsonResponse(errorResponse, 400, &request_);
        return;
    }

    // 创建解析器和存储容器
    MultipartFormDataParser parser;
    std::unordered_map<std::string, std::string> formFields;

    // 添加调试信息
    LOG_INFO("=== HandleUpload 调试信息 ===");
    LOG_INFO("Content-Type: %s", contentType.c_str());
    LOG_INFO("请求体大小: %zu 字节", request_.body().size());

    // 正确调用 Parse 函数（参数顺序：contentType, body, post, fd）
    bool parseSuccess = parser.Parse(contentType, request_.body(), formFields, -1);

    if (!parseSuccess)
    {
        nlohmann::json errorResponse{{"error", "文件解析失败"}};
        response_ = HttpResponse::CreateJsonResponse(errorResponse, 400, &request_);
        return;
    }

    // 调试输出解析结果
    LOG_INFO("解析后的表单字段:");
    for (const auto &field : formFields)
    {
        LOG_INFO("  %s: %s", field.first.c_str(), field.second.c_str());
    }

    // 检查必要的字段是否存在
    if (formFields.find("filename") == formFields.end() || formFields["filename"].empty())
    {
        LOG_INFO("错误: 解析后文件名为空");
        nlohmann::json errorResponse{{"error", "未找到文件名"}};
        response_ = HttpResponse::CreateJsonResponse(errorResponse, 400, &request_);
        return;
    }

    // 问题：当前的 Parse 函数没有提取文件内容！
    // 需要修改 ParseMultipartFormData 来同时提取文件内容
    // 临时解决方案：重新解析或修改 ParseMultipartFormData

    // 创建 UploadedFile 对象（但缺少文件内容）
    UploadedFile file;
    file.filename = formFields["filename"];
    file.contentType = formFields["content_type"];

    // 需要从请求体中提取文件内容
    // 这里需要调用一个能提取文件内容的函数
    if (!ExtractFileContentFromBody(contentType, request_.body(), file))
    {
        nlohmann::json errorResponse{{"error", "无法提取文件内容"}};
        response_ = HttpResponse::CreateJsonResponse(errorResponse, 400, &request_);
        return;
    }
    LOG_INFO("文件内容大小: %d字节",file.content.size());


    // 现在可以保存文件
    if (UploadService::SaveUploadedFile(file, request_.GetUserID()))
    {
        nlohmann::json successResponse{
            {"status", "success"},
            {"filename", file.filename},
            {"message", "文件上传成功"}};

        // 同步返回其他表单字段
        for (auto &kv : formFields)
        {
            if (kv.first != "filename" && kv.first != "content_type")
            {
                successResponse[kv.first] = kv.second;
            }
        }

        response_ = HttpResponse::CreateJsonResponse(successResponse, 200, &request_);
    }
    else
    {
        nlohmann::json errorResponse{{"error", "文件保存失败"}};
        response_ = HttpResponse::CreateJsonResponse(errorResponse, 500, &request_);
    }
}

void HttpConn::HandleDelete()
{
    if (!ExtractLoginFromCookie())
    {
        nlohmann::json errorResponse;
        errorResponse["error"] = "未登录";
        response_ = HttpResponse::CreateJsonResponse(errorResponse, 403, &request_);
        return;
    }

    string filename = request_.path().substr(strlen("/delete/"));
    bool success = UploadService::DeleteFile(filename, request_.GetUserID());

    if (success)
    {
        nlohmann::json successResponse;
        successResponse["status"] = "success";
        successResponse["message"] = "文件删除成功";
        response_ = HttpResponse::CreateJsonResponse(successResponse, 200, &request_);
    }
    else
    {
        nlohmann::json errorResponse;
        errorResponse["error"] = "删除失败";
        errorResponse["message"] = "文件不存在或没有权限";
        response_ = HttpResponse::CreateJsonResponse(errorResponse, 404, &request_);
    }
}

void HttpConn::HandleFileList()
{
    LOG_INFO("打印文件列表" );
    if (!ExtractLoginFromCookie())
    {
        nlohmann::json errorResponse;
        errorResponse["error"] = "未登录";
        response_ = HttpResponse::CreateJsonResponse(errorResponse, 403, &request_);
        return;
    }

    nlohmann::json jsonResponse;
    vector<UploadedFileInfo> fileInfos = UploadService::QueryAllFiles(request_.GetUserID());

    for (const auto &file : fileInfos)
    {
        nlohmann::json fileJson;
        fileJson["filename"] = file.original_filename;
        fileJson["upload_time"] = file.upload_time;
        fileJson["user_id"] = file.uploader_id;
        fileJson["size"] = file.file_size;
        jsonResponse.push_back(fileJson);
    }

    response_ = HttpResponse::CreateJsonResponse(jsonResponse, 200, &request_);
    response_.MakeResponse(writeBuff_);
}

void HttpConn::HandleLogout()
{
    string cookie = request_.header().count("Cookie") ? request_.header().at("Cookie") : "";
    string token = ParseTokenFromCookie(cookie);

    if (!token.empty())
    {
        RedisSessionManager().DeleteSession(token);
    }

    // 清除cookie并重定向到登录页
    response_ = HttpResponse::CreateRedirectResponse("/login.html", 302, &request_);
    response_.RemoveCookie("token");
}

bool HttpConn::ExtractLoginFromCookie()
{
    auto it = request_.header().find("Cookie");
    if (it == request_.header().end())
    {
        LOG_INFO("No cookie found for client[%d]", fd_);
        return false;
    }

    string token = ParseTokenFromCookie(it->second);
    if (token.empty())
    {
        LOG_INFO("No token found in cookie for client[%d]", fd_);
        return false;
    }

    int userID = 0;
    if (authService_ && authService_->VerifyToken(token, userID))
    {
        request_.SetUserID(userID);
        LOG_INFO("User authenticated: userID=%d", userID);
        return true;
    }

    LOG_INFO("Invalid token for client[%d]", fd_);
    return false;
}

string HttpConn::ParseTokenFromCookie(const string &cookieStr)
{
    istringstream ss(cookieStr);
    string item;

    while (getline(ss, item, ';'))
    {
        size_t start = item.find_first_not_of(" ");
        if (start == string::npos)
            continue;

        item = item.substr(start);
        if (item.find("session_token=") == 0)
        {
            return item.substr(strlen("session_token="));
        }
    }

    return "";
}

bool HttpConn::IsStaticResource(const string &path)
{
    static const vector<string> exts = {
        ".js", ".css", ".html", ".htm", ".png", ".jpg", ".jpeg",
        ".gif", ".svg", ".woff", ".ttf", ".ico", ".txt", ".pdf"};

    for (const auto &ext : exts)
    {
        if (path.size() >= ext.size() &&
            path.compare(path.size() - ext.size(), ext.size(), ext) == 0)
        {
            return true;
        }
    }

    return false;
}

void HttpConn::ForceLoginUser(int userID)
{
    string token = RedisSessionManager().CreateSession(userID, 3600);
    request_.SetUserID(userID);

    // 设置cookie
    response_.SetCookie("token", token, "/", 3600, true, false);
    LOG_INFO("User %d logged in, token set", userID);
}