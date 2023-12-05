/*

在使用I/O多路复用时，accept 和 read 的行为确实有所不同：

accept：在I/O多路复用模型中，通常将监听套接字设置为非阻塞模式。这意味着如果没有新的连接请求，accept 会立即返回，而不是阻塞等待。

read：同样，read 操作在非阻塞模式下执行。如果没有数据可读，read 会立即返回，程序可以继续执行其他任务或检查其他套接字。

使用I/O多路复用的主要优势是提高了程序处理多个并发网络连接的能力，尤其是在高负载情况下，因为它避免了阻塞调用和为每个连接创建大量进程或线程的开销。这提高了资源使用效率和系统的整体性能。

*/

#include <stdio.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/epoll.h>

#define SERVERIP "127.0.0.1"
#define PORT 6789


int main()
{
    // 1. 创建socket（用于监听的套接字）
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) {
        perror("socket");
        exit(-1);
    }
    // 2. 创建服务端IP地址
    struct sockaddr_in server_addr;
    server_addr.sin_family = PF_INET;
    // 点分十进制转换为网络字节序
    inet_pton(AF_INET, SERVERIP, &server_addr.sin_addr.s_addr);
    // 服务端也可以绑定0.0.0.0即任意地址
    // server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    //绑定IP和端口
    int ret = bind(listenfd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret == -1) {
        perror("bind");
        exit(-1);
    }
    // 3. 设置监听
    ret = listen(listenfd, 8);
        if (ret == -1) {
        perror("listen");
        exit(-1);
    }
    
    // 4.调用epoll_create，创建一个epoll实例
    int epfd = epoll_create(100);
    // 5.将监听的文件描述符相关的检测信息，加入epoll实例的红黑树中
    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = listenfd;
    //epoll_ctrl添加文件描述符，并且定义要检测的事件，这里是检测读事件
    ret = epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &event);
    if (ret == -1) {
        perror("epoll_ctl");
        exit(-1);
    }
    // 这是epoll_create的传出参数，结构体数组，保存内核检测完成的fd就绪队列
    struct epoll_event events[1024];
    // while循环等待客户端连接
    while (1) {
        // 6.epoll_wait检测哪些fd里面有数据了，第二个参数是数组，接收检测后的数据（传出参数），-1表示阻塞，有数据才返回
        int num = epoll_wait(epfd, events, 1024, -1);
        if (num == -1) {
            perror("poll");
            exit(-1);
        } else if (num == 0) {
            // 这里其实没用，不阻塞才会返回0
            continue;
        } else {
            // 7.循环遍历epoll_wait的传出参数，也就是发生改变的文件描述符集合，集合大小就是epoll_wait的返回值！
            for (int i = 0; i < num; i++) {
                //结构体里面的.fd成员就是改变的文件描述符，记为curfd
                int curfd = events[i].data.fd;
                // 8.如果是监听的文件描述符变了，那就是有新的客户端到达了
                if (curfd == listenfd) {
                    //接收客户端信息，和之前的代码一样
                    struct sockaddr_in client_addr;
                    socklen_t client_addr_len = sizeof(client_addr);
                    int connfd = accept(listenfd, (struct sockaddr*)&client_addr, &client_addr_len);
                    if (connfd == -1) {
                        perror("accept");
                        exit(-1);
                    }
                    // 打印并输出客户端信息，IP组成至少16个字符（包含结束符）
                    char client_ip[16] = {0};
                    inet_ntop(AF_INET, &client_addr.sin_addr.s_addr, client_ip, sizeof(client_ip));
                    unsigned short client_port = ntohs(client_addr.sin_port);
                    printf("ip:%s, port:%d\n", client_ip, client_port);
                    
                    // 将新的客户端信息加入监听集合，也就是通过epoll_ctrl添加文件描述符，并且定义要检测的事件，这里是检测读事件，和前面一样
                    event.events = EPOLLIN;
                    event.data.fd = connfd;//但这里fd是不一样的，这里是connfd
                    epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, &event);
                } 
                else //9.如果改变的不是监听描述符，那就是已连接的客户端有数据到达了
                {
                    // 读数据，和之前代码一样，这里我们只检测读事件，这里如果要检测写的话还需要做一些位运算
                    if (events[i].events & EPOLLOUT) {
                        continue;
                    }
                    // 接收客户端的消息，直接read curfd即可
                    char recv_buf[1024] = {0};
                    ret = read(curfd, recv_buf, sizeof(recv_buf));
                    if (ret == -1) {//read有问题没读到
                        perror("read");
                        exit(-1);
                    } else if (ret > 0) {//打印收到的数据，并且往回发送
                        printf("recv server data : %s\n", recv_buf);
                        write(curfd, recv_buf, strlen(recv_buf));
                    } else {
                        //客户端断开连接了，就要从epoll的待检测集合里面删除，还是调用epoll_ctrl
                        printf("client closed...\n");
                        //从集合里删除
                        epoll_ctl(epfd, EPOLL_CTL_DEL, curfd, NULL);
                        close(curfd);//关闭文件描述符
                        //这里需要先删除再关闭，因为一旦文件描述符被关闭，它就不再有效，因此最好先从 epoll 集合中删除，然后再执行关闭操作。
                        break;
                    }
                }
            }
        }
    }
    close(listenfd);
    close(epfd);
    return 0;
}