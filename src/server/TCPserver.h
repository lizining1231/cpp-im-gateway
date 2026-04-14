#ifndef TCPSERVER_H
#define TCPSERVER_H

#include<sys/types.h>
#include<unistd.h>
#include<string>
#include<vector>
#include<map>
class Connection;  // 前置声明
class ConnectionManager;  // 因为有互相依赖

using MessageCallback=std::string (*)(char const* msg,size_t len); 
using CloseCallback=void (*)(int fd,ConnectionManager* connmgr);

class SocketListener{
    public:
    explicit SocketListener(int port);
    ~SocketListener();
    int getFd() const;
    int accept();
    void close();

    private:
    int listen_fd_;
    void initSocket(int port);

    SocketListener(const SocketListener&)=delete;// 禁止拷贝
    SocketListener& operator=(const SocketListener&)=delete;

};

class SelectPoller{
    public:
    explicit SelectPoller(int listen_fd);
    ~SelectPoller()=default;
    std::vector<int> client_fds;
    void addFd(int client_fd);
    void wait();
    bool isReady(int fd)const;
    void removeFd(int client_fd);
    void closeAllClients();
    
    private:
    int port;
    int listen_fd_;

    fd_set all_fds;
    fd_set read_fds;
    int max_fd;


};

class Buffer{
    public:
    void appendData(const char*data,ssize_t length);
    bool takeData(std::string& request,const std::string& delimeter);

    private:
    std::string recv_buffer;

};

class ConnectionManager{
    public:
    explicit ConnectionManager(SelectPoller* poller_);
    Connection* getconn(int client_fd);    
    void add(int client_fd);
    void remove(int client_fd);
    private:
    std::map<int,Connection>connections_;    // 一个fd对应一个连接
    SelectPoller* poller_;

};

class Connection{
    public:
    void recv(int client_fd);
    void send(int client_fd);
    Connection(int client_fd);
    Connection();    

    void setMessageCallback(MessageCallback cb);
    void setCloseCallback(CloseCallback close_cb,ConnectionManager* connmgr);    
    


    private:
    int client_fd;
    Buffer recv_buffer;
    ConnectionManager* connmgr_;
    CloseCallback close_cb_;    
    MessageCallback handler;
};

class EventLoop{
    public:
    EventLoop(int port);
    ~EventLoop();
    void start();
    void stop();
    void setMessageCallback(MessageCallback cb);
    
    private:
    SocketListener listener;
    SelectPoller poller;
    ConnectionManager connmgr;
    MessageCallback user_handler;
    bool running_;
};



#endif