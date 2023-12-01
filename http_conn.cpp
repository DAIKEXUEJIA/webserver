#include "http_conn.h"

//所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
int http_conn::m_epollfd = -1;//静态成员变量初始化，初始值是-1
int http_conn::m_user_count = 0;


// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录,也就是resources这个文件夹，是网站根目录而不是服务器根目录
const char* doc_root = "/home/ubuntu/webserver/resources";


//向epoll添加需要监听的文件描述符
//EPOLLONESHOT事件，用于在多线程环境中，防止多个线程对同一个socket的并发访问
//主函数main的函数实现，不需要加上类作用域http_conn::

//设置文件描述符为非阻塞
void setNoneBlocking(int& fd){
    int old_flag = fcntl(fd,F_GETFL);//第二个参数是命令
    int new_flag = old_flag|O_NONBLOCK;//设置该fd为非阻塞
    fcntl(fd,F_SETFL,new_flag);
}

//向epoll中添加需要监听的文件描述符
void addfd(int epollfd,int fd,bool one_shot){
    epoll_event event;
    event.data.fd = fd;
    //默认LT,这里EPOLLRDHUP意思是如果断开连接,就触发挂起
    //有EPOLLRDHUP就不需要判断了，直接事件设置即可
    event.events = EPOLLIN|EPOLLRDHUP|EPOLLET;//设置epoll检测事件，如果是设置边沿触发那么这里是EPOLLIN|EPOLLET
    //设置oneshot事件的部分
    if(one_shot){
        event.events|EPOLLONESHOT;
    }
    //添加文件描述符信息
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);//传入设置好的事件即可
    //文件描述符设置为非阻塞
    setNoneBlocking(fd);
}

//从epoll中移除文件描述符
void removefd(int epollfd,int fd){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);//删除的时候events事件直接传入0即可
    close(fd);//关闭文件描述符 
}

//修改文件描述符需要重置oneshot事件，以确保下一次可读的时候，EPOLLIN事件可以被触发
//Linux系统中epoll事件（如EPOLLIN, EPOLLOUT, EPOLLONESHOT等）由整数常量定义,修改的时候传入Int即可
void modfd(int epollfd,int fd,int event){
    epoll_event events;
    events.data.fd = fd;
    events.events = event|EPOLLET|EPOLLONESHOT|EPOLLRDHUP;//重置oneshot事件，socket下一次可读的时候，EPOLLIN可以被触发
    //第三个参数是修改操作
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&events);
}

//类的成员函数需要加上类名称
//初始化连接的参数
void http_conn::init(int sockfd,sockaddr_in client_addr){
    printf("init函数执行\n");
    m_sockfd = sockfd;//http_conn类里面通过文件描述符进行读写操作
    m_address = client_addr;

    //设置端口复用
    int reuse = 1;//端口复用的值，1可复用，0不可复用
    setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

    //添加到epoll对象里面，监听新的客户端
    addfd(m_epollfd,sockfd,true);//单个客户端设置oneshot
    m_user_count++;//全局计数器++,也就是新的连接+1

    init();//初始化一下成员私有变量（状态信息）
}

//初始化类的私有成员变量
void http_conn::init(){
    m_check_state = CHECK_STATE_REQUESTLINE;//解析请求首行
    m_linger = false;       // 默认不保持链接  Connection : keep-alive保持连接
    m_checked_index = 0;//解析一个字符串的操作
    m_start_line = 0;
    m_read_index = 0;

    m_method= GET;
    m_url = 0;
    m_version = 0;
    bzero(m_read_buf,READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);

}


//关闭连接
void http_conn::close_conn(){
    //对已经是-1的描述符remove会引起不可预测的行为
    if(m_sockfd!=-1){//避免重复关闭，如果sockfd已经是-1，说明该连接文件描述符已经关闭了
        removefd(m_epollfd,m_sockfd);///从epoll实例里面移除这个sockfd
        m_sockfd = -1;
    }
    m_user_count--;//全局计数器--
}

//一次性读出所有数据
bool http_conn::read(){
    printf("读函数\n");
    if(m_read_index>=READ_BUFFER_SIZE){
        //已经超过缓冲区最大下标
        return false;
    }
    
    //死循环读取客户数据，直到无数据可读或者对方关闭连接
    int bytes_read = 0;
    while(true){
        //recv从任务类的成员变量m_sockfd进行读取
        //注意，第二个参数是从哪里开始读取！需要用读缓冲区数组+开始读取的位置！
        //数组的大小 = READ_BUFFER_SIZE-m_read_index，能不能用sizeof()？
        //bytes_read = recv(m_sockfd,m_read_buf+m_read_index,sizeof(m_read_buf),0);
        bytes_read = recv(m_sockfd,m_read_buf+m_read_index,READ_BUFFER_SIZE-m_read_index,0);
        //修改设置为非阻塞
        if(bytes_read==-1){
            //=-1就是出错了
            if(errno==EAGAIN||errno == EWOULDBLOCK){//非阻塞状态读取的话，会返回这两个错误，就是读完了
                break;//读完了的话跳出循环
            }
            //其他错误返回false
            perror("recv");
            return false;
        }
        else if(bytes_read==0){
            //=0就是对方关闭了连接
            return false;
        }
        else{
            //返回值>0的情况就是读到了数据
            m_read_index += bytes_read;//bytes_read是接收到的字节数，加上接收到的字节数就是下一次的起点
        }
    }
    printf("读取到了数据：%s\n",m_read_buf);
    return true;
}

//一次性写完数据
// 写HTTP响应
bool http_conn::write(){
    printf("一次性写完数据");
   int temp = 0;
    int bytes_have_send = 0;    // 已经发送的字节
    int bytes_to_send = m_write_idx;// 将要发送的字节 （m_write_idx）写缓冲区中待发送的字节数
    
    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }
    // 打印响应内容（用于调试）
    printf("HTTP Response:\n%s", m_write_buf);

    while(1) {
        // 分散写
        //writev 允许您同时写入多个非连续的内存缓冲区。
        //这意味着如果 HTTP 响应被分散在多个缓冲区中（例如，一个缓冲区用于头部，另一个用于主体），
        //可以使用单个 writev 调用将它们全部发送，而不需要多次调用 write。
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;
        if ( bytes_to_send <= bytes_have_send ) {
            // 发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap();
            if(m_linger) {
                init();
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return true;
            } else {
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return false;
            } 
        }
    }

}


//主状态机：解析请求
//解析读取HTTP请求，注意，这里的HTTP_CODE是http_conn类的一部分，是类里面定义的一个枚举类型
//public: enum HTTP_CODE { /* 定义枚举值 */ };
//因此这里的返回值是http_conn:HTTP_CODE
http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;//这是初始状态
    char* text = 0;//要获取的一行数据

    //根据主状态机的情况，处理数据
    while((m_check_state==CHECK_STATE_CONTENT)&&(line_status==LINE_OK)|| (line_status = parse_line())==LINE_OK){
        //解析到了一行完整的数据，或者解析到了请求体，也是一行完整的数据,就进入while循环
        
        //获取一行数据
        text = get_line();

        //修改m_srart_line
        m_start_line = m_checked_index;
        printf("got 1 http line:%s\n",text);

        //分析主状态机的状态
        switch (m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_requst_line(text);
                if(ret==BAD_REQUEST){
                    return BAD_REQUEST;//如果是语法错误就返回
                }
                break;
            }
            case CHECK_STATE_HEADER:
            {
                //解析请求头
                ret = parse_headers(text);
                if(ret==BAD_REQUEST){
                    return BAD_REQUEST;
                }
                else if(ret==GET_REQUEST){
                    return do_request();//说明解析完了请求头，现在需要解析具体的请求的信息
                }
            }
            case CHECK_STATE_CONTENT:
            {
                ret =parse_content(text);
                if(ret==GET_REQUEST){
                    return do_request();
                }
                //如果失败的话就写成LINE_OPEN
                line_status = LINE_OPEN;
                break;
            }
            
            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
    //解析HTTP请求，做业务逻辑
    printf("parse request,create response");
    //生成响应，写数据

}


//解析http请求行，获取请求方法，目标URL，HTTP版本
http_conn:: HTTP_CODE http_conn::parse_requst_line(char* text){
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t"); // 判断第二个参数中的字符哪个在text中最先出现
    if (! m_url) { 
        return BAD_REQUEST;
    }
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';    // 置位空字符，字符串结束符
    char* method = text;
    if ( strcasecmp(method, "GET") == 0 ) { // 忽略大小写比较
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }
    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_version = strpbrk( m_url, " \t" );
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    if (strcasecmp( m_version, "HTTP/1.1") != 0 ) {
        return BAD_REQUEST;
    }
    /**
     * http://110.40.135.12:10000/index.html
    */
    if (strncasecmp(m_url, "http://", 7) == 0 ) {   
        m_url += 7;//移动到192(110)之后
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        m_url = strchr( m_url, '/' );
    }
    if ( !m_url || m_url[0] != '/' ) {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER; // 改变主状态机的状态，检查状态变成检查头
    return NO_REQUEST;
}

// 解析HTTP请求的一个头部信息
http_conn:: HTTP_CODE http_conn::parse_headers(char* text){
    // 遇到空行，表示头部字段解析完毕
    if( text[0] == '\0' ) {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 ) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linger = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    } else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    } else {
        printf( "oop! unknow header %s\n", text );
    }
    return NO_REQUEST;

}

// 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content( char* text ) {
    if ( m_read_index >= ( m_content_length + m_checked_index ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//解析一行数据，依据是\r\n
http_conn:: LINE_STATUS http_conn::parse_line(){
    char temp;
    for(;m_check_state<m_read_index;m_checked_index++){
        //每一行最后都有\r\n
        temp = m_read_buf[m_checked_index];
        if(temp=='\r'){
            //read是下一次的索引
            if((m_checked_index+1)==m_read_index){
                return LINE_OPEN;//没有读取到完整的数据
            }
            else if(m_read_buf[m_checked_index+1]=='\n'){
                m_read_buf[m_checked_index++]='\0';
                m_read_buf[m_checked_index++]='\0';//把\r\n变成\0
                return LINE_OK;
            }
            return LINE_BAD;
        }else if(temp=='\n'){
            if((m_checked_index>1)&&(m_read_buf[m_checked_index-1]=='\r')){
                m_read_buf[m_checked_index-1]='\0';
                m_read_buf[m_checked_index]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }   
    }
    return LINE_OPEN;

}


// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn:: HTTP_CODE http_conn::do_request(){
    // "/home/nowcoder/webserver/resources"
    strcpy( m_real_file, doc_root );
    int len = strlen( doc_root );
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    if ( stat( m_real_file, &m_file_stat ) < 0 ) {//<0就说明没有这个文件
        return NO_RESOURCE;
    }

    // 判断访问权限，stat函数来判断
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );
    // 创建内存映射，把要发送的html网页的数据，映射到这个地址上。
    //这个地址的数据要发送给浏览器
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );//这里的close要和自己写的区分
    return FILE_REQUEST;
}

//用了内存映射，就需要用完了释放掉
// 对内存映射区执行munmap操作
void http_conn::unmap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}


/*
尽管底层的数据发送是通过 write 或 writev 函数完成的，
但在这之前，需要构建一个格式正确的 HTTP 响应。
这包括生成状态行、头部、响应主体等。这些 add_ 函数的目的是为了构建这样的响应。
*/

// 往写缓冲中写入待发送的数据
//往写缓冲中添加格式化的响应数据。它使用 vsnprintf 来格式化字符串，
//并将其追加到响应缓冲区。这对于创建响应主体或添加任何特定格式的数据非常有用。
bool http_conn::add_response( const char* format, ... ) {
    if( m_write_idx >= WRITE_BUFFER_SIZE ) {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) ) {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}

//add_status_line：这个函数专门用于添加 HTTP 响应的状态行，如 "HTTP/1.1 200 OK"。
//状态行是每个 HTTP 响应的必要部分，它指示了响应的 HTTP 版本、状态码和状态消息。
bool http_conn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

//add_headers：此函数用于添加 HTTP 响应头。它可能调用其他函数来添加特定的头部字段，
//如 Content-Length、Content-Type 和 Connection。响应头部是提供关于响应本身的元数据的关键部分，例如响应主体的长度和类型。
// bool http_conn::add_headers(int content_len) {
//     add_content_length(content_len);
//     add_content_type();
//     add_linger();
//     add_blank_line();
// }
// 修改 add_headers 函数以接收文件名参数
bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    // 现在根据文件名设置 Content-Type
    add_content_type();
    add_linger();
    add_blank_line();
    return true; // 假设这些函数都成功执行，返回 true
}

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

//持久连接尝试：在响应中设置 Connection: keep-alive
// bool http_conn::add_linger() {
//     return add_response("Connection: %s\r\n", "keep-alive");
// }


bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

// // 新的 add_content_type 函数，现在它接受文件名作为参数
// bool http_conn::add_content_type(const char* filename) {
//     // 提取文件扩展名
//     const char* ext = strrchr(filename, '.');
//     const char* type = "text/plain"; // 默认类型

//     if (ext) {
//         if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) {
//             type = "text/html";
//         } else if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) {
//             type = "image/jpeg";
//         } else if (strcmp(ext, ".png") == 0) {
//             type = "image/png";
//         }
//         // ... 这里可以添加更多的文件类型
//     }

//     return add_response("Content-Type: %s\r\n", type);
// }

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
//process_write工作在应用层，负责构建符合 HTTP 协议格式的响应消息。这包括生成状态行、响应头、以及响应正文（如 HTML 页面、错误消息等）
//write工作在网络层,负责将 process_write 函数构建的 HTTP 响应数据发送到网络中，传输给客户端。
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title );
            // 现在根据请求的文件设置 Content-Type
            //add_content_type(m_real_file);
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;

    
    return true;
}

//工作线程要执行的代码，由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process(){
    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
        //如果请求不完整
    if ( read_ret == NO_REQUEST ) {
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        return;
    }
    
    // 生成响应
    bool write_ret = process_write( read_ret );
    if ( !write_ret ) {
        close_conn();
    }
    modfd( m_epollfd, m_sockfd, EPOLLOUT);
}