#ifndef ECHO_SERVER_H
#define ECHO_SERVER_H

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

class EchoServer{ 
    public:
    EchoServer(int port);
    ~EchoServer();
    void start();


    private:
    int port;
    Socket socket;

    int acceptClient();
    void handleClient(int client_fd);
    void cleanupClient(int client_fd);

   
};

// 设计三个类：Socket、FileDescriptor、Connection



/*
class FileDescriptor{
    public:
    FileDescriptor();
    ~FileDescriptor();

    private:
    void acceptClient();
}

class Connection{
    public:
    Connection();
    ~Connection();

    private:
    void handleClient(int client_fd);
}*/


#endif