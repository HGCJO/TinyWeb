#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}
//逐个删除所有 util_timer 类型的节点
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while(tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}
void sort_timer_lst::add_timer(util_timer *timer)
{
    if(!timer)
    {
        return;
    }
    //只有一个节点时，这个节点既是头节点（head）也是尾节点（tail）
    if(!head)
    {
        head=tail=timer;
        return;
    }
    //定时器的排序要从小到大
    if(timer->expire < head->expire)
    {
        timer->next=head;
        head->prev=timer;

        head =timer;
        return;
    }
    add_timer(timer,head);
}

void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if(!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;
    //判断是否需要调整位置，!tmp：判断当前定时器是否是链表的尾节点
    if(!tmp || (timer->expire < tmp->expire ))
    {
        return;
    }
    //被调整的定时器恰好是链表头节点,重新插入到链表中合适的位置
    if(timer == head)
    {
        head = head->next;
        head->prev=NULL;
        timer->next=NULL;
        add_timer(timer,head);
    }
    else//调整已存在于链表中的定时器节点位置
    {
        //原链表是：A(10s) → B(20s) → C(30s) → D(40s)
        //先执行B->prev(A)->next = B->next(C) ====→ A->next指向C
        timer->prev->next = timer->next;
        //再执行B->next(C)->prev = B->prev(A) ====→ C->prev指向A
        timer->next->prev = timer->prev;
        //此时链表变为A → C → D，B被摘除；
        add_timer(timer,timer->next);
    }
}
void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    //只有timer这一个节点
    if ((timer == head) && (timer == tail))
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    //被调整的定时器恰好是链表头节点
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next=NULL;
        delete timer;
        return;
    }
    timer->prev->next=timer->next;
    timer->next->prev=timer->prev;
    delete timer;
}

void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }
    //当前的时间
    time_t cur = time(NULL);
    util_timer *tmp = head;
    //由于定时器链表是按过期时间从小到大排序的（参考add_timer逻辑），
    //链表头节点是最早到期的定时器，因此一旦遇到未到期的定时器，后续节点必然也未到期，可直接终止循环
    while(tmp)
    {
        //当前时间戳(cur) < 定时器过期时间戳(tmp->expire)，说明这个定时器还没到期
        if(cur < tmp->expire)
        {
            break;
        }
        //说明定时器已到期
        //执行回调函数
        tmp->cb_func(tmp->user_data);
        //链表头指针指向当前节点的下一个节点（因为当前节点要被删除)
        head = tmp->next;
        //新头节点存在
        if(head)
        {
            //
            head->prev = NULL;
        }
        //新头节点不存在，链表清空l
        delete tmp;
        tmp=head;
    }
}

void sort_timer_lst::add_timer(util_timer *timer , util_timer *lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    while(tmp)
    {
        //当前顺序为 A -> C -> D, 要插入一个B
        if(timer->expire < tmp->expire)
        {
            //A与C断开
            prev->next=timer;
            //B与C连接
            timer->next=tmp;
            //C与B连接
            tmp->prev=timer;
            //B与A连接
            timer->prev=prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    //只有一个头节点
    if(!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

void Utils::init(int timesolt)
{
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;
    
    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//信号处理函数
void Utils::sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    //通过管道（pipe） 将信号编号发送到主程序的 epoll 事件循环中处理
    //u_pipefd：是一个全局 / 类内的管道文件描述符对（u_pipefd[0] 读端，u_pipefd[1] 写端），且读端已被注册到 epoll 中。
    //只发送 1 个字节：因为信号编号是整数
    send(u_pipefd[1],(char*)&msg,1,0);
    errno = save_errno;
}
//设置信号函数
//int sig：要处理的信号编号,void(handler)(int)：函数指针，指向自定义的信号处理函数
//bool restart：是否开启「被信号中断的系统调用自动重启」功能
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    struct sigaction sa;
    //将 sa 的所有字节置 0
    memset(&sa, '\0', sizeof(sa));
    //将 sigaction 结构体的 sa_handler 成员赋值为传入的自定义信号处理函数指针 handler。
    sa.sa_handler = handler;

    if (restart)
    //设置 SA_RESTART 标志，让被信号中断的系统调用自动重启
        sa.sa_flags |= SA_RESTART;
    //执行当前信号处理函数时，阻塞所有其他信号
    sigfillset(&sa.sa_mask);
    //若返回 -1（表示设置失败），则触发断言，终止程序并提示错误
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);//设置一个闹钟，在 m_TIMESLOT 秒后向当前进程发送 SIGALRM 信号；
}

void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;
class Utils;

void cb_func(client_data *user_data)
{
    epoll_ctl(Utils::u_epollfd,EPOLL_CTL_DEL,user_data->sockfd,0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}