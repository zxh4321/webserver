#include "webserver.h"

Webserver::Webserver()
{
    // http_conn类对象  http_conn类对象中包含了对于连接的客户端http报文的解析、根据解析所做出的相应报文 因此将一个客户端连接作为一个对象
    users = new http_conn[MAX_FD];

    // root文件夹路径
    char server_path[200];
    getcwd(server_path,200);  //getcwd会将当前的工作路径的绝对路径复制到server_path数组中
    char root[6] = "/root";
    m_root = (char*)malloc(strlen(server_path)+strlen(root)+1);
    strcpy(m_root,server_path);
    strcat(m_root,root);   //通过字符串拼接root目录
    // client_data 中包括了客户端地址 socket文件描述符 以及一个定时器
    users_timer = new client_data[MAX_FD];
}
Webserver::~Webserver()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete [] users_timer;
    delete m_pool;
}
// 初始化参数
void Webserver::init(int port,string user,string passWord,string databaseName,int log_write,
              int op_linger,int trigmode,int sql_num,int thread_num,int close_log,int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = op_linger;
    m_TRIGModel = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

void Webserver::trig_mode()
{
    // 水平触发和边缘触发
    // LT +LT
    if(0==m_TRIGModel)
    {
        m_LISTENTrimode = 0;
        m_CONNTrigmode = 0;
    }else if(1==m_TRIGModel)
    {
        // LT+ET
        m_LISTENTrimode = 0;
        m_CONNTrigmode = 1;
    }else if(2 ==m_TRIGModel)
    {
        // ET+LT
        m_LISTENTrimode = 1;
        m_CONNTrigmode = 0;
    }else if(3==m_TRIGModel)
    {
        // ET+ET
        m_LISTENTrimode=1;
        m_CONNTrigmode = 1;
    }
}

void Webserver::thread_pool()
{
    //线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}
void Webserver::log_write()
{
    if(0==m_close_log)
    {
        // 初始化日志
        if(1==m_log_write)
        {
            // 异步输出日志  
            Log::get_instance()->init("./ServerLog",m_close_log,2000,800000,800);
        }else{
            // 同步输出日志
            Log::get_instance()->init("./ServerLog",m_close_log,2000,800000,0);
        }
    }

}
 // 初始化数据库连接池
void Webserver::sql_pool()
{
//    创建一个连接池实例
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost",m_user,m_passWord,m_databaseName,3306,m_sql_num,m_close_log);

    // 初始化数据库读取表  根据传入的数据库的相关信息 查询所有用户数据并存在一个map中
    users->initmysql_result(m_connPool);
}
void Webserver::eventListen()
{
    // 网络编程基础步骤
    m_listenfd = socket(PF_INET,SOCK_STREAM,0);
    assert(m_listenfd>=0);

    // 优雅关闭连接  
    if(0==m_OPT_LINGER)
    {
        /*
            struct linger是网络编程中的结构体 用于控制套接字关闭时的行为
            struct linger{
                int l_onoff;  是否启用linger
                int l_linger; 延迟关闭时间
            }
        */ 
        struct linger tmp = {0,1};   //表示在关闭套接字时使用linger 延迟关闭时间为1秒
        /*
            setsockopt (int sockfd, int level, int optname,
		       const void *optval, socklen_t optlen)
            用于设置套接字选项值
            sockfd:要设置的套接字
            level：选项所属协议层  SOL_SOCKET表示在套接字层面上设置选项
            optname:选项的值 在这里SO_LINGR表示设置linger选项
            optval：选项值 通常为结构体地址、
            optlen: 选项值的长度

        */
        setsockopt(m_listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));
    }else if (1 == m_OPT_LINGER){
        struct linger tmp ={1,1};
        setsockopt(m_listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp)); //关闭套接字时启用linger选项,并设置延迟时间为1秒
    }
    int ret = 0;
    struct sockaddr_in address;
    bzero(&address,sizeof(address)); //清零
    address.sin_family = AF_INET;
    // 主机字节序向网络字节序转换 htonl(ip) htons(端口)    网络向主机转换  ntohl(ip) ntohs(端口)
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    int flag= 1;
    setsockopt(m_listenfd,SOL_SOCKET,SO_REUSEADDR,&flag,sizeof(flag)); //设置地址重用
    // 绑定 将套接字绑定到指定的地址和端口
    ret = bind(m_listenfd,(struct sockaddr*)&address,sizeof(address));
    assert(ret>=0);
    // 对地址进行监听 等待连接最大长度为5
    ret = listen(m_listenfd,5);
    assert(ret>=0);
    // 设置超时时间
    utils.init(TIMESLOT);

    // epoll创建内核时间表
    epoll_event events[MAX_EVENT_NUMBER];

    m_epollfd = epoll_create(5);  //创建epoll实例 最大支持事件数5
    assert(m_epollfd!=-1);

    // 将监听描述符放入epoll中进行监听
    utils.addfd(m_epollfd,m_listenfd,false,m_LISTENTrimode);

    http_conn::m_epollfd = m_epollfd;
    // socketpair用于创建一对相互连接的socket函数，它在本地进程之间创建一个全双工的通信通道 p[0]读端 p[1]写端
    ret = socketpair(PF_UNIX,SOCK_STREAM,0,m_pipefd);  
    assert(ret!=-1);
    utils.setnonblocking(m_pipefd[1]);
    // 利用epoll对m_pipefd[0]进行监听
    utils.addfd(m_epollfd,m_pipefd[0],false,0);
    // 对SIGPIPE信号设置忽略处理   SIGPIPE 通常发生在进程向已经关闭的管道写入数据
    utils.addsig(SIGPIPE,SIG_IGN);
    // 设置收到定时器信号所做出的处理
    utils.addsig(SIGALRM,utils.sig_handler,false);
    // 设置收到中的信号所做出的处理
    utils.addsig(SIGTERM,utils.sig_handler,false);
    
    // 定时器
    alarm(TIMESLOT);

    // 工具类，信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}
//  设置定时器
void Webserver::timer(int connfd,struct sockaddr_in client_address)
{
    // 此步相当于根据connfd将users[connfd]中存储的http_conn对象初始化
    users[connfd].init(connfd,client_address,m_root,m_CONNTrigmode,m_close_log,m_user,m_passWord,m_databaseName);

    // 初始化client_data数据
    // 创建定时器，设置回调函数和超时时间，绑定用户数据 将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    // 当前时间
    time_t cur = time(NULL);
    timer->expire = cur+3*TIMESLOT;
    // 此处出错 设置为users_timer->timer = timer 了
    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer);

}

// 若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void Webserver::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3*TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);
    LOG_INFO("%s","adjust timer once");
}

// 将sockfd从epoll监听中移除并关闭连接
void Webserver::deal_timer(util_timer *timer,int sockfd)
{
    // 回调函数中将sockfd从epoll中移除 然后关闭sockfd
    timer->cb_func(&users_timer[sockfd]);
    if(timer)
    {
        // 从计时器容器中删除
        utils.m_timer_lst.del_timer(timer);
    }
    LOG_INFO("close fd %d",users_timer[sockfd].sockfd);

}
// 当有新的客户端加入时，与新的客户端建立连接
bool Webserver::dealclientdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    if(0==m_LISTENTrimode)
    {
        int connfd = accept(m_listenfd,(struct sockaddr*)&client_address,&client_addrlength);
        if(connfd<0)
        {
            LOG_ERROR("%s:errno is:%d","accept error",errno);
            return false;
        }
        // 当前服务器的连接数量大于最大值
        if(http_conn::m_user_count>=MAX_FD)
        {
            utils.show_error(connfd,"Internal server busy");
            LOG_ERROR("%S","Internal server busy");
            return false;
        }
        // 设置定时器
        timer(connfd,client_address);
    }else{
        while (1)
        {
            int connfd = accept(m_listenfd,(struct sockaddr*)&client_address,&client_addrlength);
            if(connfd<0)
            {
                LOG_ERROR("%s:errno is:%d","accept error",errno);
                break;
            }
            if(http_conn::m_user_count>=MAX_FD)
            {
                utils.show_error(connfd,"Internal server busy");
                LOG_ERROR("%s","Internal server busy");
                break;
            }
            timer(connfd,client_address);
        }
        return false;
        
    }
    return true;
}

// 处理信号
bool Webserver::dealwithsignal(bool &timeout,bool &stop_server)
{
    int ret =0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0],signals,sizeof(signals),0); //从管道中读取数据
    if(ret==-1)
    {
        return false;
    }else if(ret ==0)
    {
        return false;
    }else{
        for(int i=0;i<ret;++i)
        {
            switch (signals[i])
            {
            case SIGALRM:
                timeout = true;
                break;
            case SIGTERM:
                stop_server = true;
                break;
            
            }
        }
    }
    return true;
}

void Webserver::dealwithread(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;

    // reactor 
    if(1==m_actormodel)
    {
        if(timer)
        {
            adjust_timer(timer);
        }
        // 若检测到读事件，将该事件放入请求队列   users数组根据将sockfd作为下标存储 http_conn对象 因此users+sockfd = users[sockfd];
        // 0表示读  1表示写
        // 先通知 然后让子线程进行读写操作
        m_pool->append(users+sockfd,0);
        while (true)
        {
            if(1==users[sockfd].improv){
                if(1==users[sockfd].timer_flag){
                    deal_timer(timer,sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
        
    }else{
        // proactor
        // 先处理操作 处理完成之后再通知子线程 让其完成相应的逻辑处理
        if(users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)",inet_ntoa(users[sockfd].get_address()->sin_addr));
            // 读事件完成后通知
            m_pool->append_p(users+sockfd);
            if(timer){
                adjust_timer(timer);
            }
        }else{
            deal_timer(timer,sockfd);
        }
    }
}
void Webserver::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;
    // reactor
    if(1==m_actormodel)
    {
        if(timer){
            adjust_timer(timer);
        }
        m_pool->append(users+sockfd,1);
        while (true)
        {
            if(1==users[sockfd].improv)
            {
                if(1==users[sockfd].timer_flag)
                {
                    deal_timer(timer,sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
        
    }else
    {
        // proactor 
        // 写事件
        if(users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)",inet_ntoa(users[sockfd].get_address()->sin_addr));
            if(timer)
            {
                adjust_timer(timer);
            }
        }else{
            deal_timer(timer,sockfd);
        }
    }
}

void Webserver::eventLoop()
{
    bool timeout = false;
    bool stop_server = false;
    while(!stop_server)
    {
        int number = epoll_wait(m_epollfd,events,MAX_EVENT_NUMBER,-1);
        if(number<0 && errno!=EINTR)
        {
            LOG_ERROR("%S","epoll fail");
            break;
        }
        for(int i=0;i<number;i++){
            int sockfd =events[i].data.fd;
            // 处理新到的客户连接
            if(sockfd ==m_listenfd)
            {
                // 与客户端进行连接
                bool flag = dealclientdata();
                // 连接失败 继续循环
                if(false==flag)
                {
                    continue;
                }
            } else if(events[i].events &(EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            } //监听到管道中的信号 处理信号
            else if(sockfd ==m_pipefd[0] && (events[i].events &EPOLLIN))
            {
                bool flag = dealwithsignal(timeout,stop_server);
                if(flag ==false)
                {
                    LOG_ERROR("%s","dealclientdata failure");
                }
            }
            else if(events[i].events &EPOLLIN) //读
            {
                dealwithread(sockfd);
            }
             else if (events[i].events & EPOLLOUT)
            {
                dealwithwrite(sockfd);
            }
        }
        if(timeout)
        {
            utils.timer_handler();
            LOG_INFO("%S","timer tick");
            timeout = false;
        }
    }
}