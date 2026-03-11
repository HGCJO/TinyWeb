#ifndef LOCKER_H
#define LOCKER_H

#include<exception>
#include<pthread.h>
#include<semaphore.h>

//定义信号量
class sem
{
public:
    sem()
    {
        if (sem_init(&m_sem,0,0)!=0)
        {
            //抛出异常
            throw std::exception();
        }
    }
    sem(int num)
    {
        if (sem_init(&m_sem, 0, num) != 0)
        {
            throw std::exception();
        }
    }
    ~sem()
    {
        sem_destroy(&m_sem);
    }
    bool wait()
    {
        return sem_wait(&m_sem)==0;//成功返回0，最终返回true，成功消耗一个
    }
    bool post()//有任务
    {
        return sem_post(&m_sem)==0;//成功返回0，最终返回true,成功生成一个
    }

private:
    sem_t m_sem;
};

//定义锁
class locker 
{
public:
    locker()
    {
        if(pthread_mutex_init(&m_mutex,NULL)!=0)
        {
            throw std::exception();

        }
    }
    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);

    }
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex)==0;

    }
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex)==0;
    }
    pthread_mutex_t * get()
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};

//定义条件
class cond
{
public:
    cond()
    {
        if(pthread_cond_init(&m_cond,NULL)!=0)
        {
            throw std::exception();

        }

    }
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }
    bool wait(pthread_mutex_t *m_mutex)
    {
        int ret =0 ;
        ret=pthread_cond_wait(&m_cond,m_mutex);//解开锁--阻塞等待--成功唤醒---重新加锁
        return ret ==0;
    }
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t)
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool signal()//挑选一个线程唤醒
    {
        return pthread_cond_signal(&m_cond)==0;
    }
    bool broadcast()//所有等待 m_cond 的线程都会被唤醒,这些线程会同时竞争关联的互斥锁。
    {
        return pthread_cond_broadcast(&m_cond) == 0;
    }


private:
    pthread_cond_t m_cond;
};

#endif