#include "http_conn.h"
#include <mysql/mysql.h>
#include <fstream>

// 定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string,string> users;

void http_conn::initmysql_result(connection_pool *connPool)
{
    // 先从连接池中取一个连接
    MYSQL *mysql = NULL;  //传入传出参数
    connectionRAII mysqlcon(&mysql,connPool);
    // 在user表中检索username,passwd数据
    if(mysql_query(mysql,"SELECT username,passwd FROM user"))
    {
        // 日志输出
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }
    // 从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);
    // 返回结果集中的列数
    int num_fields = mysql_num_fields(result);
    // 返回所有字段结构数组
    MYSQL_FIELD * fields = mysql_fetch_field(result);
    // 从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;

    }
    
}

// 对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd,F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}

/*
将内核事件表注册为读事件，ET模式，选择开启EPOLLONESHOT
通过开启EPOLLONESHOT可以确保一个线程处理socket时其他线程无法处理
EPOLLET将EPOLL设置为边缘触发
EPOLLRDHUP 当某个socket关闭连接时，系统会将该套接字上发生的事件中包含上EPOLLRDHUB并通知epoll等待程序 对端关闭连接
EPOLLIN表示有数据可读

*/  
void addfd(int epollfd,int fd,bool one_shot,int TRIGMode)
{
    epoll_event event;  //epoll事件数组
    event.data.fd = fd;
    if(TRIGMode==1)
    {
        event.events = EPOLLIN |EPOLLET |EPOLLRDHUP;
    }else{
        event.events = EPOLLIN |EPOLLRDHUP;
    }
    if(one_shot)
    {
        event.events|=EPOLLONESHOT;
    }
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    setnonblocking(fd);
}
// 从内核时间表删除描述符
void removefd(int epollfd,int fd)
{
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

// 将事件重置为EPOLLONESHOT
void modfd(int epollfd,int fd,int ev,int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    if(TRIGMode==1)
    {
        event.events = ev|EPOLLET|EPOLLONESHOT|EPOLLRDHUP;
    }else
    {
        event.events = ev|EPOLLONESHOT|EPOLLRDHUP;
    }
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;


// 关闭连接
void http_conn::close_conn(bool real_close)
{
    if(real_close &&(m_sockfd!=-1))
    {
        printf("close%d\n",m_sockfd);
        removefd(m_epollfd,m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// 初始化连接
void http_conn::init(int sockfd,const sockaddr_in &addr,char *root,int TRIGMode,int close_log,string user,string passwd,string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;
    addfd(m_epollfd,sockfd,true,m_TRIGMode);
    m_user_count++;

    // 当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;
    strcpy(sql_user,user.c_str());   //将user转换为字符串并拷贝进sql_user
    strcpy(sql_passwd,passwd.c_str());
    strcpy(sql_name,sqlname.c_str());

    init();
}
// 初始化新接受的连接
// check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf,'\0',READ_BUFFER_SIZE);
    memset(m_write_buf,'\0',WRITE_BUFFER_SIZE);
    memset(m_real_file,'\0',FILENAME_LEN);
}
/*
从状态机，用于分析出一行内容
 返回值为行的读取状态，有LINE_OK(完整读取一行)：LINE_BAD(报文语法错误) ，LINE_OPEN(读取的行不完整)
http报文中每一行以\r\n作为结束字符 空行则仅仅是\r\n
从状态机负责读取buffer中的数据，将每行数据的末尾\r\n置为\0\0

从状态机从m_read_buf中逐字读取，判断当前字节是否为\r
    接下来的字符\n,将\r\n修改成\0\0将m_checked_idx指向下一行的开头 返回LINE_OK
    接下来到了buffer的尾端，则表示buffer还需要继续接受 返回LINE_OPEN
    否则，表示语法错误 返回LINE_BAD
当前字符不是\r,判断是不是\n 
    如果前一个字符是\r，则将\r\n修改成\0\0
如果当前字符既不是\r也不是\n 返回LINE_OPEN
    表示接受不完整继续接受

*/ 

http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for(;m_checked_idx<m_read_idx;++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if(temp=='\r')
        {
            // 判断当前字符是否是回车符 如果是回车符 判断是否读到了缓冲区最后 如果是返回LINE_OPEN
            if((m_checked_idx+1)==m_read_idx)
            {
                return LINE_OPEN;
            }else if(m_read_buf[m_checked_idx+1]=='\n')
            {
                // 判断下一个字符是否是换行符
                m_read_buf[m_checked_idx++]='\0';
                m_read_buf[m_checked_idx++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }else if(temp=='\n')
        {
            if(m_checked_idx>1 &&m_read_buf[m_checked_idx-1]=='\r')
            {
                m_read_buf[m_checked_idx-1]='\0';
                m_read_buf[m_checked_idx] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }

    }
    return LINE_OPEN;
}


// 循环读取客户端数据直到无数据可读或者对方关闭
// 非阻塞的ET模式需要一次性读完数据
bool http_conn::read_once()
{
    if(m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;
    // LT水平触发读取数据
    if(m_TRIGMode==0)
    {
        bytes_read = recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
        m_read_idx+=bytes_read;
        // 读取失败
        if(bytes_read<=0){
            return false;
        }
        return true;
    }else
    {
        // 需要一次性读完
        while(true)
        {
            // m_read_buf+m_read_idx 表示读数组开始地址   READ_BUFFER_SIZE-m_read_idx 表明缓冲区剩余空间
            bytes_read = recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
            if(bytes_read==-1)
            {
                if(errno==EAGAIN ||errno ==EWOULDBLOCK) //EAGAIN表示资源暂时不可用 需要重试 EWOULDBLOCK表示操作会阻塞，但在非阻塞模式下它表示当前资源不可用
                    break;
                return false;
            }else if(bytes_read ==0)
            {
                return false;
            }
            m_read_idx+=bytes_read;

        }
        return true;
    }

}

// 解析http请求行，获得请求方法，目标url及版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text,"\t");
    if(!m_url){
        return BAD_REQUEST;
    }
    *m_url++='\0';  //在url最后加上\0

    char *method = text;
    if(strcasecmp(method,"GET")==0)
    {
        m_method = GET;
    }else if(strcasecmp(method,"POST")==0)
    {
        m_method = POST;
        cgi = 1;
    }else
    {
        // 此处可以扩展其他请求
        return BAD_REQUEST;
    }
    // m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有
    // 将m_url向后偏移，通过查找继续跳过空格和\t字符,指向请求资源的第一个字符
    m_url+=strspn(m_url,"\t");
    // 使用与判断请求的方式判断版本号
    m_version = strpbrk(m_url,"\t");
    if(!m_version)
    {
        return BAD_REQUEST;
    }
    *m_version++='\0';
    m_version+=strspn(m_version,"\t");
    // 仅支持HTTP/1.1
    if(strcasecmp(m_version,"HTTP/1.1")!=0)
    {
        return BAD_REQUEST;
    }
    if(strncasecmp(m_url,"http://",7)==0)
    {
        m_url+=7;
        m_url = strchr(m_url,'/');
    }
     if(strncasecmp(m_url,"https://",8)==0)
    {
        m_url+=8;
        m_url = strchr(m_url,'/');
    }
    if(!m_url ||m_url[0]!='/')
        return BAD_REQUEST;
    // 当url为/时，显示欢迎界面
    if(strlen(m_url)==1)
        strcat(m_url,"judge.html");
    // 请求行处理完，将主状态机状态转移到请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析http请求的一个头部信息 
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 判断是否为空行还是请求头 如果是空行进而判断content-length是否为0 如果不是0表明是post请求 状态转移CHECK_STATE_CONTENT 否则说明是get请求
    if(text[0]=='\0'){
        if(m_content_length!=0){
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }else if(strncasecmp(text,"Connection:",11)==0)
    {
        text+=11;
        text +=strspn(text,"\t");
        if(strcasecmp(text,"keep-alive")==0)
        {
            m_linger = true;
        }
    }
    else if(strncasecmp(text,"Content-length:",15)==0)
    {
        text+=15;
        text+=strspn(text,"\t");
        m_content_length = atol(text);
    }
    else if(strncasecmp(text,"Host:",5)==0)
    {
        text+=5;
        text+=strspn(text,"\t");
        m_host = text;
    }else
    {
        // 日志信息
        LOG_INFO("oop!unknow header: %s", text);
        
    }
    return NO_REQUEST;

}

// 判断http请求是否被完整的读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    // 判断buffer中是否读了消息体
    if(m_read_idx>=(m_content_length+m_checked_idx)){
        text[m_content_length]='\0';
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;
    while((m_check_state==CHECK_STATE_CONTENT && line_status==LINE_OK) ||((line_status=parse_line())==LINE_OK))
    {
            text = get_line();
            m_start_line = m_checked_idx;
            LOG_INFO("%s",text);
            switch (m_check_state)
            {
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if(ret ==BAD_REQUEST)
                    return BAD_REQUEST;
                break;
            }  
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if(ret==BAD_REQUEST)
                    return BAD_REQUEST;
                else if(ret==GET_REQUEST)
                {
                    return do_request();
                }
                break;
            }   
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if(ret==GET_REQUEST)
                    return do_request();
                line_status = LINE_OPEN;
                break;
            }
            default:
                return INTERNAL_ERROR;
            
            }
    }
    return NO_REQUEST;

}

// 响应报文生成
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file,doc_root);
    int len = strlen(doc_root);
    const char *p=strrchr(m_url,'/');  //strrchr  用于在一个字符串中查找最后一次出现指定字符的位置

    // 处理cgi post请求
    if(cgi==1 &&(*(p+1)=='2' ||*(p+1)=='3'))
    {
        //根据标志判断是登录还是注册检测
        char flag = m_url[1];

        char*m_url_real = (char*)malloc(sizeof(char)*200);
        strcpy(m_url_real,"/");
        strcat(m_url_real,m_url+2);
        strncpy(m_real_file+len,m_url_real,FILENAME_LEN-len-1);

        // 将用户名和密码提取出来
        char name[100],password[100];
        int i;
        for(i=5;m_string[i]!='&';++i)
        {
            name[i-5] = m_string[i];
        }
        name[i-5]='\0';

        int j=0;
        for(i=i+10;m_string[i]!='\0';++i,++j)
            password[j]=m_string[i];
        password[j]='\0';

        if(*(p+1)=='3')
        {
            // 如果是注册，先检测数据库中是否有重名
            // 没有重名，进行增加数据
            char *sql_insert = (char*)malloc(sizeof(char)*200);
            strcpy(sql_insert,"insert into user(username,passwd) values(");
            strcat(sql_insert,"'");
            strcat(sql_insert,name);
            strcat(sql_insert,"','");
            strcat(sql_insert,password);
            strcat(sql_insert,"')");
            if(users.find(name)==users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql,sql_insert);
                users.insert(pair<string,string>(name,password));
                m_lock.unlock();
                if(!res)
                {
                    strcpy(m_url,"/log.html");
                }else
                {
                    strcpy(m_url,"registerError.html");
                }

            }else
            {
                strcpy(m_url,"/registerError.html");
            }
        
        } else if(*(p+1)=='2') //如果是登录直接判断  若浏览器端输入的用户名和密码可以查到 返回1 否则返回0
        {
            if(users.find(name)!=users.end() && users[name]==password)
            {
                strcpy(m_url,"/welcome.html");
            }else{
                strcpy(m_url,"/logError.html");
            }
        }

    }

    // get请求
    if(*(p+1)=='0')
    {
        char *m_url_real = (char*)malloc(sizeof(char)*200);
        strcpy(m_url_real,"/register.html");
        strncpy(m_real_file+len,m_url_real,strlen(m_url_real));

        free(m_url_real);
    }else if(*(p+1) =='1')
    {
        char *m_url_real = (char*) malloc(sizeof(char)*200);
        strcpy(m_url_real,"/log.html");
        strncpy(m_real_file+len,m_url_real,strlen(m_url_real));

        free(m_url_real);

    }else if(*(p+1)=='5')
    {
        char *m_url_real = (char*) malloc(sizeof(char)*200);
        strcpy(m_url_real,"/picture.html");
        strncpy(m_real_file+len,m_url_real,strlen(m_url_real));

        free(m_url_real);

    }else if(*(p+1)=='6')
    {
        char *m_url_real = (char*) malloc(sizeof(char)*200);
        strcpy(m_url_real,"/video.html");
        strncpy(m_real_file+len,m_url_real,strlen(m_url_real));
        free(m_url_real);

    }else if(*(p+1)=='7')
    {
        char *m_url_real = (char*) malloc(sizeof(char)*200);
        strcpy(m_url_real,"/fans.html");
        strncpy(m_real_file+len,m_url_real,strlen(m_url_real));
        free(m_url_real);

    }
    else if(*(p+1)=='8')
    {
        char *m_url_real = (char*) malloc(sizeof(char)*200);
        strcpy(m_url_real,"/upload.html");
        strncpy(m_real_file+len,m_url_real,strlen(m_url_real));
        free(m_url_real);
    }
    else{
        strncpy(m_real_file+len,m_url,FILENAME_LEN-len-1);
    }
    if(stat(m_real_file,&m_file_stat)<0)
    {
        return NO_RESOURECE;
    }
    // 如果文件中没有设置其他用户读取权限 S_IROTH
    if(!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;
    // 用于检查文件是否为目录 S_ISDIR
    if(S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;
    int fd = open(m_real_file,O_RDONLY);  //读写权限打开
    //内存映射  PROT_READ 对内存区域进行读取操作权限 不允许写操作  MAP_PRIVATE 每个进程都将获得一份独立的拷贝，对于内存映射区域的修改不会影响到原文件  共享文件但不共享修改
    m_file_address = (char*)mmap(0,m_file_stat.st_size,PROT_READ,MAP_PRIVATE,fd,0);  
    close(fd);
    return FILE_REQUEST;

}
// 解除内存映射
void http_conn::unmap()
{
    if(m_file_address)
    {
        munmap(m_file_address,m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool http_conn::write()
{
    int temp = 0;
    if(bytes_to_send ==0)
    {
        modfd(m_epollfd,m_sockfd,EPOLLIN,m_TRIGMode);
        init();
        return true;
    }   
    while (1)
    {
        temp = writev(m_sockfd,m_iv,m_iv_count);
        if(temp<0)
        {
            if(errno==EAGAIN)  //EAGAIN 表示当前资源不可用 稍后可能变为可用状态
            {
                modfd(m_epollfd,m_sockfd,EPOLLOUT,m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }
        bytes_have_send+=temp;
        bytes_to_send-=temp;
        /*
        iov_base:表示数据缓冲区的起始地址
        iov_len:表示数据缓冲区的长度
            已经发送的字节数大于等于第一个iov的长度  意味着第一个iov元素中的数据已经全部发送完毕  为了处理剩余未发送的数据 需要将第一个iov元素长度设置为0
            然后使用m_file——address和偏移量确定起始地址 将第二个iov元素的基础地址设置为剩余数据的起始地址
        */
        if(bytes_have_send>=m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address+(bytes_have_send-m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }else
        {
            // iov元素中的数据还未完全发送完毕  需要将iov元素的基础地址设置为m_write_buf加上已经发送的字节偏移量 这样可以继续发送缓冲区中的数据
            // 将iov的长度设置为iov元素减去已经发送的字节数 表示剩余可发送的数据长度
            m_iv[0].iov_base = m_write_buf+bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len-bytes_have_send;
        }
        // epollin 连接到达 有数据来临  epollout 有数据要写
        if(bytes_to_send<=0)
        {
            unmap();
            modfd(m_epollfd,m_sockfd,EPOLLIN,m_TRIGMode);
            if(m_linger)
            {
                init();
                return true;
            }else{
                return false;
            }
        }
    }
    
}
bool http_conn::add_response(const char* format,...)
{
    // 如果当前已经写入的字符大于等于缓冲区大小   缓冲区已经满了无法写入
    if(m_write_idx>=WRITE_BUFFER_SIZE)
    {
        return false;
    }
    va_list arg_list;  //va_list是一个在C语言中处理变长参数的类型
    va_start (arg_list,format); //这一步是为了访问传递给函数的可变参数
    /*
    m_write_buf+m_write_idx:表示缓冲区中待写入的字符串的起始位置
    WRITE_BUFFER_SIZE-1-m_write_idx：表示缓冲区sheng
    arg_list 用于访问可变参数列表的变量
    vsnprintf：根据format和可变参数列表将格式化后的数据写入缓冲区
    */
    int len = vsnprintf(m_write_buf+m_write_idx,WRITE_BUFFER_SIZE-1-m_write_idx,format,arg_list);
    if(len>=(WRITE_BUFFER_SIZE-1-m_write_idx))
    {
        // 写入的字符大于等于剩余可用空间
        va_end(arg_list);
        return false;
    }
    m_write_idx +=len;
    va_end(arg_list);
    LOG_INFO("request:%s",m_write_buf);
    return true;
}
bool http_conn::add_status_line(int status,const char* title)
{
    return add_response("%s %d %s\r\n","HTTP/1.1",status,title);
}
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() && add_blank_line();
}
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n",content_len);
}
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n","text/html");
}
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n",(m_linger==true)?"keep-alive":"close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s","\r\n");
}
bool http_conn::add_content(const char *content)
{
    return add_response("%s",content);
}
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500,error_500_title);
        add_headers(strlen(error_500_form));
        if(!add_content(error_500_form))
            return false;
        break;
    }
      
    case BAD_REQUEST:
    {
        add_status_line(404,error_404_title);
        add_headers(strlen(error_404_form));
        if(!add_content(error_404_form))
            return false;
        break;

    }
     case FORBIDDEN_REQUEST:
     {
        add_status_line(403,error_404_title);
        add_headers(strlen(error_403_form));
        if(!add_content(error_403_form))
            return false;
        break;  
     }
    case FILE_REQUEST:
    {
        add_status_line(200,ok_200_title);
        if(m_file_stat.st_size!=0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx+m_file_stat.st_size;
            return true;
        }else{
            const char * ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if(!add_content(ok_string))
            {
                return false;
            }
        }
        
    }  
    default:
        return false;
        
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if(read_ret==NO_REQUEST)
    {
        modfd(m_epollfd,m_sockfd,EPOLLIN,m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if(!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd,m_sockfd,EPOLLOUT,m_TRIGMode);
}