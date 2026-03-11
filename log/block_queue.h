/*************************************************************
*循环数组实现的阻塞队列，m_back = (m_back + 1) % m_max_size;  
*线程安全，每个操作前都要先加互斥锁，操作完后，再解锁
**************************************************************/
//生产者线程将任务对象 / 任务数据放入阻塞队列，消费者线程从队列中取出任务并执行
#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include "../lock/locker.h"

using namespace std;

template <class T>
class block_queue
{
public:
    block_queue(int max_size = 1000)
    {
        if(max_size <=0)
        {
            exit(-1);
        }
        m_max_size = max_size;
        m_array = new T[max_size];
        m_size = 0;
        m_front = -1;
        m_back = -1;
    }

    void clear()
    {
        m_mutex.lock();
        m_size = 0;
        m_front = -1;
        m_back =-1;
        m_mutex.unlock();
    }

    ~block_queue()
    {
        m_mutex.lock();
        if (m_array != NULL)
        {   delete [] m_array;
            m_array = NULL; // 建议添加，避免野指针
        }

        m_mutex.unlock();
    }

    //判断队列是否满了
    bool full()
    {
        m_mutex.lock();
        if(m_size >= m_max_size)
        {
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }
    //判断队列是否为空
    bool empty()
    {
        m_mutex.lock();
        if(m_size == 0)
        {
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }
    //返回队首元素
    bool front(T &value)
    {
        m_mutex.lock();
        if(0 == m_size)
        {
            m_mutex.unlock();
            return false;

        }
        value = m_array[m_front];
        m_mutex.unlock();
        return true;
    }
    //返回队尾元素
    bool back(T &value) 
    {
        m_mutex.lock();
        if (0 == m_size)
        {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_back];
        m_mutex.unlock();
        return true;
    }
    int size()
    {
        int tmp = 0;
         m_mutex.lock();
        tmp = m_size;

        m_mutex.unlock();
        return tmp;
    }
    int max_size()
    {
        int tmp = 0;

        m_mutex.lock();
        tmp = m_max_size;

        m_mutex.unlock();
        return tmp;
    }
    //往队列添加元素，需要将所有使用队列的线程先唤醒
    //当有元素push进队列,相当于生产者生产了一个元素
    //若当前没有线程等待条件变量,则唤醒无意义
    bool push(const T &item)
    {
        m_mutex.lock();
        if(m_size >= m_max_size)
        {
            //唤醒它们可以让消费线程尝试取元素，尽快让队列腾出空间
            m_cond.broadcast();
            m_mutex.unlock();
            return false;
        }
        //若 m_back 已经到数组最后一个位置（m_max_size-1），+1 后取余会回到 0
        m_back = (m_back +1)%m_max_size;
        //将待入队的元素 item 放入队列尾部的位置。
        m_array[m_back] = item;
        m_size++;

        m_cond.broadcast();
        m_mutex.unlock();
        return true;
    }
    //pop时,如果当前队列没有元素,将会等待条件变量,无超时pop需要「永久阻塞、反复检查」
    bool pop(T &item)
    {
        m_mutex.lock();
        while (m_size <= 0)
        {
            //如果队列为空，则会阻塞等待
            /*wait 方法的作用是：
            释放当前持有的互斥锁（m_mutex）；
            让当前线程进入阻塞状态，等待被生产者的 broadcast/signal 唤醒；
            被唤醒后，重新获取互斥锁，然后退出 wait 方法。*/
            if(!m_cond.wait(m_mutex.get()))
            {
                m_mutex.unlock();
                return false;
            }
        }

        m_front = (m_front+1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }
    //增加了超时处理,带超时pop是「单次限时等待、超时即退出」
    bool pop(T &item, int ms_timeout)
    {
        // 1. 定义时间相关结构体：timespec用于设置超时时间，timeval用于获取当前系统时间
        struct timespec t = {0,0};
        struct timeval now = {0, 0};
        //// 2. 获取当前系统的精确时间（秒 + 微秒），存入now变量
        gettimeofday(&now, NULL);
        m_mutex.lock();
        if (m_size <= 0)
        {
            // 5. 计算超时时间点：
            t.tv_sec = now.tv_sec + ms_timeout / 1000;
            t.tv_nsec = (ms_timeout % 1000) * 1000;


            // 6. 调用条件变量的超时等待接口：
            if (!m_cond.timewait(m_mutex.get(), t))
            {
                m_mutex.unlock();
                return false;
            }
        }
        //超时等待后再次检查 m_size <= 0 是必要且关键的安全设计
        //当超时时间耗尽时，timewait 会返回失败，但此时队列仍可能为空
        if (m_size <= 0)
        {
            m_mutex.unlock();
            return false;
        }

        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }
    

private:
    locker m_mutex;
    cond m_cond;

    T *m_array;
    int m_size;
    int m_max_size;
    int m_front;
    int m_back;
};



#endif