#ifndef TCPSERVER_H
#define TCPSERVER_H

#include<sys/types.h>
#include<unistd.h>
#include<string>
#include<vector>
#include<map>


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

/*class EventLoop{
    public:
    void start();
    void stop();
    private:
    
};*/


class Buffer{
    public:
    void appendData(const char*data,ssize_t length);
    bool takeData(std::string& request,const std::string& delimeter);

    private:
    std::string recv_buffer;

};

class Connection{
    public:
    int client_fd;
    Buffer recv_buffer;
    Connection(int client_fd);
    Connection();
};

class TCPServer{ 
    public:
    TCPServer(int port);
    ~TCPServer();

    void eventLoop();
    
    using MessageCallback=std::string (*)(char const* msg,ssize_t len);
    void setMessageCallback(MessageCallback cb);

    private:
     
    std::map<int,Connection>connections;    // 一个fd对应一个连接

    void handleClientData(int client_fd);
    
    MessageCallback handler;

    SocketListener listener;
    SelectPoller poller;

};

#endif