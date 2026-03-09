#ifndef LOCKER_H
#define LOCKER_H
#endif

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
            throw std::exception();
        }
        
    }
private:
    sem_t m_sem;
};
