#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
/*
线程池类
线程池主要分为三部分 任务队列 管理者线程 工作线程
*/
class threadpool
{
private:
// 工作线程运行函数，他不断从工作队列中取出任务并执行
    static void *worker(void *arg);
    void run();
public:
    threadpool(int actor,connection_pool *connPool,int thread_number = 8,int max_request = 10000);
    ~threadpool();
    bool append(T* request,int state);
    bool append_p(T* request);
private:
    int m_thread_number;    //线程池中的线程数量
    int m_max_requests;     //请求队列中允许的最大请求数
    pthread_t *m_threads;  //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue;//任务队列
    locker m_queuelocker;       //保护任务队列的锁
    sem m_queuestat;            //信号量
    connection_pool *m_connPool; //数据库连接池
    int m_actor_model;          //模式切换
};

template <typename T>
threadpool<T>::threadpool(int actor_model, connection_pool *connPool, int thread_number, int max_request)
    : m_actor_model(actor_model), m_connPool(connPool), m_thread_number(thread_number), m_max_requests(max_request), m_threads(NULL)
{
    if(thread_number<=0 || max_request<=0){
        throw std::exception();
    }
    // 创建用于存放线程的数组
    m_threads = new pthread_t[thread_number];
    if(!m_threads){
        throw std::exception();
    }
    // 创建工作线程
    for(int i=0;i<thread_number;i++){
        if(pthread_create(m_threads+i,NULL,worker,this)!=0){
            delete [] m_threads;
            throw std::exception();
        }
        // 设置线程可分离状态 当线程处于可分离状态时 该线程的资源会自动的释放
        if(pthread_detach(m_threads[i])!=0){
            delete [] m_threads;
            throw std::exception();
        }
    }
}
template <typename T>
threadpool<T>::~threadpool()
{
    delete [] m_threads;
}

template <typename T>
 bool threadpool<T>::append(T *request, int state)
{
    m_queuelocker.lock();
    // 请求队列大于 最大任务数
    if(m_workqueue.size()>=m_max_requests){
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();

    return true;
}

template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if(m_workqueue.size()>=m_max_requests){
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
template <typename T>
 void *threadpool<T>::worker(void *arg)
{
    // 进行类型的转换
    threadpool * pool = (threadpool*)arg;
    // 工作线程 任务队列中的任务进行处理
    pool->run();
    return pool;
}
template <typename T>
void threadpool<T>::run()
{
    
    while (true)
    {
        m_queuestat.wait();
        m_queuelocker.lock();
        if(m_workqueue.empty()){ //任务队列中没有任务 循环继续等待
            m_queuelocker.unlock();
            continue;
        }
        T* request= m_workqueue.front();  //从队列前面取出任务
    
        m_workqueue.pop_front();          //将取出的任务从队列中删除
        m_queuelocker.unlock();
        if(!request){
            continue;
        }
        if(1==m_actor_model){
            if(0==request->m_state){
    
                if(request->read_once()){
                    request->improv = 1;
                    connectionRAII mysqlcon(&request->mysql,m_connPool);
                    request->process();
                }else{
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }else{
                if(request->write()){
                    request->improv = 1;
                }else{
                    request->improv =1;
                    request->timer_flag = 1;
                }
            }
        }else{
            connectionRAII mysqlcon(&request->mysql,m_connPool);
            request->process();
        }
    }
    
}
#endif