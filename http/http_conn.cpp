#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>
//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string,string>users;


//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    //获取文件描述符 fd 对应的文件状态标志
    int old_option = fcntl(fd,F_GETFL);
    //保留原有标志，新增「非阻塞」标志
    int new_option = old_option  | O_NONBLOCK;
    //将新的状态标志设置到 fd 上
    fcntl(fd,F_SETFL,new_option);
    // 返回原始的状态标志,允许后续恢复文件描述符的原始状态。
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd,int fd,bool one_shot,int TRIGMode)
{
    epoll_event event;
    event.data.fd=fd;

    if(1==TRIGMode)//TRIGMode控制ET/LT模式
    {
        //EPOLLRDHUP:快速检测客户端断连，避免服务端持续阻塞 / 空轮询
        event.events=EPOLLIN | EPOLLET | EPOLLRDHUP;
    }
    else//LT
        event.events=EPOLLIN | EPOLLRDHUP;
    if(one_shot)//单次触发控制，防止事件重复触发
        //EPOLLONESHOT:让被监听的文件描述符（fd）只触发一次 epoll 事件
        event.events |= EPOLLONESHOT;

    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,%event);
    //设置非阻塞
    setnonblocking(fd);
}

//从内核时间表删除描述符,将文件描述符从监听数上摘下
void removefd(int epollfd,int fd)
{
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

//将事件重置为EPOLLONESHOT,动态更新 epoll 对 fd 的监听事件,ev可能取值为EPOLLIN 或者EPOLLOUT
void modfd(int epollfd,int fd,int ev,int TRIGMode)
{
    epoll_event event;
    event.data.fd=fd;

    if(1==TRIGMode)
        event.events=ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events=ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);

}

int http_conn::m_user_count=0;
int http_conn::m_epollfd= -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if(real_close && (m_scokfd != -1))
    {
        printf("close %d\n",m_sockfd);
        removefd(m_epollfd,m_sockfd);
        m_sockfd= -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd,const sockaddr_in &addr,char *root,int TRIGMode,
                    int close_log,string user,string passwd,string sqlname)
{
    m_sockfd=sockfd;
    m_address=addr;

    addfd(m_epollfd,sockfd,true,m_TRIGMode);
    m_use_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log=close_log;
    //C++ 字符串到 C 风格字符数组
    strcpy(sql_user,user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新连接的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql=NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;//请求行状态,主状态机
    m_linger = false;//是否保持连接
    m_method = GET;
    m_url=0;
    m_version=0;
    m_content_length = 0;
    m_host= 0;
    m_start_line= 0;//已经解析的字符串长度
    m_checked_idx=0;
    m_read_idx= 0;
    m_write_idx =0;
    cgi = 0;            // 是否启用的POST
    m_state = 0;        //读为0, 写为1
    timer_flag = 0;
    improv = 0;
    //对三个缓冲区 / 字符数组进行全零（空字符）初始化
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for(; m_checked_idx < m_read_idx;++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if(temp == '\r')//HTTP 协议中规定行结束符为\r\n
        {
            if((m_checked_idx + 1)== m_read_idx)//\r是缓冲区的最后一个字符,说明\n（换行符）还未被读取到缓冲区中（可能是数据还没接收完）
                return LINE_OPEN;
            else if(m_read_buf[m_checked_idx+1]=='\n')//HTTP 协议的行结束
            {
                //将\r\n转为两个\0，既截断了当前行，也避免了换行符干扰后续字符串处理
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if(temp=='\n')
        {
            if(m_checked_idx>1 && m_read_buf[m_checked_idx-1]=='\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    //循环完，代表读完
    return LINE_OPEN;
}



//循环读取客户数据，直到无数据可读或对方关闭连接
bool http_conn::read_once()
{
    if(m_read_idx>=READ_BUFFER_SIZE)
    {
        return false;//读取完毕buff
    }
    int bytes_read=0;

    //LT读取数据
    if(0==m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    else
    {
        while(true)
        {
            //从套接字接受数据，存储在m_read_buff缓冲区中,m_read_buf+m_read_idx:从缓冲区的未写入位置开始存储新数据    READ_BUFFER_SIZE-m_read_idx本次读取的最大字节数    0表示阻塞模式
            bytes_read=recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);

            if(bytes_read==-1)//读取失败
            {
                //非阻塞ET模式下，需要一次性将数据读取完毕,用于识别「当前无数据可读，但并非错误」的场景
                if(errno==EAGAIN || errno==EWOULDBLOCK)
                    break;//暂时无数据，跳出循环
                return false;
            }
            else if(bytes_read==0)//服务器关闭连接
            {
                return false;
            }
            //修改m_read_idx的读取字节数
            m_read_idx+=bytes_read;
        }
        return true;
    }
    
}
//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    //在str1中查找第一个属于str2中任意字符的位置,合法的请求行应为 请求方法 + 空格 + URL + 空格 + 版本号
    m_url = strpbrk(text," \t");//GET /index.html HTTP/1.1
    if(!m_url)
    {
        return BAD_REQUEST;
    }
    //把分隔符替换为字符串结束符 '\0'，同时指针后移，让 text 只保留「请求方法」
    *m_url++ = "\0";
    char *method=text;
    if(strcasecmp(method,"GET"==0))
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else    
        return BAD_REQUEST;
    //让 m_url 精准指向 URL 的有效起始位置
    m_url += strspn(m_url, " \t");
    //strpbrk 找到 URL 后的分隔符,为拆分「HTTP 版本号」做准备
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");//指向H
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    //比较 m_url 的前 7 个字节是否等于 "http://"
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        //提取出真正的服务器路径部分,原 m_url 指向 "http://localhost/index.html"，移动后指向 "localhost/index.html"
        m_url += 7;
        m_url = strchr(m_url, '/');//,查找第一个 / 字符,最终 m_url 指向 "/index.html"
    }
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/')//不为空且第一个字符为/
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;//请求行结束，该请求头了
    return NO_REQUEST;
}
//解析http请求的一个头部信息，CHECK_STATE_HEADER，
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if(text[0]=='\0')
    {
        if(m_content_length !=0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            
        }
    }
}


//各子线程通过process函数对任务进行处理，调用process_read函数和process_write函数分别完成报文解析与报文响应两个任务。
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();

    //NO_REQUEST，表示请求不完整，需要继续接收请求数据
    if(read_ret==NO_REQUEST)
    {
        //注册并监听事件
        modefd(m_epollfd,m_sockfd,EPOLLIN);
        return;
    }
    //调用process_write完成报文响应
    bool write_ret=process_write(read_ret);
    if (!write_ret)     
    {
        close_conn();
    }
    ////注册并监听写事件
    modfd(m_epollfd,m_sockfd,EPOLLOUT);

}