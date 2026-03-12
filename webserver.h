#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"

const int MAX_FD = 65536;           //最大文件描述符
const int MAX_EVENT_NUMBER = 10000; //最大事件数
const int TIMESLOT = 5;             //最小超时单位


class WebServer
{
public:
    WebServer();                                                    //构造函数       
    ~WebServer();                                                   //析构函数
    //初始化 WebServer 核心配置
    void init(int port , string user, string passWord, string databaseName,
              int log_write , int opt_linger, int trigmode, int sql_num,
              int thread_num, int close_log, int actor_model);

    void thread_pool();                                             //创建/初始化线程池
                                                                    
    void sql_pool();                                                //初始化数据库连接池                                                         

    void log_write();                                               //配置日志写入策略

    void trig_mode();                                               //设置 epoll 触发模式

    void eventListen();                                             //创建监听套接字,开始监听

    void eventLoop();                                               //服务核心事件循环,持续从 epoll 实例获取就绪事件连接 / 读写 / 信号 / 超时），分发处理（如新连接、读写请求、定时器超时等）

    void timer(int connfd,struct sockaddr_in client_address);       //为新建立的客户端连接（connfd）创建定时器

    void adjust_timer(util_timer *timer);                           //调整定时器

    void deal_timer(util_timer *timer,int sockfd);                  //处理定时器超时         

    bool dealclientdata();                                          //处理新客户端连接
    
    bool dealwithsignal(bool& timeout,bool& stop_server);           //处理信号事件

    void dealwithread(int sockfd);                                  //处理套接字（sockfd）的读事件

    void dealwithwrite(int sockfd);                                 //处理套接字（sockfd）的写事件
public:
        //基础
        int m_port;
        //网站根目录路径
        char *m_root;
        //控制日志的写入方式 / 输出方式   0 同步 1 异步需要有最大长度
        int m_log_write;
        //控制是否开启日志
        int m_close_log;
        //Reactor 模型与Proactor/Actor 模型切换
        int m_actormodel;
        //存储管道（pipe） 
        int m_pipefd[2];
        int m_epollfd;
        //登录用户
        http_conn *users;

        //数据库相关
        connection_pool *m_connPool;
        string m_user;              //登陆数据库用户名
        string m_passWord;          //登陆数据库密码
        string m_databaseName;      //使用数据库名
        int m_sql_num;


        //线程池相关
        threadpool<http_conn> *m_pool;
        int m_thread_num;

        //epoll_event相关， 会将所有就绪的事件填充到这个数组中
        epoll_event events[MAX_EVENT_NUMBER];

        int m_listenfd;
        //延迟关闭
        int m_OPT_LINGER;
        //触发模式
        int m_TRIGMode;
        //监听套接字触发模式
        int m_LISTENTrigmode;
        //连接套接字触发模式
        int m_CONNTrigmode;


        //定时器相关
        client_data *users_timer;
        Utils utils;


};
#endif