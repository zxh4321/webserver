#include"lst_timer.h"
#include"../http/http_conn.h"
#include "../log/log.h"

sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head= tmp->next;
        delete tmp;
        tmp = head;
    }
    
}
void sort_timer_lst::add_timer(util_timer*timer)
{
    if(!timer)
    {
        return;
    }
    if(!head)
    {
        head = tail = timer;
        return;
    }
    // 根据expire时间进行排序 时间短的在前面
    if(timer->expire <head->expire)
    {
        timer->next=head;
        head->prev = timer;
        head= timer;
        return;
    }
    add_timer(timer,head);

}

void sort_timer_lst::add_timer(util_timer *timer,util_timer *lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    while(tmp){
        // 传入的结点与整个list进行比较如果时间比遍历到的结点时间小 则插入在那个结点前面
        if(timer->expire <tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp=tmp->next; 
    }
    // 遍历到最后空结点
    if(!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if(!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;
    // 如果需要调整的结点是最后一个结点 或者调整后的结点时间仍然比后一个结点小 则不需要变动
    if(!tmp ||(timer->expire <tmp->expire))
    {
        return;
    }
    // 如果调整的是头结点 删除头结点并重新插入
    if(timer==head)
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer,head);
    }else
    {
        // 将结点从链表中移除 然后重新插入链表中
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer,timer->next);
    }
}

void sort_timer_lst::del_timer(util_timer *timer)
{
    if(!timer)
    {
        return;
    }
    // 链表中唯一一个元素
    if((timer==head) && (timer==tail)){
        delete timer;
        head= NULL;
        tail = NULL;
    }
    // 删除的是头结点
    if(timer ==head)
    {
        head = head->next;
        head->prev = NULL;

        delete timer;
        return;
    }
    // 删除的是尾结点
    if(timer==tail)
    {
        tail = tail->prev;
        tail->next=NULL;
        delete timer;
        return;    
    }
    // 删除的是中间结点
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
   
}
// 定时任务处理函数
void sort_timer_lst::tick()
{
    if(!head)
    {
        return;
    }
    time_t cur = time(NULL); //获取当前时间
    util_timer *tmp = head;  
    // 遍历定时器链表
    while(tmp)
    {
        // 链表容器为升序
        // 当前时间小于定时器的超时时间 后面的定时器也没有到期
        if(cur<tmp->expire)
        {
            
            break;
        }
        // 当前定时器到期 调用回调函数 执行定时事件
        tmp->cb_func(tmp->user_data);
        // 将处理后的定时器从链表中容器中删除，并重置头结点
        head = tmp->next;
        if(head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}
void Utils::init(int timeslot)
{
    m_TIMESLOT  = timeslot;
}
// 对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd,F_GETFL);
    int new_option = old_option |O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}

// 将内核事件注册读事件，ET模式，选择开启EPOLLONESHOT模式
void Utils::addfd(int epollfd,int fd,bool one_shot,int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    if(1==TRIGMode)
    {
        event.events = EPOLLIN |EPOLLET |EPOLLRDHUP;
    }else{
        event.events = EPOLLIN |EPOLLRDHUP;
    }
    if(one_shot)
        event.events |=EPOLLONESHOT;
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    // 将监听的文件描述符设置为非阻塞
    setnonblocking(fd);
}

// 信号处理函数
void Utils::sig_handler(int sig)
{
    int save_errno = errno;
    int msg = sig;
    // 利用管道发送信号
    send(u_pipefd[1],(char*)&msg,1,0);
    errno = save_errno;
}

// 设置信号函数
void Utils::addsig(int sig,void(handler)(int),bool restart)
{
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    // 指定信号处理函数
    sa.sa_handler = handler;
    // 收到信号时重启
    if(restart)
        sa.sa_flags |=SA_RESTART;
    // 将sa.sa_mask设置为所有信号的集合，以阻塞在处理此信号期间可能发生的其他信号
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig,&sa,NULL)!=-1);
}
// 定时器处理函数 从链表中取出
void Utils::timer_handler()
{
    m_timer_lst.tick();
    // 设置定时器 m_TIMESLOT之后发送SIGALRM信号 信号发送后被捕获调用sig_handler回调函数通过socket管道发送信号
    alarm(m_TIMESLOT);
}
// 向客户端发送 错误 数据
void Utils::show_error(int connfd,const char *info)
{
    send(connfd,info,strlen(info),0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;
class Utils;
// 定时器处理的具体函数 将用户的从epoll中移除 关闭连接
void cb_func(client_data *user_data)
{
    epoll_ctl(Utils::u_epollfd,EPOLL_CTL_DEL,user_data->sockfd,0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}