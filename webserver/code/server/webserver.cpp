#include"webserver.h"

WebServer::WebServer(int port, int trigMode, int timeoutMS, bool OptLinger, 
        int sqlPort, const char* sqlUser, const  char* sqlPwd, 
        const char* dbName, int connPoolNum, int threadNum,
        bool openLog, int logLevel, int logQueSize){
    srcDir_ = getcwd(nullptr, 256);
    assert(srcDir_);
    strcat(srcDir_,"/resources/");
    HttpConn::userCount = 0;
    HttpConn::srcDir = srcDir_;

    SqlConnPool::Instance()->Init("localhost", sqlPort, sqlUser, sqlPwd, dbName, connPoolNum);

    InitEventMode_(trigMode);
    if(!InitSocket_()) isClose_ = true;

    if(openLog){
        Log::Instance()->init(logLevel, "./log", ".log", logQueSize);
        if(isClose_) { LOG_ERROR("========== Server init error!=========="); }
        else {
            LOG_INFO("========== Server init ==========");
            LOG_INFO("Port:%d, OpenLinger: %s", port_, OptLinger? "true":"false");
            LOG_INFO("Listen Mode: %s, OpenConn Mode: %s",
                            (listenEvent_ & EPOLLET ? "ET": "LT"),
                            (connEvent_ & EPOLLET ? "ET": "LT"));
            LOG_INFO("LogSys level: %d", logLevel);
            LOG_INFO("srcDir: %s", HttpConn::srcDir);
            LOG_INFO("SqlConnPool num: %d, ThreadPool num: %d", connPoolNum, threadNum);
        }
    }
}

WebServer::~WebServer(){
    close(listenFd_);
    isClose_ = true;
    free(srcDir_);
    SqlConnPool::Instance()->ClosePool();
}

void WebServer::InitEventMode_(int trigMode){
    listenEvent_ = EPOLLRDHUP;
    connEvent_ = EPOLLONESHOT | EPOLLRDHUP;

    switch(trigMode){
        case 0:
            break;
        case 1:
            connEvent_ |= EPOLLET;
            break;
        case 2:
            listenEvent_ |= EPOLLET;
        case 3:
            listenEvent_ |= EPOLLET;
            connEvent_ |= EPOLLET;
            break;
        default:
            listenEvent_ |= EPOLLET;
            connEvent_ |= EPOLLET;
            break;
    }

    HttpConn::isET = (connEvent_ & EPOLLET);
}

void WebServer::Start(){
    int timeMs = -1;
    if(!isClose_){LOG_INFO("========== Server start ==========");}
    while(!isClose_){
        if(timeoutMS_ > 0){
            timeMs = timer_->GetNextTick();
        }
        int eventCnt = epoller_->Wait(timeMs);
        for(int i = 0; i < eventCnt; i++){
            int fd = epoller_->Wait(timeMs);
            uint32_t events = epoller_->GetEvents(i);
            if(fd == listenFd_) DealListen_();
            else if(events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                assert(users_.count(fd) > 0);
                CloseConn_(&users_[fd]);
            }
            else if(events & EPOLLIN){
                assert(users_.count(fd) > 0);
                DealRead_(&users_[fd]);
            }
            else if(events & EPOLLOUT){
                assert(users_.count(fd) > 0);
                DealWrite_(&users_[fd]);
            }
            else {
                LOG_ERROR("Unexpected event");
            }
        }
    }
}

void WebServer::SendError_(int fd, const char*info){
    assert(fd > 0);
    int ret = send(fd, info, strlen(info), 0);
    if(ret < 0){
        LOG_WARN("send error to client[%d] error!", fd);
    }
    close(fd);
}

void WebServer::CloseConn_(HttpConn* client){
    assert(client);
    LOG_INFO("Client[%d] quit!", client->GetFd());
    epoller_->DelFd(client->GetFd());
    client->Close();
}

void WebServer::AddClient_(int fd, sockaddr_in addr){
    assert(fd > 0);
    users_[fd].init(fd,addr);
    if(timeoutMS_ > 0){
        timer_->add(fd, timeoutMS_, std::bind(&WebServer::CloseConn_, this, &users_[fd]));
    }
    epoller_->AddFd(fd, EPOLLIN | connEvent_);
    SetFdNonblock(fd);
    LOG_INFO("Client[%d] in!", users_[fd].GetFd());

}

void WebServer::DealListen_(){
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    do{
        int fd = accept(listenFd_, (struct sockaddr*)&addr, &len);
        if(fd <= 0) return;
        else if(HttpConn::userCount >= MAX_FD){
            SendError_(fd, "Server busy!");
            LOG_WARN("Clients is full!");
            return;
        }
        AddClient_(fd, addr);
    }while(listenEvent_ & EPOLLET);
}

void WebServer::DealRead_(HttpConn* client){
    assert(client);
    ExtentTime_(client);
    threadpool_->AddTask(std::bind(&WebServer::OnRead_, this, client));
}

void WebServer::DealWrite_(HttpConn* client){
    assert(client);
    ExtentTime_(client);
    threadpool_->AddTask(std::bind(&WebServer::OnWrite_, this, client));
}

void WebServer::ExtentTime_(HttpConn* client){
    assert(client);
    if(timeoutMS_ > 0){
        timer_->adjust(client->GetFd(), timeoutMS_);
    }
}

void WebServer::OnRead_(HttpConn* client){
    assert(client);
    int ret = -1;
    int readErrno = 0;
    ret = client ->read(&readErrno);
    if(ret <= 0 && readErrno != EAGAIN){
        CloseConn_(client);
        return;
    }
    OnProcess(client);
}

void WebServer::OnProcess(HttpConn* client){
    // 首先调用process()进行逻辑处理
    if(client->process()) { // 根据返回的信息重新将fd置为EPOLLOUT（写）或EPOLLIN（读）
    //读完事件就跟内核说可以写了
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);    // 响应成功，修改监听事件为写,等待OnWrite_()发送
    } else {
    //写完事件就跟内核说可以读了
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLIN);
    }
}

void WebServer::OnWrite_(HttpConn* client){
    assert(client);
    int ret = -1;
    int writeErrno = 0;
    ret = client->write(&writeErrno);
    if(client->ToWriteBytes() == 0) {
        /* 传输完成 */
        if(client->IsKeepAlive()) {
            // OnProcess(client);
            epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLIN); // 回归换成监测读事件
            return;
        }
    }
    else if(ret < 0) {
        if(writeErrno == EAGAIN) {  // 缓冲区满了 
            /* 继续传输 */
            epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
            return;
        }
    }
    CloseConn_(client);
}

bool WebServer::InitSocket_(){
    int ret;
    struct sockaddr_in addr;
    if(port_ > 65535 || port_ < 1024){
        LOG_ERROR("Port:%d error!",  port_);
        return false;
    }
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);

    {
        struct linger optLinger = {0};
        if(openLinger_){
            /* 优雅关闭: 直到所剩数据发送完毕或超时 */
            optLinger.l_onoff = 1;
            optLinger.l_linger = 1;
        }

        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if(listenFd_ < 0){
            LOG_ERROR("Create socket error!", port_);
            return false;
        }

        ret = setsockopt(listenFd_, SOL_SOCKET, SO_LINGER, &optLinger, sizeof(optLinger));
        if(ret < 0){
            close(listenFd_);
            LOG_ERROR("Init linger error!", port_);
            return false;
        }

        int optval = 1;
        /* 端口复用 */
        /* 只有最后一个套接字会正常接收数据。 */
        ret = setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int));
        if(ret == -1) {
            LOG_ERROR("set socket setsockopt error !");
            close(listenFd_);
            return false;
        }

        ret = bind(listenFd_, (struct sockaddr*)&addr, sizeof(addr));
        if(ret < 0) {
            LOG_ERROR("Bind Port:%d error!", port_);
            close(listenFd_);
            return false;
        }

        ret = listen(listenFd_, 6);
        if(ret < 0) {
            LOG_ERROR("Listen port:%d error!", port_);
            close(listenFd_);
            return false;
        }

        ret = epoller_->AddFd(listenFd_,  listenEvent_ | EPOLLIN);  // 将监听套接字加入epoller
        if(ret == 0) {
            LOG_ERROR("Add listen error!");
            close(listenFd_);
            return false;
        }
        SetFdNonblock(listenFd_);   
        LOG_INFO("Server port:%d", port_);
        return true;
    }
}

int WebServer::SetFdNonblock(int fd){
    assert(fd > 0);
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFD, 0) | O_NONBLOCK);
}