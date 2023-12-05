#include <stdio.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

#define SERVERIP "127.0.0.1"
#define PORT 6789

//回调函数
void recycleChild(int arg) {
    // 写while是为了处理多个信号，多个子进程同时死了会发送多个信号，方便处理多个
    while (1) {
        //在回调函数里面调用waitpid，第一个参数是-1，回收所有子进程
        int ret = waitpid(-1, NULL, WNOHANG);
        if (ret == -1) {
            // 所有子进程都回收了，结束回收
            break;
        } else if (ret == 0) {
            // 返回值=0，表示还有子进程活着
            break;
        } else if(ret > 0) {
            // 某个子进程被回收了
            printf("子进程 %d 被回收了\n", ret);
        }
    }
}

int main()
{
    // 注册信号捕捉，这里是为了使用信号回收子进程的资源
    struct sigaction act;
    //使用第一个回调
    act.sa_flags = 0;
    //信号掩码清空
    sigemptyset(&act.sa_mask);//这里信号掩码清空是为了处理信号的时候不阻塞其他信号
    //这里是赋值回调函数
    act.sa_handler = recycleChild;
    //注册信号捕捉
    sigaction(SIGCHLD, &act, NULL);

    // 1. 创建socket（用于监听的套接字）
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) {
        perror("socket");
        exit(-1);
    }

    // 2. 绑定
    struct sockaddr_in server_addr;
    server_addr.sin_family = PF_INET;
    // 点分十进制转换为网络字节序
    inet_pton(AF_INET, SERVERIP, &server_addr.sin_addr.s_addr);
    // 服务端也可以绑定0.0.0.0即任意地址,这是一种通用做法
    // server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    int ret = bind(listenfd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret == -1) {
        perror("bind");
        exit(-1);
    }

    // 3. 监听
    ret = listen(listenfd, 8);//这里第二个参数写128也可以
        if (ret == -1) {
        perror("listen");
        exit(-1);
    }
    // 并发的不同之处：需要不断循环等待客户端连接
    while (1) {
        // 4. 接收客户端连接
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        //接收连接并判断accept的返回值
        int connfd = accept(listenfd, (struct sockaddr*)&client_addr, &client_addr_len);
        if (connfd == -1) {
            // 这里接收accept生成的文件描述符
            //用于处理信号捕捉导致的accept: Interrupted system call
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            exit(-1);
        }
        //每一个连接进来，创建一个子进程跟客户端通信
        pid_t pid = fork();
        //pid==0说明是子进程
        if (pid == 0) {
            // 子进程，出了这个if语句块，就不再是子进程的内容
            // 输出客户端信息，IP组成至少16个字符（包含结束符）
            char client_ip[16] = {0};
            //将客户端IP网络字节序转换成主机字节序，并保存到数组
            inet_ntop(AF_INET, &client_addr.sin_addr.s_addr, client_ip, sizeof(client_ip));
            //获取客户端通信端口
            unsigned short client_port = ntohs(client_addr.sin_port);
            printf("ip:%s, port:%d\n", client_ip, client_port);

            // 5. 子进程开始通信
            // 服务端先接收客户端信息，再向客户端发送数据
            // 接收客户端传来的数据，存在缓冲区里面
            char recv_buf[1024] = {0};
            while (1) {
                //读数据，并判断有没有接收成功
                ret = read(connfd, recv_buf, sizeof(recv_buf));
                //返回-1是读失败了
                if (ret == -1) {
                    perror("read");
                    exit(-1);
                    //如果ret>0就是读到了，读出来的话打印出来
                } else if (ret > 0) {
                    printf("recv client data : %s\n", recv_buf);
                } else {//返回值==0，说明客户端已经断开连接
                    // 表示客户端断开连接
                    printf("client closed...\n");
                    // read不到数据，需要退出循环，否则会出现read: Connection reset by peer错误
                    break;
                }
                // 发送数据，写一个字符串，把字符串发回去
                char *send_buf = "hello, i am server";
                // 粗心写成sizeof，那么就会导致遇到空格终止
                write(connfd, send_buf, strlen(send_buf));
            }
            // 关闭文件描述符，这是子进程的最后一步，关闭通信文件描述符，退出子进程
            close(connfd);
            exit(0);//退出当前子进程
        }
    }
	//while(1)结束了之后，监听的文件描述符也要关闭
    close(listenfd);
    return 0;
}