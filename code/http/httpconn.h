#ifndef HTTP_CONN_H
#define HTTP_CONN_H

#include <sys/types.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <errno.h>
#include <fstream>
#include <memory>

#include "../log/log.h"
#include "../pool/sqlconnRAII.h"
#include "../buffer/buffer.h"
#include "../processing/UserService.h"
#include "../processing/AuthService.h"
#include "../processing/uploaded_file.h"
#include "../processing/uploadservice.h"
#include "../processing/RedisSessionManager .h"
#include "httprequest.h"
#include "httpresponse.h"

class HttpConn {
public:
    enum PROCESS_STATE {
        AGAIN,   // 数据还不够
        FINISH,  // 处理完成，准备写响应
        ERROR    // 请求格式错误
    };
    
    HttpConn();
    ~HttpConn();
    
    void init(int sockFd, const sockaddr_in& addr);
    ssize_t read(int* saveErrno);
    ssize_t write(int* saveErrno);
    void Close();
    
    int GetFd() const;
    int GetPort() const;
    const char* GetIP() const;
    sockaddr_in GetAddr() const;
    
    PROCESS_STATE process();
    
    int ToWriteBytes() { 
        return iov_[0].iov_len + iov_[1].iov_len; 
    }

    bool IsKeepAlive() const {
        return request_.IsKeepAlive();
    }
    bool ExtractFileContentFromBody(const std::string &contentType,
        const std::string &body,
        UploadedFile &file);
    static bool isET;
    static const char* srcDir;
    static std::atomic<int> userCount;
    
private:
    void RouteRequest();
    void HandleUserAuth();
    void HandleUpload();
    void HandleDelete();
    void HandleFileList();
    void HandleLogout();
    void HandleStaticFile();
    void HandleApiRequest();
    
    void ForceLoginUser(int userID);
    bool ExtractLoginFromCookie();
    std::string ParseTokenFromCookie(const std::string& cookieStr);
    bool IsStaticResource(const std::string& path);
    
    
    int fd_;
    struct sockaddr_in addr_;
    bool isClose_;
    
    int iovCnt_;
    struct iovec iov_[2];
    
    Buffer readBuff_;
    Buffer writeBuff_;

    HttpRequest request_;
    HttpResponse response_;

    std::shared_ptr<sw::redis::Redis> redis_;
    std::unique_ptr<AuthService> authService_;
};

#endif //HTTP_CONN_H