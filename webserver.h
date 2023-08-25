#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"


const int MAX_FD = 65536;
const int MAX_EVENT_NUMBER = 10000;//最大事件数
const int TIMESLOT = 5;     //最小超时单位


class Webserver
{
public:
    Webserver();
    ~Webserver();
    // 初始化参数
    void init(int port,string user,string passWord,string databaseName,int log_write,
              int op_linger,int trigmode,int sql_num,int thread_num,int close_log,int actor_model);
    void thread_pool();
    void sql_pool();
    void log_write();
    void trig_mode();
    void eventListen();
    void eventLoop();
    void timer(int connfd,struct sockaddr_in client_address);
    void adjust_timer(util_timer *timer);
    void deal_timer(util_timer *timer,int sockfd);
    bool dealclientdata();
    bool dealwithsignal(bool& timeout,bool& stop_server);
    void dealwithread(int sockfd);
    void dealwithwrite(int sockfd);

public:
    //基础参数
    // 端口号
    int m_port;
    // 根目录
    char *m_root;
    // 是否异步写日志 0 异步 1 非异步
    int m_log_write;
    // 是否开启日志 0 开启 1不开启
    int m_close_log;
    // 是reactor模式 还是 Proactor模式
    int m_actormodel;
    // 管道
    int m_pipefd[2];
    //epoll文件描述符
    int m_epollfd;
    http_conn *users;

    // 数据库相关
    // 数据库实例
    connection_pool *m_connPool;
    // 用户名
    string m_user;
    // 密码
    string m_passWord;
    // 数据库名
    string m_databaseName;
    // 数据库连接池大小
    int m_sql_num;

    // 线程相关 线程池  线程中主要处理http的解析和响应工作
    threadpool<http_conn> *m_pool;
    // 线程池中个数
    int m_thread_num;

    // epoll_event相关 epoll要监听的时间数组
    epoll_event events[MAX_EVENT_NUMBER];
    // 监听的文件描述符
    int m_listenfd;
    // 是否优雅关闭 启用linger和延长时间  启用linger在关闭连接前要等待一段时间
    int m_OPT_LINGER;
    // 触发模式
    int m_TRIGModel;
    // 监听的触发模式
    int m_LISTENTrimode;
    // 连接的触发模式
    int m_CONNTrigmode;

    // 定时器相关
    client_data *users_timer;
    // 工具类
    Utils utils;

};


#endif