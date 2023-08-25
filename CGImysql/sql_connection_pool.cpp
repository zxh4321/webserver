#include <mysql/mysql.h>
#include <exception>
#include <stdio.h>
#include <string>
#include <string.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;
connection_pool::connection_pool()
{
    m_CurConn = 0;
    m_FreeConn = 0;
}
// 单例模式
connection_pool* connection_pool::GetInstance()
{
    static connection_pool connPool;
    return &connPool;
}
// 初始话数据库连接池 根据设定的参数创建连接实例
void connection_pool::init(string url,string User,string PassWord,string DBName,int Port,int MaxConn,int close_log)
{
    m_url = url;
    m_User = User;
    m_Port = Port;
    m_PassWord = PassWord;
    m_DatabaseName = DBName;
    m_close_log = close_log;
    for(int i=0;i<MaxConn;i++){
        MYSQL * con=NULL;
        con = mysql_init(con);
        if(con ==NULL){
            LOG_ERROR("MySQL Error");
            exit(1);
        }
        // c_str 通过该函数可以将字符串转换成具有c风格的字符串
        con = mysql_real_connect(con,url.c_str(),User.c_str(),PassWord.c_str(),DBName.c_str(),Port,NULL,0);
        if(con==NULL){
            LOG_ERROR("MySQL Error");
            exit(-1);
        }
        connList.push_back(con);
        ++m_FreeConn;
    }
    reserve = sem(m_FreeConn);
    m_Maxconn = m_FreeConn;
}

// 当有请求时，从数据连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL*connection_pool::GetConnection()
{
    MYSQL*con = NULL;
    // 连接池内没有连接可用
    if(0==connList.size()){
        return NULL;
    }
    reserve.wait();
    lock.lock();
    con = connList.front();
    connList.pop_front();
    --m_FreeConn;
    ++m_CurConn;
    lock.unlock();

    return con;

}

// 释放当前连接 将连接重新加入连接池中
bool connection_pool::RealeaseConnection(MYSQL* con)
{
    if(con==NULL){
        return false;
    }
    lock.lock();
    connList.push_back(con);
    ++m_FreeConn;
    --m_CurConn;
    lock.unlock();
    reserve.post();
    return true;


}

// 销毁数据库连接池
void connection_pool::DestroyPool()
{
    lock.lock();
    if(connList.size()>0)
    {
        // 构造迭代器 对list中的连接进行关闭
        list<MYSQL*>::iterator it;
        for(it=connList.begin();it!=connList.end();it++){
            MYSQL * con = *it;
            mysql_close(con);       //关闭连接
        }
        m_CurConn = 0;
        m_FreeConn = 0;
        connList.clear();
    }
    lock.unlock();
}


// 当前空闲数
int connection_pool::GetFreeConn()
{
    return this->m_FreeConn;
}

connection_pool::~connection_pool()
{
    DestroyPool();
}

connectionRAII::connectionRAII(MYSQL **SQL,connection_pool* connPool)
{
    *SQL = connPool->GetConnection();
    conRAII = *SQL;
    poolRAII = connPool;
}
connectionRAII::~connectionRAII()
{
    poolRAII->DestroyPool();
}