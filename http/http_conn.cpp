#include "http_conn.h"

#include <map>
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
void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);
    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

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

    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
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
    if(real_close && (m_sockfd != -1))
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
    if(strcasecmp(method,"GET")==0)
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

//解析http请求的一个头部信息，CHECK_STATE_HEADER
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    //是否是空行
    if(text[0]=='\0')
    {
        if(m_content_length !=0)//有请求体
        {
            m_check_state = CHECK_STATE_CONTENT;//第三阶段
            return NO_REQUEST;
        }
        return GET_REQUEST;//没有请求体
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {//解析 HTTP 请求头里的 Connection: 字段，若字段值为 keep-alive，则设置 m_linger = true
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)//如果原text是"Content-Length: 1024"，执行后text指向" 1024"
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);//将text指向的字符串（数字字符串）转换为长整型（long），赋值给成员变量m_content_length。
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");//原行是Host: www.example.com，执行后text指向" www.example.com"）
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;

}
//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if(m_read_idx >= (m_content_length + m_checked_idx))//请求体完整读取后，缓冲区应达到的总字节数
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;//以完整解析，可以执行后续业务逻辑
    }
    return NO_REQUEST;
}

//子线程读取
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status =LINE_OK;
    HTTP_CODE ret=NO_REQUEST;
    char *text =0;
    //持续解析 HTTP 请求的每一行数据，直到请求解析完成 / 出错，或当前行数据不完整。确保主状态机在请求体阶段，且从状态机已完成上一行解析，从而跳过无意义的行解析
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text =get_line();
        ////m_start_line是每一个数据行在m_read_buf中的起始位置
        //m_checked_idx表示从状态机在m_read_buf中读取的位置
        m_start_line = m_checked_idx;
        //主状态机的三种状态转移逻辑
        switch(m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                //解析请求行
                ret=parse_request_line(text);
                if (ret==BAD_REQUEST)   
                    return BAD_REQUEST;
                break;

            }
            case CHECK_STATE_HEADER:
            {
                //解析请求头
                ret=parse_headers(text);
                if(ret==BAD_REQUEST)
                    return BAD_REQUEST;
                //完整解析GET请求之后，跳转到报文响应函数,get没有请求体
                else if(ret == GET_REQUEST)
                {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                //解析消息体
                ret=parse_content(text);
                ////完整解析POST请求后，跳转到报文响应函数
                if(ret==GET_REQUEST)
                    return do_request();
                //解析完消息体即完成报文解析，避免再次进入循环，更新line_status
                line_status=LINE_OPEN;
                break;

            }
            default:
                return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

//对报文的内容进行回应
http_conn::HTTP_CODE http_conn::do_request()
{
    //把网站根目录路径（doc_root）复制
    strcpy(m_real_file,doc_root);
    //计算网站根目录路径 doc_root 的字符串长度
    int len = strlen(doc_root);
    //m_url = "/2?user=admin&pass=123"-->p指向/
    const char*p = strrchr(m_url,'/');

    //处理cgi
    if(cgi ==1 && (*(p+1) == '2' || *(p+1) == '3'))
    {
        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];//表示2或者其他数字代表不同的业务逻辑
        //动态分配一块 200 字节的内存，用于拼接处理后的 URL 路径
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");//初始化 m_url_real，先写入根路径分隔符 /。
        //拼接 URL 中从第三个字符开始的剩余部分到 m_url_real   拼接后 m_url_real = "/login.html"
        strcat(m_url_real, m_url + 2);
        //将拼接后的 URL 路径写入「真实文件路径」m_real_file
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        //释放内存
        free(m_url_real);


        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            //动态拼接一条 MySQL 插入语句:INSERT INTO user(username, passwd) VALUES('zhangsan', '123456')
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if(users.find(name) == users.end())
            {
                m_lock.lock();
                //执行数据库插入
                int res = mysql_query(mysql, sql_insert);
                //同步内存缓存
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();
                //注册成功,跳转到登录页面
                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else//存在用户名，注册失败
                strcpy(m_url, "/registerError.html");
        }
         //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }

    }
    ////如果请求资源为/0，表示跳转注册界面
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        //将网站目录和/register.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //如果请求资源为/1，表示跳转登录界面
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //POST请求，跳转到picture.html，即图片请求页面
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //POST请求，跳转到video.html，即视频请求页面
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //POST请求，跳转到fans.html，即关注页面
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //都不是则，直接将url与网站目录拼接，//这里的情况是welcome界面，请求服务器上的一个图片
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    //通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    //失败返回NO_RESOURCE状态，表示资源不存在,获取指定文件 /
    // 路径的文件状态信息，并将这些信息填充到传入的 struct stat 结构体中。
    if (stat(m_real_file, &m_file_stat) < 0)//失败返回-1
        return NO_RESOURCE;
    //判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;
    //判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;
    //以只读方式获取文件描述符，mmap 是将服务器本地文件映射到当前 HTTP 服务器进程的虚拟内存空间,减少了数据拷贝次数
    //浏览器最终拿到的是服务器从这块映射内存中读取数据
    int fd = open(m_real_file,O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    //表示请求文件存在，且可以访问
    return FILE_REQUEST;
}
//解除映射
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool http_conn::write()
{
    int temp =0;

    //若要发送的数据长度为0
    //表示响应报文为空，一般不会出现这种情况
    if(bytes_to_send == 0)
    {
        modfd(m_epollfd,m_sockfd,EPOLLIN,m_TRIGMode);
        //连接复用
        init();
        return;
    }

    while(1)
    {
        //将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        temp=writev(m_sockfd,m_iv,m_iv_count);
        if (temp < 0)
        {
            //非真正错误
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            //处理“真正的写错误”，取消内存映射
            unmap();
            return false;
        }
        //temp为发送的字节数
        bytes_have_send += temp;
        bytes_to_send -= temp;
        //
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            // 分支1：响应头已全部发送完毕，仅剩响应体待发送
            m_iv[0].iov_len = 0;
            //m_file_address内存映射地址指针，指向服务器本地文件通过 mmap 映射到进程虚拟内存的起始位置
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            // 分支2：响应头还没发完，需要调整响应头的发送起点和长度
            // 响应头未发送部分的起始地址 = 原响应头起始地址 + 已发送字节数
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }
        //全部发送结束
        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    
    }
}
//=========================================================
bool http_conn::add_response(const char *format, ...)
{
    //如果写入内容超出m_write_buf大小则报错
    if(m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    //定义可变参数列表
    va_list arg_list;
    //将变量arg_list初始化为传入参数，format：可变参数列表的 “前一个固定参数”（即最后一个确定的参数）
    va_start(arg_list,format);
    //作用是将格式化后的字符串写入指定内存区域，返回值是实际写入的字符数
    int len =vsnprintf(m_write_buf+m_write_idx,WRITE_BUFFER_SIZE-1- m_write_idx,format,arg_list);
    //如果写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        //释放可变参数列表的资源
        va_end(arg_list);
        return false;
    }
    //更新m_write_idx位置
    m_write_idx += len;
    va_end(arg_list);
    LOG_INFO("request:%s", m_write_buf);

    return true;
}


/*
    HTTP/1.1 404 Not Found\r\n
    Content-Type:text/html\r\n
    Content-Length:49\r\n
    Connection:close\r\n
    \r\n
    The requested file was not found on this server.\n
    */

//添加状态行
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
//添加消息报头，具体的添加文本长度、连接状态和空行
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
//添加Content-Length，表示响应报文的长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
//添加文本类型，这里是html
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
//添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
//添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
//添加文本content
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}



//服务器子线程调用process_write完成响应报文，随后注册epollout事件。服务器主线程检测写事件，并调用http_conn::write函数将响应报文发送给浏览器端。
bool http_conn::process_write(HTTP_CODE ret)
{
    switch(ret)
    {
        ////内部错误，500
        /*
        HTTP/1.1 500 Internal Error
        Content-Length: 60
        Connection: close
        */
        case INTERNAL_ERROR:
        {
            //状态行
            add_status_line(500, error_500_title);
            //消息报头
            add_headers(strlen(error_500_form));

            if (!add_content(error_500_form))
                return false;
            break;
        }
        //报文语法有误，404
        case BAD_REQUEST:
        {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form))
                return false;
            break;
        }
        //资源没有访问权限，403
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form))
                return false;
            break;
        }
        //文件存在，200
        case FILE_REQUEST:
        {
            add_status_line(200,ok_200_title);
            //如果请求的资源存在
            if(m_file_stat.st_size!=0)
            {
                add_headers(m_file_stat.st_size);
                if(m_file_stat.st_size != 0)
                {
                    add_headers(m_file_stat.st_size);
                    //iov_base：缓冲区起始地址；
                    //iov_len：缓冲区长度
                    //第一个缓冲区指向已组装好的响应行 + 响应头
                    m_iv[0].iov_base = m_write_buf;
                    m_iv[0].iov_len = m_write_idx;

                    //第二个缓冲区指向文件内容的内存地址
                    m_iv[1].iov_base = m_file_address;
                    m_iv[1].iov_len = m_file_stat.st_size;
                    m_iv_count = 2;
                    //计算需要发送的总字节数
                    bytes_to_send = m_write_idx + m_file_stat.st_size;
                    //表示响应报文组装成功,无需执行后续的“空文件”逻辑
                    return true;
                }
                else
                {
                    //处理文件大小为0
                    const char *ok_string = "<html><body></body></html>";
                    add_headers(strlen(ok_string));
                    if(!add_content(ok_string))
                        return false;
                }
            }
        }
        default:
            return false;
    }
    //除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}






//各子线程通过process函数对任务进行处理，调用process_read函数和process_write函数分别完成报文解析与报文响应两个任务。
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();

    //NO_REQUEST，表示请求不完整，需要继续接收请求数据
    if(read_ret==NO_REQUEST)
    {
        //注册并监听事件
        modfd(m_epollfd,m_sockfd,EPOLLIN);
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