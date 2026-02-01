#include"echo_server.h"
#include<iostream>
#include<cstring>
#include<unistd.h>
#include<arpa/inet.h>
#include<sys/socket.h>
#include<stdexcept>
#include <cerrno>
#include <cstring>
#include<netinet/tcp.h>

#define BUFFER_SIZE 1024
#define BACKLOG 128

EchoServer::EchoServer(int port):server_fd(-1),port(port){
    int client_fd=-1;
    std::cout<<"the initialized echo server on port"<<port<<std::endl;
};

EchoServer::~EchoServer(){

    stop();
};


void EchoServer::start(){

    setupSocket();
    int client_fd=acceptClient();
    handleClient(client_fd);

    cleanup(client_fd);
    
}   


void EchoServer::stop(){

    cleanup(-1);
    std::cerr<<"Server stoped"<<std::endl;
}


void EchoServer::cleanup(int client_fd){

    if(client_fd>=0){
        shutdown(client_fd, SHUT_WR);// 发送FIN

        
        close(client_fd);
    
        client_fd=-1;
    }

    if(server_fd>=0){
        shutdown(server_fd, SHUT_WR);// 发送FIN


        close(server_fd);

        server_fd=-1;
    }
}


void EchoServer::setupSocket(){

    // 设置套接字
    server_fd=socket(AF_INET,SOCK_STREAM,0);
    if(server_fd<0){
        throw std::runtime_error("Socket creation failed");
    }

    // 设置套接字选项
    int opt=1;
    if(setsockopt(server_fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt))<0){
        throw std::runtime_error("Setsocketopt failed");
    }

    // 绑定地址、端口
    sockaddr_in server_addr{};
    server_addr.sin_family=AF_INET;
    server_addr.sin_addr.s_addr=INADDR_ANY;
    server_addr.sin_port=htons(port);

    if(bind(server_fd,(sockaddr*)&server_addr,sizeof(server_addr))<0){
        throw std::runtime_error("Bind failed");
    }

    //监听
    if(listen(server_fd,BACKLOG)<0){
        throw std::runtime_error("Listen failed");
    }

    std::cout<<"Server listening on port"<<port<<std::endl;

}


int EchoServer::acceptClient(){

    std::cout<<"Waiting for client connection..."<<std::endl;

    sockaddr_in client_addr{};
    socklen_t client_len=sizeof(client_addr);

    int client_fd;// 客户端的client_fd作为局部变量，每个连接独立管理，互不干扰
    
    client_fd=accept(server_fd,(sockaddr*)&client_addr,&client_len);// 阻塞等待客户端连接

    if(client_fd<0){
        throw std::runtime_error(
            std::string("Accept failed:")+strerror(errno)
        );
    }

    // 将二进制的IP地址转换成字符串
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET,&client_addr.sin_addr,client_ip,INET_ADDRSTRLEN);

    //将网络字节序转化为主机字节序

    std::cout<<client_ip<<":"<<ntohs(client_addr.sin_port)<<"(fd:"<<client_fd<<")"<<std::endl;

    return client_fd;
}


void EchoServer::handleClient(int client_fd){

    char buffer[BUFFER_SIZE];
    while(true){
        ssize_t bytes_read=recv(client_fd,buffer,BUFFER_SIZE-1,0);

        if(bytes_read<=0){
            if(bytes_read==0){
                std::cout<<"Client disconnected"<<std::endl;
              
            }
            else{
                std::cerr<<"Receive error"<<std::endl;
                
            }
            break;
        }

        buffer[bytes_read]='\0';// 读取到bytes_read字节，设此字节为‘\0’

        if(send(client_fd,buffer,bytes_read,0)<0){
            std::cerr<<"Send error"<<std::endl;
            break;
        }
        std::cout<<"recv num："<<bytes_read<<std::endl;
    }

}

