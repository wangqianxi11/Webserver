/*
 * @Author       : mark
 * @Date         : 2020-06-17
 * @copyleft Apache 2.0
 */

#include "webserver.h"

using namespace std;

// WebServer构造函数
WebServer::WebServer(
    int port, int trigMode, int timeoutMS, bool OptLinger,                                                                                 // http连接端口号、触发模式、超时时间、是否优雅关闭
    int sqlPort, const char *sqlUser, const char *sqlPwd,                                                                                  // 数据库端口、数据库用户名、数据库密码
    const char *dbName, int connPoolNum, int threadNum,                                                                                    // 数据库名称、数据库连接池数目、线程池数目
    bool openLog, int logLevel, int logQueSize) :                                                                                          // 是否打开日志、日志级别、日志队列大小
                                                  port_(port), openLinger_(OptLinger), timeoutMS_(timeoutMS), isClose_(false),             // 参数赋给成员变量
                                                  timer_(new HeapTimer()), threadpool_(new ThreadPool(threadNum)), epoller_(new Epoller()) // 创建定时器、线程池、epoller实例
{
    srcDir_ = getcwd(nullptr, 256);
    strncat(srcDir_, "/resources", 16); // 设置http静态资源的目录
    HttpConn::userCount = 0;
    HttpConn::srcDir = srcDir_;
    SqlConnPool::Instance()->Init("localhost", sqlPort, sqlUser, sqlPwd, dbName, connPoolNum); // 初始化单例模式的数据库连接池

    // 建立redis连接池
    try {
        auto redisClient = std::make_shared<sw::redis::Redis>("tcp://127.0.0.1:6379");
        UploadService::InitRedis(redisClient);
        RedisSessionManager::InitRedis(redisClient);
    } catch (const std::exception &ex) {
        LOG_ERROR("Redis 初始化失败: %s", ex.what());
        isClose_ = true;
    }
    InitEventMode_(trigMode); // 初始化连接和监听的事件模式(LT/ET)
    if (!InitSocket_())
    {
        isClose_ = true;
    } // 套接字初始化

    if (openLog)
    {
        // 初始化日志系统
        Log::Instance()->init(logLevel, "./log", ".log", logQueSize);
        if (isClose_)
        {
            LOG_ERROR("========== Server init error!==========");
        }
        else
        {
            LOG_INFO("========== Server init ==========");
            LOG_INFO("Port:%d, OpenLinger: %s", port_, OptLinger ? "true" : "false");
            LOG_INFO("Listen Mode: %s, OpenConn Mode: %s",
                     (listenEvent_ & EPOLLET ? "ET" : "LT"),
                     (connEvent_ & EPOLLET ? "ET" : "LT"));
            LOG_INFO("LogSys level: %d", logLevel);
            LOG_INFO("srcDir: %s", HttpConn::srcDir);
            LOG_INFO("SqlConnPool num: %d, ThreadPool num: %d", connPoolNum, threadNum);
        }
    }
}

// WebServer的析构函数
WebServer::~WebServer()
{
    close(listenFd_); // 关闭监听socket
    isClose_ = true;  // 连接标志位设为true，表示关闭连接
    free(srcDir_);
    SqlConnPool::Instance()->ClosePool(); // 关闭数据库连接池
}

// 初始化WebServer事件触发模式
void WebServer::InitEventMode_(int trigMode)
{
    listenEvent_ = EPOLLRDHUP;              // 监听事件，默认设置，用于检测对端关闭连接
    connEvent_ = EPOLLONESHOT | EPOLLRDHUP; // 连接事件：保证一个socket连接在任意时刻只被一个线程处理|检测对端关闭
    switch (trigMode)
    {
    case 0: // 默认的LT模式
        break;
    case 1:
        connEvent_ |= EPOLLET;
        break;
    case 2:
        listenEvent_ |= EPOLLET;
        break;
    case 3:
        listenEvent_ |= EPOLLET;
        connEvent_ |= EPOLLET;
        break;
    default:
        listenEvent_ |= EPOLLET;
        connEvent_ |= EPOLLET;
        break;
    }
    HttpConn::isET = (connEvent_ & EPOLLET); // 根据连接事件是否有EPOLLET，设置http连接是否使用ET模式
}

void WebServer::Start()
{
    int timeMS = -1; /* epoll wait timeout == -1 无事件将阻塞 */
    if (!isClose_)
    {
        LOG_INFO("========== Server start ==========");
    }
    while (!isClose_)
    { // 典型的reactor模式，主循环直到isClose_为true结束
        if (timeoutMS_ > 0)
        {
            timeMS = timer_->GetNextTick(); // 计算下一次定时任务时长
        }
        int eventCnt = epoller_->Wait(timeMS); // 等待I/O事件发生，返回事件数量，epoll_wait函数
        for (int i = 0; i < eventCnt; i++)
        {
            /* 处理事件 */
            int fd = epoller_->GetEventFd(i);         // 获取事件描述符
            uint32_t events = epoller_->GetEvents(i); // 获取事件
            if (fd == listenFd_)
            {
                DealListen_();
            }
            else if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                CloseConn_(&users_[fd]);
            }
            else if (events & EPOLLIN)
            {
                DealRead_(&users_[fd]); // 客户端发来请求，读取并解析
            }
            else if (events & EPOLLOUT)
            {

                DealWrite_(&users_[fd]); // 服务端发送响应，
            }
            else
            {
                LOG_ERROR("Unexpected event");
            }
        }
    }
}

void WebServer::SendError_(int fd, const char *info)
{
    int ret = send(fd, info, strlen(info), 0);
    if (ret < 0)
    {
        LOG_WARN("send error to client[%d] error!", fd);
    }
    close(fd);
}

void WebServer::CloseConn_(HttpConn *client)
{

    LOG_INFO("Client[%d] quit!", client->GetFd());
    epoller_->DelFd(client->GetFd());
    client->Close();
}

void WebServer::AddClient_(int fd, sockaddr_in addr)
{
    /*
    @description:
        初始化用户状态，
        设置超时关闭
        注册epoll中监听读事件
        设置非阻塞
        写日志
    */

    users_[fd].init(fd, addr);
    if (timeoutMS_ > 0)
    {
        timer_->add(fd, timeoutMS_, std::bind(&WebServer::CloseConn_, this, &users_[fd]));
    }
    epoller_->AddFd(fd, EPOLLIN | connEvent_);
    SetFdNonblock(fd);
    LOG_INFO("Client[%d] in!", users_[fd].GetFd());
}

void WebServer::DealListen_()
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    while (true)
    {
        int fd = accept(listenFd_, (struct sockaddr *)&addr, &len);
        if (fd < 0)
        {
            int tmpErr = errno;
            if (tmpErr == EAGAIN || tmpErr == EWOULDBLOCK)
            {
                // 所有连接已 accept 完毕（ET 模式下必须循环）
                break;
            }
            else if (tmpErr == EMFILE || tmpErr == ENFILE)
            {
                // 文件描述符耗尽，无法再创建 socket
                LOG_ERROR("accept error: too many open files (errno=%d)", tmpErr);
            }
            else
            {
                // 其他错误，记录日志
                LOG_ERROR("accept error (errno=%d): %s", tmpErr, strerror(tmpErr));
            }
            break;
        }

        // 超出最大连接数
        if (HttpConn::userCount >= MAX_FD)
        {
            SendError_(fd, "Server busy!");
            LOG_WARN("Clients are full! Reject client[%d]", fd);
            close(fd);
            continue;
        }

        // 接受成功，注册客户端 socket
        AddClient_(fd, addr);
    }
}

// 处理读事件
void WebServer::DealRead_(HttpConn *client)
{
    ExtentTime_(client);
    // 提交读任务到线程池，使用bind将Onread_方法和当前对象和客户端连接绑定
    // 交给线程池异步处理，避免主线程阻塞
    threadpool_->AddTask(std::bind(&WebServer::OnRead_, this, client));
}

// 处理写事件
void WebServer::DealWrite_(HttpConn *client)
{
    ExtentTime_(client);
    // 提交写任务到线程池，使用bind将Onread_方法和当前对象和客户端连接绑定
    // 交给线程池异步处理，避免主线程阻塞
    threadpool_->AddTask(std::bind(&WebServer::OnWrite_, this, client));
}

void WebServer::ExtentTime_(HttpConn *client)
{
    // 延长客户端连接超时的方法，客户端有活动时刷新超时计时器
    if (timeoutMS_ > 0)
    {
        timer_->adjust(client->GetFd(), timeoutMS_);
    }
}

void WebServer::OnRead_(HttpConn *client)
{
    /*
    从客户端socket读取
    如果读取失败或连接断开，关闭连接
    读取成功就进一步处理OnProcess
    */

    int ret = -1;
    int readErrno = 0;
    ret = client->read(&readErrno);
    LOG_INFO("从客户端读取了%d字节", ret);
    if (ret <= 0 && readErrno != EAGAIN)
    {
        CloseConn_(client);
        return;
    }

    OnProcess(client);
}

void WebServer::OnProcess(HttpConn *client)
{
    HttpConn::PROCESS_STATE state = client->process();
    int fd = client->GetFd();

    switch (state)
    {
    case HttpConn::PROCESS_STATE::FINISH:
    case HttpConn::PROCESS_STATE::ERROR:
        epoller_->ModFd(fd, connEvent_ | EPOLLOUT); // 不论是正常响应还是400，都要写回
        break;
    case HttpConn::PROCESS_STATE::AGAIN:
        epoller_->ModFd(fd, connEvent_ | EPOLLIN); // 数据还不够，继续读
        break;
    }
}

void WebServer::OnWrite_(HttpConn *client)
{
    int ret = -1;
    int writeErrno = 0;
    ret = client->write(&writeErrno);
    if (client->ToWriteBytes() == 0)
    {
        /* 传输完成 */
        if (client->IsKeepAlive())
        {
            OnProcess(client);
            return;
        }
    }
    else if (ret < 0)
    {
        if (writeErrno == EAGAIN)
        {
            /* 继续传输 */
            epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
            return;
        }
    }
    CloseConn_(client);
}

/* Create listenFd */
bool WebServer::InitSocket_()
{
    int ret;
    struct sockaddr_in addr;
    // 端口号有效验证
    if (port_ > 65535 || port_ < 1024)
    {
        LOG_ERROR("Port:%d error!", port_);
        return false;
    }
    // 地址结构初始化
    addr.sin_family = AF_INET; // IPv4协议
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_); // 设置监听端口
    struct linger optLinger = {0};
    if (openLinger_)
    {
        /* 优雅关闭: 直到所剩数据发送完毕或超时 */
        optLinger.l_onoff = 1;
        optLinger.l_linger = 1;
    }

    listenFd_ = socket(AF_INET, SOCK_STREAM, 0); // 创建IPv4套接字
    if (listenFd_ < 0)
    {
        LOG_ERROR("Create socket error!", port_);
        return false;
    }

    // 设置套接字选项
    ret = setsockopt(listenFd_, SOL_SOCKET, SO_LINGER, &optLinger, sizeof(optLinger));
    if (ret < 0)
    {
        close(listenFd_);
        LOG_ERROR("Init linger error!", port_);
        return false;
    }

    int optval = 1;
    /* 端口复用 */
    /* 只有最后一个套接字会正常接收数据。 */
    ret = setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(int));
    if (ret == -1)
    {
        LOG_ERROR("set socket setsockopt error !");
        close(listenFd_);
        return false;
    }

    // 绑定
    ret = bind(listenFd_, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0)
    {
        LOG_ERROR("Bind Port:%d error!", port_);
        close(listenFd_);
        return false;
    }

    // 监听
    ret = listen(listenFd_, 1024);
    if (ret < 0)
    {
        LOG_ERROR("Listen port:%d error!", port_);
        close(listenFd_);
        return false;
    }
    // 将套接字注册到epoll实例中，
    ret = epoller_->AddFd(listenFd_, listenEvent_ | EPOLLIN);
    if (ret == 0)
    {
        LOG_ERROR("Add listen error!");
        close(listenFd_);
        return false;
    }
    // 设置非阻塞模式
    SetFdNonblock(listenFd_);
    LOG_INFO("Server port:%d", port_);
    return true;
}

int WebServer::SetFdNonblock(int fd)
{
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFD, 0) | O_NONBLOCK);
}
