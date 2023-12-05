#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <assert.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536//最大的文件描述符个数，其实支持不了这么高并发，后面压力测试看最多支持多少
#define MAX_EVENT_NUMBER 10000 //epoll监听的最大事件数量

/*
extern 关键字用于声明一个变量或函数在另一个文件中已经定义，
即告诉编译器这个变量或函数的定义将在其他地方找到。这是一种在不同文件之间共享全局变量或函数的方法。
*/

//添加文件描述符到epoll中,在http_conn.cpp里面实现
extern void addfd(int epollfd,int fd,bool one_shot);//one_shot事件，后面再看

//从epoll中删除文件描述符
extern void removefd(int epollfd,int fd);

//epoll中修改文件描述符
extern void modfd(int epollfd,int fd,int event);

//信号处理函数
void addsig(int sig,void(*handler) (int) ){
    struct sigaction act;//这个结构体是作为注册信号的参数
    //将act结构体的内存初始化为0
    memset(&act,'\0',sizeof(act));
    act.sa_handler = handler;//设置函数指针的属性，这里handler理论上应该重写一个函数
    //设置信号掩码，SIGPIPE信号设置为全部阻塞,和SIGCHILD不同
    sigfillset(&act.sa_mask);
    //assert(sigaction(sig,&act,NULL)!=-1);//这里是为了安全性，先执行sigaction，再用assert进行判断
    sigaction(sig,&act,NULL);
    //返回值0才是成功 -1是失败了
}


int main(int argc,char* argv[]){//这两个参数是接收从命令行得到的运行程序名称和端口号

    if(argc<=1){
        printf("按照如下格式运行：%s port number\n",basename(argv[0]));//basename用于获取路径最后的文件名称。例如，如果传递 /usr/bin/myprogram 给 basename，它将返回 myprogram。
        exit(-1);//退出程序，使用 exit(-1) 表示如果没有提供必要的命令行参数，程序将立即终止并返回一个错误代码（-1）
        //return 1;//这里也是一样的，终止程序并且返回一个错误代码
    }

    int port = atoi(argv[1]);//取出输入的端口号

    //要提前做的操作：处理SIGPIPE信号
    addsig(SIGPIPE,SIG_IGN);//网络通信中，如果一方断开连接，会产生SIGPIPE信号，需要对这个信号进行处理
    //注意：SIG_IGN是可以作为信号处理函数传入的,SIG_IGN是忽略信号

    //创建线程池，初始化线程池,因为我们要做网络连接，所以用http的类
    threadPool<http_conn> *pool;  //使用的是模拟proactor模式，主线程监听有没有读的事件
    //用try-catch语句处理异常
    try{
        pool = new threadPool<http_conn>;//pool是线程池指针
    }
    catch(...){  //proactor模式就是主线程把数据一次性读完，读完了封装成任务类，交给子线程（线程池工作队列）
        return 1;
    }

    //创建一个数组用于保存所有的客户端信息（所有的http请求都封装在了任务类里面，其实最好是分开但是分开不易看懂）
    http_conn* users = new http_conn[MAX_FD];

    //网络通信部分
    int listenfd = socket(PF_INET,SOCK_STREAM,0);//流式协议使用TCP
    
    //设置端口复用，注意一定要在绑定端口之前设置复用
    int reuse = 1;//端口复用的值，1可复用，0不可复用
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    
    //绑定监听socket和地址
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;//这里含义是服务器可以绑定任意地址，0.0.0.0
    address.sin_port = htons(port);//这里的port是之前就取出来的，主机字节序host转换成网络字节序network
    //bind(listenfd,(struct sockaddr*)&address,sizeof(address));
    if (bind(listenfd, (struct sockaddr*)&address, sizeof(address)) == -1) {
        perror("bind error");
        //exit(1);
    }

    //监听
    listen(listenfd,5);//这里是未连接+已连接的队列，和最大值，一般指定5即可
    //创建epoll事件数组
    epoll_event events[MAX_EVENT_NUMBER];
    //创建一个epoll实例，返回值是操作这个实例的文件描述符
    int epollfd = epoll_create(100);//参数随便写，不是0就行

    //将监听的文件描述符添加到epoll对象，这里封装函数，因为后面还需要添加别的fd
    addfd(epollfd,listenfd,false);//listen描述符不需要添加oneshot，因为可以多个线程同时操作一个
    //给全局静态变量赋值
    http_conn::m_epollfd = epollfd;

    //循环检测有没有事件发生
    while(1){
        //events数组名称已经是地址了
        //这里设置成阻塞,epoll即使是ET模式，也只需要read/write操作设置为非阻塞，其他还是阻塞，没有事件的时候返回防止占用CPU资源
        int num = epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);
        //要确保调用错误不是因为被信号中断
        if(num==-1&&errno!=EINTR){
            //调用失败
            printf("epoll failure\n");
            break;
        }
        //epoll_wait调用成功，返回就绪的文件描述符个数，循环遍历
        printf("num is %d\n",num);
        for(int i=0;i<num;i++){
            //在传出参数events数组里面读取就绪的文件描述符
            int sockfd = events[i].data.fd;
            //分为两种情况，监听描述符变了（新客户端到达），和原有客户端fd变了（新数据到达）
            if(sockfd==listenfd){
                //新的客户端
                printf("new connect success\n");
                struct sockaddr_in client_address;
                socklen_t client_addr_length = sizeof(client_address);
                //accept接收连接并且返回和客户端通信的文件描述符
                int connfd = accept(listenfd,(struct sockaddr*)&client_address,&client_addr_length);
                if(connfd==-1){
                    printf("errno is %d\n",errno);
                    continue;//继续执行for循环
                }
                printf("accept success is %d\n",connfd);
                //如果此时客户端数量已经大于最大限度，不能增加新的客户端了
                if(http_conn::m_user_count>=MAX_FD){
                    //给客户端写入信息，告诉客户端服务器在忙，响应报文（还没写）
                    printf("full\n");
                    close(connfd);
                    continue;//继续遍历，因为可能还有已有客户端的就绪数据
                }
                //注意我们之前还创建了users数组，新的客户端放在users数组里面
                //文件描述符作为索引
                users[connfd].init(connfd,client_address);//初始化新客户端，epoll实例增加/全局计数器++逻辑都在init里
            }
            else if ((events[i].events & EPOLLRDHUP) || (events[i].events & EPOLLHUP) || (events[i].events & EPOLLERR)){
                //客户端异常断开/错误等事件，此时关闭fd，关闭连接
                printf("客户端异常断开\n");
                users[sockfd].close_conn();//这里传入的是sockfd，因为之前在数组里获取的就是sockfd
            }
            //最开始的问题出在这里！因为&运算优先级高于|运算优先级，&会被先计算，也就是（events[i].events& EPOLLRDHUP）|信号，这个基本上永远是true。
            // else if(events[i].events& EPOLLRDHUP|EPOLLHUP|EPOLLERR){//不是监听描述符，是已有的客户端描述符,开始判断事件
            //     //客户端异常断开/错误等事件，此时关闭fd，关闭连接
            //     printf("客户端异常断开\n");
            //     users[sockfd].close();//这里传入的是sockfd，因为之前在数组里获取的就是sockfd
            // }
            //判断有没有读事件发生
            else if(events[i].events&EPOLLIN){
                printf("read事件发生\n");
                //proactor模式，一次性读数据
                if(users[sockfd].read()){
                    //一次性把所有数据都读完，传入工作线程中
                    pool->append(users+sockfd);
                }
                else{
                    printf("读失败\n");
                    //读失败了
                    users[sockfd].close_conn();
                }
            }
            //判断有没有写事件发生
            else if(events[i].events&EPOLLOUT){
                printf("写事件发生\n");
                if(!users[sockfd].write()){ //proactor也是一次性写完所有数据
                    users[sockfd].close_conn();
                }
                //写事件只要判断是否成功即可
            }
        }


    }
    close(epollfd);
    close(listenfd);
    delete []users;
    delete pool;//释放对象，这里是通信程序结束才会执行
    return 0;


}