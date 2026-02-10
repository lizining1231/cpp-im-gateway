#include "server/echo_server.h"
#include<iostream>
// 等会儿可以在这里添加个宏定义or输入
int main(void){
    try{
        std::cout<<"Echo server started"<<std::endl;

        EchoServer server(8080);
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
