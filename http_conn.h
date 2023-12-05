//这是头文件部分
#ifndef HTTP_CONN_H
#define HTTP_CONN_H
//不写在一起因为内容太多
//这个类里面需要定义线程池的工作函数process，因为线程池把任务分给子线程，也是子线程去执行process
#include<sys/epoll.h>///cpp里面定义epoll添加和删除fd的函数
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include"locker.h"
#include <features.h>
#include <bits/stdint-uintn.h>
#include <sys/socket.h>
#include <bits/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include<string.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <stdarg.h>


//任务类
class http_conn{
public:

    //设置成静态的话，所有http_conn类都会共享同一个m_epollfd变量！
    static int m_epollfd;//所有socket上面的事件都注册到同一个epoll对象上面，所以是静态的
    static int m_user_count;

    // 文件名的最大长度
    static const int FILENAME_LEN = 200;    

    //读缓冲区的大小，这是常量
    static const int READ_BUFFER_SIZE = 2048;
    //写缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 2048;

    //定义有限状态机需要的一些状态:

    // HTTP请求方法，这里只支持GET（要判断是GET请求还是别的请求）
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};
    /*
    enum METHOD: 定义了不同的 HTTP 请求方法。这里包括 GET、POST 等常见的 HTTP 方法。
    enum CHECK_STATE: 表示解析 HTTP 请求的主状态机的不同状态，如正在分析请求行、头部字段或请求体。
    enum HTTP_CODE: 表示处理 HTTP 请求可能产生的结果，例如请求不完整、语法错误、服务器内部错误等。
    enum LINE_STATUS: 表示从状态机读取行的状态，如读取到完整行、行出错、行数据不完整等。
    */
   
    /*
        解析客户端请求时，主状态机的状态(解析HTTP请求报文的三个部分)
        CHECK_STATE_REQUESTLINE:当前正在分析请求行
        CHECK_STATE_HEADER:当前正在分析头部字段
        CHECK_STATE_CONTENT:当前正在解析请求体
    */
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
    
    /*
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据
        GET_REQUEST         :   表示获得了一个完成的客户请求
        BAD_REQUEST         :   表示客户请求语法错误
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求,获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
    enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };
    
    // 从状态机的三种可能状态，即行的读取状态，分别表示
    // 1.读取到一个完整的行 2.行出错（语法错误） 3.行数据尚且不完整（还没检测完）
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };


public:
    http_conn(){}
    ~http_conn(){}

    void process();//处理客户端请求，我们用的是模拟proactor模式，数据交给工作线程的时候已经获取了。
    //因此线程池的工作就是解析http请求报文，解析完了之后看需要哪个资源，再拼接成响应信息。
    
    //初始化新接受的连接，是public的（可以自由调用）
    void init(int sockfd,sockaddr_in client_addr);
    //关闭events为错误事件的连接
    void close_conn();
    //非阻塞地读取
    bool read();
    //非阻塞地写
    bool write();

    //解析HTTP请求
    HTTP_CODE process_read();
    // 填充HTTP应答
    bool process_write( HTTP_CODE ret );    

    
    //下面这一组函数被process_read调用以分析HTTP请求
    //解析请求首行
    HTTP_CODE parse_requst_line(char* text);
    //解析请求头
    HTTP_CODE parse_headers(char* text);
    //解析请求体
    HTTP_CODE parse_content(char * text);
    //具体处理请求体 
    http_conn:: HTTP_CODE do_request();
    //解析一行数据，依据是\r\n
    LINE_STATUS parse_line();
    char* get_line(){
        return m_read_buf+m_start_line;//这是返回一个char*指针，方便读取一行数据
    }

private:
    //私有成员初始化，上面的init是连接的初始化
    void init();
    //通信套接字
    int m_sockfd;//该HTTP连接的socket套接字
    sockaddr_in m_address;//通信的客户端socket地址
    //该连接（任务类）的读缓冲区
    char m_read_buf[READ_BUFFER_SIZE];
    //读的时候需要定义其他解析，标识读缓冲区中读入的客户端数据最后一个字节的下一个位
    //标识下一次从哪里开始读
    int m_read_index;

    int m_checked_index;//正在读取的字符在读缓冲区中的位置，记录索引是不是\n
    int m_start_line;//当前正在解析的行的起始位置

    CHECK_STATE m_check_state;//主状态机当前所处的状态

    char* m_url;//请求目标文件的文件名
    char* m_version;//协议版本，只支持HTTP1.1
    METHOD m_method;//请求方法
    char* m_host;//主机名
    bool m_linger;//记录HTTP请求是否要保持连接
    int m_content_length; // HTTP请求的消息总长度


    char m_real_file[ FILENAME_LEN ];       // 客户请求的目标文件的完整路径，其内容等于 doc_root + m_url, doc_root是网站根目录

    

    char m_write_buf[ WRITE_BUFFER_SIZE ];  // 写缓冲区
    int m_write_idx;                        // 写缓冲区中待发送的字节数
    char* m_file_address;                   // 客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;                // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    struct iovec m_iv[2];                   // 我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量。
    int m_iv_count;

    int bytes_to_send;              // 将要发送的数据的字节数
    int bytes_have_send;            // 已经发送的字节数
     // 这一组函数被process_write调用以填充HTTP应答。
    void unmap();
    bool add_response( const char* format, ... );
    bool add_content( const char* content );
    bool add_content_type();
    bool add_status_line( int status, const char* title );
    bool add_headers(int content_len);
    bool add_content_length( int content_length );
    bool add_linger();
    bool add_blank_line();
    

};


#endif