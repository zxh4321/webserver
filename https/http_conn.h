#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

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
#include <string>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>
#include <mysql/mysql.h>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

using namespace std;

class http_conn
{

public:
    static const int FILENAME_LEN = 200;        //文件名长度
    static const int READ_BUFFER_SIZE=2048;     //读缓冲区大小
    static const int WRITE_BUFFER_SIZE = 1024;  //写缓冲区大小
    enum METHOD
    {//报文请求方法
        GET=0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    // 主状态机的状态
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE=0,   //请求行
        CHECK_STATE_HEADER,          //请求头
        CHECK_STATE_CONTENT          //内容
    };
    // 报文解析的结果
    enum HTTP_CODE
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURECE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    //从状态机状态
    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    http_conn(){}
    ~http_conn(){}
public:
// 初始化套接字地址，函数内部会调用私有方法init
    void init(int sockfd,const sockaddr_in &addr,char*,int,int,string user,string passwd,string sqlname);  
    // 关闭http连接
    void close_conn(bool real_close=true);

    void process();
    // 读取浏览器端发来的全部数据
    bool read_once();
    // 响应报文写入函数
    bool write();
    sockaddr_in *get_address(){
        return &m_address;
    }
    // 同步线程初始化数据库读取表
    void initmysql_result(connection_pool *connPool);
    // CGI使用线程池初始化数据库表
    // 是否有定时器
    int timer_flag;
    // 任务是否处理完成
    int improv;
private:
    void init();
    // 从m_read_buf读取，并处理请求报文
    HTTP_CODE process_read();
    // 向m_write_buf写入响应报文数据
    bool process_write(HTTP_CODE ret);
    // 从主状态机解析报文中的请求头数据
    HTTP_CODE parse_request_line(char *text);
    // 从主从状态机中解析报文中请求头的数据
    HTTP_CODE parse_headers(char* text);
    // 从主状态机中解析报文中请求内容
    HTTP_CODE parse_content(char* text);
    // 生成响应报文
    HTTP_CODE do_request();
    // m_start_line 是已经解析过的字符
    // get_line()用于将指针向后偏移，指向未处理的字符
    char* get_line(){return m_read_buf+m_start_line;};

    // 从状态机读取一行分析是请求报文的哪一部分
    LINE_STATUS parse_line();

    void unmap();
    // 根据响应报文格式，生成对应8个部分，以下函数由do_request调用
    bool add_response(const char* format,...);
    bool add_content(const char* content);
    bool add_status_line(int status,const char* title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();
public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL *mysql;
    int m_state;
private:
    int m_sockfd;
    sockaddr_in m_address;  //用于表示IPV4地址和端口的结构体
    char m_read_buf[READ_BUFFER_SIZE]; //存储读取的请求报文数据
    char m_read_idx;        //缓冲区中m_read_buf中数据的最后一个字节的下一个位置
    long m_checked_idx;     //m_read_buf读取的位置的m_checked_idx
    int m_start_line;       //m_read_buf中已经解析的字符个数

    // 存储发出的相应报文数据
    char m_write_buf[WRITE_BUFFER_SIZE];
    // 指示buffer中的长度
    int m_write_idx;

    // 主状态机的状态
    CHECK_STATE m_check_state;
    // 请求方法
    METHOD m_method;

    // 以下为解析请求报文中对应的6个变量
    // 存储读取文件的名称
    char m_real_file[FILENAME_LEN];
    char*m_url;
    char *m_version;
    char *m_host;
    int m_content_length;
    bool m_linger;

    // 读取服务器上的文件地址
    char *m_file_address;
    struct stat m_file_stat;  //文件系统中的文件属性结构
    struct iovec m_iv[2];  //io向量机制iovec  用于描述一个数据缓冲区 通常与readv 和writev系统调用一起使用
    int m_iv_count;
    int cgi;                //是否启用的post
    char *m_string;         //存储请求头数据
    int bytes_to_send;      //剩余发送字节数
    int bytes_have_send;    //已发送字节数

    char * doc_root;
    map<string,string> m_users;
    int m_TRIGMode;
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];


};

#endif