#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template<typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request, int state);
    bool append_p(T *request);
private:
/*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void*worker(void *arg);
    void run();

private:
    int m_thread_number;        //线程池的线程数量
    int m_max_requests;         //请求队列中允许的最大请求数量
    pthread_t *m_threads;       //描述线程池的数组，大小为m_thread_number,存放线程池中每个线程的tid
    std::list<T*>m_workqueue;   //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理
    connection_pool *m_connPool;//数据库
    int m_actor_model;          //模型切换
};
template<typename T>
threadpool<T>::threadpool(int actor_model,connection_pool *connPool,int thread_number,int max_requests) :m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)
{
    if(thread_number< = 0 || max_requests <=0 )
        throw std::exception();
    m_threads = new pthread_t[m_thread_number];//描述线程池的数组   
    if(!m_threads)
        throw std::exception();
    
    for(int i=0;i<thread_number;++i)
    {
        if(pthread_create(m_threads +i;NULL,worker,this )!=0)//参数 1 m_threads + i：指向线程池数组 m_threads 的第 i 个元素，用于存储新创建线程的 ID；worker代表线程工作回调函数,this为其参数
        {
            delete[] m_threads;//释放已分配的线程 ID 数组,用 new[] 分配内存，却用普通 delete（而非 delete[]）释放，会导致内存泄漏,会遍历整个数组，释放所有元素的内存，完成完整的内存回收。
            throw std::exception();
        }

        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }

}

template<typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}

//添加任务
template<typename T>
bool threadpool<T>::append(T*request,int state)
{
    m_queuelocker.lock();
    if(m_workqueue.size()>= m_max_requests)
    {
        m_queuelocker.unlock();
        return flase;
    }
    request->m_state=state;
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();//有任务需要处理，信号量加一，添加一个任务
    return true;
}

template<typename T>
bool threadpool<T>::append_p(T*request)
{
    m_queuelocker.lock();
    if(m_workqueue.size()>=m_max_requests)
    {
        m_queuelocker.unlock();
        return flase;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}


//线程回调函数
template<typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool =(threadpool*)arg;//用普通对象T request，对象会被分配在栈上，会出现野指针
    pool.run();
    retunr pool;
}

//真正的工作函数
template<typename T>
void threadpool<T>::run()
{
    while(true)
    {
        m_queuestat.wait();//消耗一个信号量，处理一个任务
        m_queuelocker.lock();
        if(m_workqueue.empty())//任务队列为空,跳过当前循环
        {
            m_queuelocker.unlock();
            continue;
        }
        T*request = m_workqueue.front();//队列的第一个任务拿出来
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if(!request)
        {
            continue;
        }
        if(m_actor_model==1)
        {
            if(0==request->m_state)
            {   
                if(request->read_once())
                {
                    request->improv =1;
                    connectionRAII mysqlcon(&request->mysql,m_connPool);
                    request->process();
                }
                else
                {
                    request->improv=1;
                    request->time_flag=1;
                }
            }
            else
            {
                if(request->write())
                {
                    request->improv=1;
                }
                else
                {
                    request->improv=1;
                    request->time_flag=1;
                }
            }
        }
        else
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
    }
}
#endif