#ifndef LST_TIMER
#define LST_TIMER

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

#include <time.h>
#include "../log/log.h"

class util_timer;
//封装客户端连接的核心信息
struct client_data
{
    sockaddr_in address;//// 客户端的网络地址信息
    int sockfd;         // 客户端连接的套接字文件描述符
    util_timer *timer;  // 指向该连接对应的超时定时器指针
};

//双向链表的结点类型
class util_timer
{
public:
    //初始化定时器对象的前驱（prev）和后继（next）指针为 NULL
    util_timer() : prev(NULL),next(NULL) {}

public:
    //用于判断定时器是否到期
    time_t expire;                  
    //cb_func:定时器到期时触发的回调函数指针,参数是 client_data 指针
    void (* cb_func)(client_data *);
    //指向该定时器关联的客户端连接数据。
    client_data *user_data;
    //双向链表的前驱 / 后继指针
    util_timer *prev;
    util_timer *next;

}
//双向链表管理类
class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();
    //将新定时器按「到期时间（expire）」插入到链表的合适位置
    void add_timer(util_timer *timer);
    //调整该定时器在链表中的位置，保证链表仍按到期时间有序
    void adjust_timer(util_timer *timer);
    //从双向链表中移除指定的定时器结点

    void del_timer(util_timer *timer);
    //检查链表中是否有定时器到期
    void tick();

private:
    //实现从指定节点（lst_head）开始的有序插入
    void add_timer(util_timer *timer,util_timer *lst_head);

    util_timer *head;
    util_timer *tail;
};
//网络编程通用工具类,涵盖 IO 管理、信号处理、定时器管理、错误处理等关键能力
class Utils
{
public:
    Utils(){}
    ~Utils(){}

    void init(int timeslot);

    //对文件描述符设置非阻塞
    int setnonblocking(int fd);
    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT(保证事件只触发一次)
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);
    //信号处理函数
    static void sig_handler(int sig);
    //设置信号函数,void(handler)(int) 本质是函数指针的语法糖，核心是约束传入的 handler 必须是 “接收 int、返回 void” 的函数
    void addsig(int sig, void(handler)(int), bool restart = true);
    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();
    void show_error(int connfd, const char *info);
public:
    static int *u_pipefd;
    sort_timer_lst m_timer_lst;
    static int u_epollfd;
    int m_TIMESLOT;

};

void cb_func(client_data *user_data);

#endif