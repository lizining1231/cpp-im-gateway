#ifndef TCPSERVER_H
#define TCPSERVER_H

#include<sys/types.h>
#include<unistd.h>
#include<string>
#include<vector>
class Socket{
    public:
    explicit Socket(int port);
    ~Socket();
    int getServer_fd() const;
    

    private:
    int server_fd;
    void initSocket(int port);
    void cleanupServer();

    Socket(const Socket&)=delete;// 禁止拷贝
    Socket& operator=(const Socket&)=delete;
};

class TCPServer{ 
    public:
    TCPServer(int port);
    ~TCPServer();
    //void start();
    void eventLoop();

    private:
    int port;
    Socket socket;

    fd_set all_fds;
    fd_set read_fds;
    int max_fd;
    int server_fd;
    std::vector<int> client_fds;


    int acceptClient();
    void handleClientData(int client_fd);
    std::string handleMessage(const char* buffer, ssize_t bytes_read);
    void cleanupClient();

};


#endif