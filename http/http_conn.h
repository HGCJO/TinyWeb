#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

class http_conn
{
public:
    static const int FILENAME_LEN = 200;//文件名称长度
    static const int READ_BUFFER_SIZE = 2048;//读写字符串长度
    static const int WRITE_BUFFER_SIZE = 1024;
    enum METHOD//该枚举（enum METHOD）用于标准化表示 HTTP 请求的各类方法，
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATCH
    };
    enum CHECK_STATE//枚举定义了HTTP 请求解析的状态机阶段
    {
        CHECK_STATE_REQUESTLINE = 0,//以下三个分别代表，请求行，请求头，请求体
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    enum HTTP_CODE/// 请求处理过程中不同的处理结果状态码
    {
        NO_REQUEST,                 //不完整的请求
        GET_REQUEST,                //合法的完整请求
        BAD_REQUEST,                //错误的请求
        NO_RESOURCE,                //资源不存在
        FORBIDDEN_REQUEST,          //禁止访问
        FILE_REQUEST,               //文件请求就绪
        INTERNAL_ERROR,             //服务器内部错误
        CLOSED_CONNECTION           //连接已关闭
    };
    enum LINE_STATUS//标识 HTTP 请求行 / 头的解析状态
    {
        LINE_OK = 0,//继续解析下一行
        LINE_BAD,//直接判定为非法请求
        LINE_OPEN//数据没有接受完整，继续接受
    };
public:
    http_conn() {}
    ~http_conn() {}
public:
    //初始化套接字地址，函数内部会调用私有方法init
    void init(int sockfd,const sockaddr_in_ &addr);
    //关闭http连接
    void close_conn(bool real_close=true);
    void process();
    //读取浏览器端发来的全部数据
    bool read_once();
    //响应报文写入函数
    bool write();
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    //同步线程初始化数据库读取表
    void initmysql_result();
    //CGI使用线程池初始化数据库表
    void initresultFile(connection_pool *connPool);

    int timer_flag;
    int improv;


private:
    void init();
    //从m_read_buff 读取，并处理报文
    HTTP_CODE process_read();
    //向m_write_buf写入响应报文数据
    bool process_write(HTTP_CODE ret);
    //主状态机解析报文中的请求行数据
    HTTP_CODE parse_request_line(char *text);
    //主状态机解析报文中的请求头数据
    HTTP_CODE parse_headers(char *text);
    //主状态机解析报文中的请求内容
    HTTP_CODE parse_content(char *text);
    //生成响应报文
    HTTP_CODE do_request();
    
    //m_start_line是已经解析的字符
    //get_line用于将指针向后偏移，指向未处理的字符
    char* get_line()
    {
        return m_read_buf + m_start_line;
    }
    //从状态机读取一行，分析是请求报文的哪一部分
    LINE_STATUS parse_line();
    /*
    process_write() {
    1. add_status_line(200, "OK");       // 构建状态行
    2. add_headers(content_length);      // 批量构建响应头（内部调用以下函数）
       - add_content_type();             // 设置Content-Type
       - add_content_length(length);     // 设置Content-Length
       - add_linger();                   // 设置Connection
       - add_blank_line();               // 添加空行分隔头和体
    3. add_content("Hello World");       // 构建响应体
    4. 发送响应后调用unmap();           // 释放文件映射
    }
    */
    void unmap();
    //根据响应报文格式，生成对应8个部分，以下函数均由do_request调用
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();
public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL *mysql;
    int m_state;//读为0, 写为1

private:
    int m_sockfd;                  // 与客户端通信的套接字文件描述符
    sockaddr_in m_address;         // 客户端的网络地址结构体，包含IP和端口等信息
    char m_read_buf[READ_BUFFER_SIZE]; // //存储读取的请求报文数据
    long m_read_idx;               //缓冲区中数据的最后一个字节的下一个位置
    long m_checked_idx;            //m_read_buf读取的位置m_checked_idx
    int m_start_line;              //m_read_buf中已经解析的字符个数
    char m_write_buf[WRITE_BUFFER_SIZE]; //存储发出的响应报文数据
    int m_write_idx;               //指示buffer中的长度
    
    CHECK_STATE m_check_state;     // HTTP请求解析的状态机状态（请求行/请求头/请求体）//主状态机的状态
    METHOD m_method;               // HTTP请求方法（GET/POST/HEAD等）
    
    char m_real_file[FILENAME_LEN]; // 存储要访问的本地文件的真实路径
    char *m_url;                   // 指向HTTP请求中的URL地址
    char *m_version;               // 指向HTTP协议版本（如HTTP/1.1）
    char *m_host;                  // 指向HTTP请求中的Host字段
    long m_content_length;         // HTTP请求体的长度
    bool m_linger;                 // 是否保持连接（HTTP的Connection: keep-alive标识）
    
    char *m_file_address;          // 内存映射地址指针，指向服务器本地文件通过 mmap 映射到进程虚拟内存的起始位置
    struct stat m_file_stat;       // 存储本地文件的状态信息（大小、权限、类型等）
    struct iovec m_iv[2];          // io向量机制iovec
    int m_iv_count;                
    int cgi;                       // 是否启用的POST（是否通过CGI处理POST请求）
    char *m_string;                // 存储请求头数据（POST请求的请求体数据）
    int bytes_to_send;             // 待发送的字节总数
    int bytes_have_send;           // 已经发送的字节数
    char *doc_root;                // 网站根目录路径

    map<string, string> m_users;   // 存储用户账号密码的映射表（用户名->密码）
    int m_TRIGMode;                // 触发模式（ET/LT，边缘触发/水平触发）
    int m_close_log;               // 是否关闭日志（0：开启，1：关闭）

    char sql_user[100];            // 数据库登录用户名
    char sql_passwd[100];          // 数据库登录密码
    char sql_name[100];            // 要连接的数据库名称
};

#endif