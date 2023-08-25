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
    m_isasync = false;
}
Log::~Log()
{
    if(m_fp!=NULL)
    {
        fclose(m_fp);  //关闭打开的文件
    }
}
// 异步需要设置阻塞队列的长度，同步不需要
bool Log::init(const char *file_name,int close_log,int log_buf_size,int split_lines,int max_queue_size)
{
    // 如果设置了max_queue_size，则设置异步
    if(max_queue_size>=1){
        m_isasync = true;
        // 创建阻塞队列
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;
        // flush_log_thread为回调函数，这里表示创建线程异步写日志
        pthread_create(&tid,NULL,flush_log_thread,NULL);
    }
    m_close_log = close_log;
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf,'\0',m_log_buf_size);
    m_split_lines = split_lines;
    // 根据时间创建日志文件
    time_t t= time(NULL);
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    const char *p = strrchr(file_name,'/');
    char log_full_name[256]={0};
    if(p==NULL)
    {
        snprintf(log_full_name,255,"%d_%02d_%02d_%s",my_tm.tm_year+1900,my_tm.tm_mon+1,my_tm.tm_mday,file_name);
    }else{
        strcpy(log_name,p+1);
        strncpy(dir_name,file_name,p-file_name+1);
        snprintf(log_full_name,255,"%d_%02d_%02d_%s",my_tm.tm_year+1900,my_tm.tm_mon+1,my_tm.tm_mday,log_name);
    }
    m_today = my_tm.tm_mday;
    m_fp = fopen(log_full_name,"a");
    if(m_fp==NULL)
    {
        return false;
    }
    return true;
}
void Log::write_log(int level,const char *format,...)
{
    struct timeval now ={0,0};
    gettimeofday(&now,NULL);
    time_t t = now.tv_sec; //秒数
    struct tm *sys_tm = localtime(&t);  //tm表示日期和时间
    struct tm my_tm = *sys_tm;
    char s[16] = {0};
    switch (level)
    {
    case 0:
        strcpy(s,"[debug]:");
        break;
     case 1:
        strcpy(s,"[info]:");
        break;
     case 2:
        strcpy(s,"[warn]:");
        break;
     case 3:
        strcpy(s,"[erro]:");
        break;
    default:
        strcpy(s,"[info]:");
        break;
    }
    // 写入一个log，对m-count++,m_split_lines最大行数
    m_mutex.lock();
    m_count++;
    // 判断是否需要重新新建文件存储日志
    if(m_today !=my_tm.tm_mday ||m_count %m_split_lines ==0)
    {
        char new_log[256]={0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};

        snprintf(tail,16,"%d_%02d_%02d_",my_tm.tm_year+1900,my_tm.tm_mon+1,my_tm.tm_mday);

        if(m_today!=my_tm.tm_mday)
        {
            snprintf(new_log,255,"%s%s%s",dir_name,tail,log_name);
            m_today = my_tm.tm_mday;
            m_count =0;
        }else{
            snprintf(new_log,255,"%s%s%s.%lld",dir_name,tail,log_name,m_count/m_split_lines);
        }
        m_fp = fopen(new_log,"a");
    }
    m_mutex.unlock();
    //va_list和相关的宏来处理传递给函数的变长参数列表
    va_list valst;
    va_start(valst,format);
    string log_str;
    m_mutex.lock();

    // 写入具体的时间格式
    int n =snprintf(m_buf,48,"%d-%02d-%02d %02d:%02d:%02d.%06ld %s",my_tm.tm_year+1900,my_tm.tm_mon+1,my_tm.tm_mday,my_tm.tm_hour,my_tm.tm_min,my_tm.tm_sec,now.tv_usec,s);
    int m = vsnprintf(m_buf+n,m_log_buf_size-n-n,format,valst); //用于将格式化的字符串写入字符数组中，他类似于snprintf,但是可以接受一个va_list参数 可以处理变长的参数列表
    m_buf[n+m] = '\n';
    m_buf[n+m+1]='\0';
    log_str = m_buf;

    m_mutex.unlock();
    // 判断是否异步输出日志 阻塞队列是否已经满了，没有满加入队列中
    if(m_isasync && !m_log_queue->full())
    {
        m_log_queue->push(log_str);
    }else{
        m_mutex.lock();
        fputs(log_str.c_str(),m_fp);
        m_mutex.unlock();
    }
    va_end(valst);

}
void Log::flush()
{
    m_mutex.lock();
    fflush(m_fp);
    m_mutex.unlock();
}