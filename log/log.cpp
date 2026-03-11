#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
using namespace std;
Log::Log()
{
    m_count = 0;
    m_is_async = false;
}

Log::~Log()
{
    if (m_fp != NULL)
    {
        fclose(m_fp);
    }
}

//异步需要设置阻塞队列的长度，同步不需要设置
bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size)
{
    //如果设置了max_queue_size,则设置为异步
    if(max_queue_size>=1)
    {
        m_is_async =true;
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;
        //flush_log_thread为回调函数,这里表示创建线程异步写日志
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }
    m_close_log = close_log;
    m_log_buf_size=log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf,'\0',m_log_buf_size);
    m_split_lines = split_lines;
    //获取当前系统的时间戳
    time_t t =time(NULL);
    //格式化日志内容的时间戳,解决时区问题
    struct tm*sys_tm = localtime(&t);
    //将解析后的本地时间数据「固化」到独立变量中，避免静态缓冲区被覆盖导致的数据错误。
    struct tm my_tm = *sys_tm;

    //找到路径的最后一个/
    const char*p =strrchr(file_name,'/');
    char log_full_name[256] = {0};

    //文件名无目录路径
    if (p == NULL)
    {
        //拼接格式为 年_月_日_原文件名  2024_05_20_app.log
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }
    //文件名包含目录路径 file_name = "./logs/app.log"
    else
    {
        //p 指向 /，p+1 指向 app.log  log_name = "app.log"
        strcpy(log_name, p + 1);
        //将目录路径复制到 dir_name
        strncpy(dir_name, file_name, p - file_name + 1);
        //拼接格式为 目录 + 年_月_日_原文件名  最终 log_full_name 为 ./logs/2024_05_20_app.log。
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }
    m_today = my_tm.tm_mday;
    //打开文件,若文件不存在：自动创建该文件,文件存在则以追加的方式写入
    m_fp = fopen(log_full_name, "a");

    if (m_fp == NULL)
    {
        return false;
    }

    return true;
}
//va_start(valst, format) 会让 valst 指向 format 后面的第一个可变参数（比如 write_log(1, "user %d", 1001) 中，valst 会指向 1001）；
void Log::write_log(int level, const char *format, ...)
{
    struct timeval now = {0, 0};
    //作用是获取当前的系统时间，并存储到第一个参数 now 结构体中
    gettimeofday(&now,NULL);
    //时间戳
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16] = {0};
    //// 基于日志级别（整数）进行分支判断,给字符数组 s 赋值对应的日志级别字符串前缀
    switch (level)
    {
    case 0:
        strcpy(s, "[debug]:");
        break;
    case 1:
        strcpy(s, "[info]:");
        break;
    case 2:
        strcpy(s, "[warn]:");
        break;
    case 3:
        strcpy(s, "[erro]:");
        break;
    default:
        strcpy(s, "[info]:");
        break;
    }

    //写入一个log，对m_count++, m_split_lines最大行数
    m_mutex.lock();
    m_count++;
    //当前日期和日志文件的日期不一致 或者 当前日志文件的行数达到了预设的最大行数
    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) //everyday log
    {   
        char new_log[256] = {0};
        // 刷新缓冲区：把内存中未写入磁盘的日志内容强制写入
        fflush(m_fp);
        // 关闭当前日志文件句柄，避免文件句柄泄漏
        fclose(m_fp);
        //构造一个日期的后缀，因为要换日志了
        char tail[16] = {0};
        //格式为「年_月_日_」（比如 2024_05_21_）
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);

        //跨天切换（日期变了）
        if (m_today != my_tm.tm_mday)
        {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;// 更新当前日期为新日期
            m_count = 0;            // 重置日志行数计数器
        }
        //同天但行数达到阈值（按行数切分）目录名 + 日期后缀 + 日志名.序号（比如 ./log/2024_05_21_run.log.1
        else
        {
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        m_fp = fopen(new_log, "a");
    }
    m_mutex.unlock();

    //定义va_list类型变量，本质是指向可变参数列表的指针，用于遍历可变参数。
    va_list valst;
    //初始化valst，让它指向format之后的第一个可变参数  对应 format 里的 %d/%s 等占位符的实际参数
    //format是格式化字符串（比如 "user %d login, ip: %s"）
    va_start(valst, format);

    string log_str;
    m_mutex.lock();

    //写入的具体时间内容格式,例如：2024-05-20 14:30:25.123456 [info]:
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    
    //m_buf + n	写入的起始位置
    //m_log_buf_size -n -1	本次写入的最大字符数-1 是预留 \0 终止符的位置，避免缓冲区溢出
    //把 1001、"192.168.1.1" 填入格式化串，得到："user 1001 login, ip: 192.168.1.1"；
    int m = vsnprintf(m_buf + n, m_log_buf_size - n - 1, format, valst);

    m_buf[n + m] = '\n';// 日志行末尾加换行符
    m_buf[n + m + 1] = '\0';// 字符串终止符，保证C风格字符串合法
    //最后的日志内容
    log_str = m_buf;

    m_mutex.unlock();

    if (m_is_async && !m_log_queue->full())
    {
        m_log_queue->push(log_str);// 异步：放入阻塞队列，由专门线程写入文件
    }
    else
    {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);// 同步：直接写入文件
        m_mutex.unlock();
    }

    va_end(valst);
}



void Log::flush(void)
{
    m_mutex.lock();
    //强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}