#ifndef ECHO_SERVER_H
#define ECHO_SERVER_H

class EchoServer{ 

    public:
    EchoServer(int port);
    ~EchoServer();
    void start();
    void stop();

    private:

    int server_fd;
    int port;
    
    void setupSocket();
    int acceptClient();
    void handleClient(int client_fd);
    void cleanup(int client_fd);
   
};
#endif