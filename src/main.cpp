#include "server/echo_server.h"
#include<iostream>
#include <cstdlib>
#include <string>

int main(int argc,char* argv[]){
    int port=8080;

    if(argc>=2){
        port=std::atoi(argv[1]);

        if(port<=0||port>65535) {
        std::cerr << "错误：端口号必须在1-65535之间" << std::endl;
        return 1;
        } 
    }

    std::cout << "端口：" << port << std::endl;
    
    try{
        std::cout<<"Echo server started"<<std::endl;

        EchoServer server(port);
        server.start();

        std::cout<<"Echo server stopped"<<std::endl;
        return 0;

    }catch(const std::runtime_error&e){
        std::cerr<<"运行时错误："<<e.what()<<std::endl;
        return 1;

    }catch(const std::exception&e){
        std::cerr<<"标准异常："<<e.what()<<std::endl;
        return 1;

    }catch(...){
        std::cerr<<"未知异常"<<std::endl;
        return 1;
    }
    
}

