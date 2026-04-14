#include "TCPserver.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdexcept>
#include <cerrno>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <vector> 
#include <algorithm>
#include <sys/time.h>
#include <string>
#include <map>

#define BACKLOG 128

SocketListener::SocketListener(int port):listen_fd_(-1){    // 当initSocket失败, listen_fd_=-1
    initSocket(port);
}

SocketListener::~SocketListener(){
    close();
}

int SocketListener::getFd() const{
    return listen_fd_;
}

void SocketListener::initSocket(int port){
    // 设置套接字
    listen_fd_=socket(AF_INET,SOCK_STREAM,0);
   
    if(listen_fd_<0){
        throw std::runtime_error("Socket creation failed");
    }
    
    // 设置套接字选项
    int opt=1;
    if(setsockopt(listen_fd_,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt))<0){
        throw std::runtime_error("Setsocketopt failed");
    }

    // 绑定地址、端口
    sockaddr_in server_addr{};
    server_addr.sin_family=AF_INET;
    server_addr.sin_addr.s_addr=INADDR_ANY;
    server_addr.sin_port=htons(port);

    if(bind(listen_fd_,(sockaddr*)&server_addr,sizeof(server_addr))<0){
        throw std::runtime_error("Bind failed");
    }
    
    //监听
    if(listen(listen_fd_,BACKLOG)<0){
        throw std::runtime_error("Listen failed");
    }
    std::cout<<"Server listening on port"<<port<<std::endl;

}

int SocketListener::accept(){
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);

    int client_fd=::accept(listen_fd_,(sockaddr*)&client_addr,&client_len);

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

void SocketListener::close(){
     if(listen_fd_>=0){
        shutdown(listen_fd_, SHUT_WR);// 发送FIN
        ::close(listen_fd_);

        listen_fd_=-1;
    }
}

// SocketListener类没有无参构造函数，这里添加显示构造
SelectPoller::SelectPoller(int listen_fd):listen_fd_(listen_fd),max_fd(listen_fd){

    FD_ZERO(&all_fds);
    FD_SET(listen_fd,&all_fds);

}

void SelectPoller::addFd(int client_fd){
     if(client_fd>0){
        client_fds.push_back(client_fd);
        FD_SET(client_fd,&all_fds);   // 把新客户端加入被监听队伍

        if(client_fd>max_fd){
            max_fd=client_fd;   // client_fd是递增的, 可以用来重新设置最大值
        }
    }
}


bool SelectPoller::isReady(int client_fd)const{
    return FD_ISSET(client_fd,&read_fds);
}

void SelectPoller::wait(){
    read_fds=all_fds;

    timeval tv;
    tv.tv_sec=0;
    tv.tv_usec=1000;
    
    // 防止select阻塞导致P99飙升
    int activity=select(max_fd+1,&read_fds,NULL,NULL,&tv);

    if(activity<0){
        throw std::runtime_error(std::string("select:")+strerror(errno));
        }
        
}

void SelectPoller::removeFd(int client_fd){
    ::close(client_fd);
    FD_CLR(client_fd,&all_fds);
        
    auto it=std::find(client_fds.begin(),client_fds.end(),client_fd);

    if(it!=client_fds.end()){
    client_fds.erase(it);   // 将此fd从vector中删除
    }

    if(client_fd==max_fd){
        max_fd=listen_fd_;   //重置fd再遍历寻找最大值
        for(int fd:client_fds){
            if(fd>max_fd)max_fd=fd;
        }
    }
}

void SelectPoller::closeAllClients(){
    for(int fd:client_fds){
        ::close(fd);
        FD_CLR(fd,&all_fds);
    }
    client_fds.clear();
    }
 

void Buffer::appendData(const char*data,ssize_t len){
    recv_buffer.append(data,len);
}

bool Buffer::takeData(std::string& request,const std::string& delimeter){
    size_t pos=recv_buffer.find(delimeter);
        
    if(pos==std::string::npos){
        return false; // 如果没找到字符串就返回false, 调用层进行处理  
    }
    else{
        request=recv_buffer.substr(0,pos+4);
        recv_buffer.erase(0,pos+4);
        return true;
    }
}

Connection::Connection(int client_fd):client_fd(client_fd){}
Connection::Connection(){}    // 当map找不到key值时会利用此默认构造函数来创建

ConnectionManager::ConnectionManager(SelectPoller* poller):poller_(poller){}

Connection* ConnectionManager::getconn(int client_fd){

    auto it=connections_.find(client_fd);

    if(it!=connections_.end()){
        return &(it->second);
    }
    else{
        return nullptr;
    }
}

void ConnectionManager::add(int client_fd){
    connections_.emplace(client_fd,client_fd);
    poller_->addFd(client_fd);
}

void ConnectionManager::remove(int client_fd){
    connections_.erase(client_fd);
    poller_->removeFd(client_fd);
}
void Connection::setCloseCallback(CloseCallback close_cb,ConnectionManager* connmgr){
    close_cb_=close_cb;
    connmgr_=connmgr;
}

void onConnectionClose(int fd,ConnectionManager* connmgr){
    if (!connmgr) {
        return;
    }
    connmgr->remove(fd);
}

void Connection::setMessageCallback(MessageCallback handleMessage){
    handler=handleMessage;
}

void Connection::recv(int client_fd){
    char temp_buffer[4096];
    ssize_t bytes_read;
    
    bytes_read=::recv(client_fd,temp_buffer,sizeof(temp_buffer),0);
    if(bytes_read<=0){
        if(bytes_read==0){
            std::cout<<"Client disconnected"<<std::endl;
        }
        else{
            std::cerr<<"Receive error"<<std::endl;
        }
        // 清理资源并返回
        close_cb_(client_fd,connmgr_);
        return;
    }

    // 为进行职责分离, 将临时缓冲区的数据追加到永久缓冲区, 永久缓冲区负责处理
    if(this){
        this->recv_buffer.appendData(temp_buffer,bytes_read);
    }// 不用写else，因为如果没有conn什么都不用做

}

void Connection::send(int client_fd){
    std::string request;
    // 用while(1)会导致调度不均, 我们这里控制每次处理的请求量request_count为5个
    for(int request_count=0;request_count<5;request_count++){
         if (!this) {
            return;
        }

        if(!this->recv_buffer.takeData(request,"\r\n\r\n")){
            break;
        }
        // 依赖反转
        if (!handler) std::cout << "handler 为空!\n";
        
        std::string response=handler(request.c_str(), request.size());
        
        if(::send(client_fd, response.c_str(), response.length(), 0)<0){
            std::cerr<<" send error"<<std::endl;
        }
    }   
}


// EventLoop类的实现
EventLoop::EventLoop(int port):
listener(port),
poller(listener.getFd()),
connmgr(&poller)
{
    std::cout<<"the initialized TCP server on port"<<port<<std::endl;
}

EventLoop::~EventLoop(){}

void EventLoop::setMessageCallback(MessageCallback cb){
    user_handler=cb;
}

void EventLoop::start(){
    int listen_fd=listener.getFd();

    running_=true;

    while(running_){
    poller.wait();
    
    //只检查 listen_fd 是否就绪，避免 wait() 内部遍历 0~max_fd
    if(poller.isReady(listen_fd)){
        
        int new_fd=listener.accept();
        connmgr.add(new_fd); 

        Connection* conn=connmgr.getconn(new_fd);
        
        conn->setMessageCallback(user_handler);
        conn->setCloseCallback(onConnectionClose, &connmgr);
    }
    // client_fds作为vector类型已经在addFd()里面被push_back过了，这里直接用
    for(int fd:poller.client_fds){
          if (poller.isReady(fd)){
            Connection* conn=connmgr.getconn(fd);
            conn->recv(fd);
            conn->send(fd);
            }
        }

    }  
}

void EventLoop::stop(){
    running_=false;
}
