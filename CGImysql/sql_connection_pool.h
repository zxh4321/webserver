#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_
#include <exception>
#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"
#include "../log/log.h"

using namespace std;
class connection_pool
{
public:
    MYSQL *GetConnection();                 //获得数据库连接
    bool RealeaseConnection(MYSQL *conn);   //释放连接
    int GetFreeConn();                      //获得连接
    void DestroyPool();                     //销毁所有连接


    static connection_pool* GetInstance();
    void init(string url,string User,string PassWord,string DataBaseName,int port,int MaxConn,int close_log);
private:
    connection_pool();
    ~connection_pool();

    int m_Maxconn;  //最大连接数
    int m_CurConn;  //当前连接数
    int m_FreeConn; //当前空闲数
    locker lock;
    list<MYSQL*> connList; //连接池
    sem reserve;
public:
    string m_url;       //主机地址
    string m_Port;      //数据库登录端口
    string m_User;      //数据库用户名
    string m_PassWord;  //数据库密码
    string m_DatabaseName; //数据库名称
    int m_close_log;

   
};

class connectionRAII
{
private:
    MYSQL *conRAII;
    connection_pool * poolRAII;
public:
    connectionRAII(MYSQL **con,connection_pool *connPool);
    ~connectionRAII();
};


#endif