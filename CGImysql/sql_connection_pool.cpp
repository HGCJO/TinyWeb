#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

//初始化
connection_pool::connection_pool()
{
	m_CurConn = 0;
	m_FreeConn = 0;
}
connection_pool *connection_pool::GetInstance()
{
    //定义一次数据池
    static connection_pool connPool;

    return &connPool;
}
//构造初始化,让连接池持有数据库的核心配置
//工作线程从数据库连接池取得一个连接，访问数据库中的数据，访问完毕后将连接交还连接池。
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
    m_url = url;
    m_Port = Port;
    m_User = User;
	m_PassWord = PassWord;
	m_DatabaseName = DBName;
	m_close_log = close_log;
    //创建很多连接，用于后续使用
    for(int i = 0;i <MaxConn;i++)
    {
        //用于初始化一个MYSQL结构体（
        MYSQL *con = NULL;
        con = mysql_init(con);

        if(con == NULL)
        {
            LOG_ERROR("MySQL Error");
            exit(1);
        }
        //MySQL C API 的核心函数，用于建立与 MySQL 服务器的实际网络连接
        con = mysql_real_connect(con,url.c_str(),User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);
        if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}
        //将一次连接放入链表中
        connList.push_back(con);
        ++m_FreeConn;
    }
    //将信号量初始化为最大连接次数
    reserve = sem(m_FreeConn);
    m_MaxConn = m_FreeConn;
}

//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
    MYSQL *con =NULL;
    //无连接
    if(0 == connList.size())
    {
        return NULL;
    }
    //使用一个连接
    reserve.wait();
    lock.lock();

    con=connList.front();
    connList.pop_front();

    --m_FreeConn;
	++m_CurConn;

    lock.unlock();
	return con;

}

//释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
    if(con == NULL)
    {
        return false;
    }
    lock.lock();
    //将使用的con连接放回到连接池链表中
    connList.push_back(con);
    ++m_FreeConn;
	--m_CurConn;

    lock.unlock();
	reserve.post();
	return true;
}

//销毁数据库连接池
void connection_pool::DestroyPool()
{
    lock.lock();

    if(connList.size() > 0 )
    {   
        //使用容器的迭代器
        list<MYSQL *>::iterator it;
        for(it=connList.begin();it != connList.end();++it)
        {
            MYSQL *con = *it;
            //关闭每一个连接
            mysql_close(con);    
        }
        m_CurConn=0;
        m_FreeConn=0;
        connList.clear();
    }
    lock.unlock();
}

//当前空闲的连接数
int connection_pool::GetFreeConn()
{
	return this->m_FreeConn;
}

connection_pool::~connection_pool()
{
	DestroyPool();
}
//自动、安全地获取数据库连接，并绑定连接与连接池的生命周期
connectionRAII::connectionRAII(MYSQL **SQL,connection_pool *connPool)
{
    // 1. 从连接池获取可用连接，并赋值给外部传入的MYSQL*指针（通过二级指针修改外部变量）
    *SQL = connPool->GetConnection();
    // 2. 保存获取到的连接到当前RAII对象的成员变量（conRAII）
    conRAII =*SQL;
    // 3. 保存连接池指针到当前RAII对象的成员变量（poolRAII）
    poolRAII = connPool;

}

connectionRAII::~connectionRAII(){
	poolRAII->ReleaseConnection(conRAII);
}