#ifndef LST_TIMER
#define LST_TIMER
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <error.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"
class util_timer;
// 连接资源
struct client_data
{
    // 客户端cosket地址
    sockaddr_in address;
    // socket文件描述符
    int sockfd;
    // 定时器
    util_timer *timer;

};
// 定时器类 结点类型 
class util_timer
{
public:
    util_timer():prev(NULL),next(NULL){}
public:
    // 超时时间
    time_t expire;
    // 回调函数
    void (*cb_func)(client_data *);
    // 连接资源
    client_data *user_data;
    // 前向定时器
    util_timer *prev;
    // 后继定时器
    util_timer *next;
};
// 具有优先级排列的定时器链表
class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();
    void add_timer(util_timer *timer);
    void adjust_timer(util_timer *timer);
    void del_timer(util_timer *timer);
    void tick();
private:
    void add_timer(util_timer *timer,util_timer *lst_head);
    util_timer *head;
    util_timer *tail;
};
// 工具类
class Utils
{
private:
    /* data */
public:
    Utils(){};
    ~Utils(){};
    void init(int timeslot);

    // 对文件描述符设置为非阻塞
    int setnonblocking(int fd);

    // 将内核事件注册读事件，ET模式选择开启EPOLLONESHOT
    void addfd(int epollfd,int fd,bool one_shot,int TRIGMode);
    // 信号处理函数
    static void sig_handler(int sig);

    // 设置信号函数
    void addsig(int sig,void(handler)(int),bool restart = true);
    // 定时处理任务，重新定时以不断触发SIGALARM信号
    void timer_handler();

    void show_error(int connfd,const char * info);
public:
    static int *u_pipefd;
    sort_timer_lst m_timer_lst;
    static int u_epollfd;
    int m_TIMESLOT;
};
// 定时器的回调函数
void cb_func(client_data *user_data);

#endif