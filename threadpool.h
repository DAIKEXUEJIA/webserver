//头文件和实现写在一起，真实开发应该分开

//防止头文件的重复包含
#ifndef THREADPOOL_H
#define THREADPOOL_H


#include <pthread.h>
#include "locker.h"//自己定义的要用双引号
#include <exception>//抛出异常的头文件'
#include <stdio.h> //printf
#include <list>

//设计线程池类，这里为了通用，用模板的方式指定
//线程池定义成模板类，是为了实现代码的复用

//通过生产者-消费者模型来实现线程同步。线程同步的主要目的是防止由于多个线程同时访问和修改共享数据，而引起的数据不一致和竞争条件。
//线程同步作用对象是工作队列


template<typename T>//模板参数T就是任务类
class threadPool{
public:
    //构造函数，线程数目是8,为什么是8个，因为本项目是IO密集型服务器而不是计算密集型
    threadPool(int thread_num=8,int max_requests=10000);//不指定的话就是8和10000
    //析构函数
    ~threadPool();
    //往工作队列里面添加任务，类型是T
    bool append(T* request);
        //实现在下面这里只是声明

private:
//构造函数里，子线程需要执行的函数worker，必须是静态函数！
    static void* worker(void* arg);//参数void* 泛型编程
    void run(); //run函数是线程池中每个线程的核心工作循环。
//它持续检查工作队列，等待有任务到来。一旦队列中有任务，run函数会将其取出并执行。这个过程包括等待队列中的任务（通过信号量），从队列中取出任务，并执行这些任务。

private:
    //线程的数量
    int m_thread_num;
    //线程池数组，大小为m_thread_number（数组容器放置线程）
    pthread_t* m_threads;
    //请求队列中，最多允许的等待处理的请求数量
    int m_max_requests;
    //放置任务的请求队列list，里面装的是任务类，类型是T
    std::list <T*> m_workqueue;

    //互斥锁  (队列是所有线程共享的，所以需要队列互斥锁)
    locker m_queuelocker;
    //信号量 （判断是否有任务需要处理）
    sem m_queuestat;

    //是否结束线程
    bool m_stop;//判断是否结束的标志位


};

//在下面单独写实现，需要标出来
template<typename T>
threadPool<T>::threadPool(int thread_num,int max_requests)://冒号对成员进行初始化，这是初始化列表
    m_thread_num(thread_num),m_max_requests(m_max_requests),m_stop(false),m_threads(NULL){
        //构造函数里面初始化
        if((thread_num<=0)||(max_requests<=0))//如果传进来的数据错误，抛出异常
            throw std::exception();

        //把线程池数组创建出来，new一个p_thread_t类型的数组，大小就是刚才传进来的数量
        m_threads = new pthread_t[m_thread_num];

        if(!m_threads){//如果创建没成功，抛出异常
            throw std::exception();
        }
        //创建thread_num个线程，并且将他们设置成线程脱离（避免父线程的影响）
        for(int i=0;i<thread_num;i++){
            printf("create the %dth thread\n",i);

            //第一个参数传递pthread_t类型的指针，直接用m_threads+i（数组名称就是数组第一个元素的指针）
            //第三个参数是线程执行的函数，worker必须是静态函数！
            //加上这个if是已经创建完了，再进行判断的意思，如果创建失败抛出异常
            if(pthread_create(m_threads+i,NULL,worker,this)!=0){
                //失败就delete掉数组
                delete []m_threads;
                throw std::exception();//抛出异常
            }
            //创建成功，设置线程分离,传入线程池数组里面的线程编号
            if(pthread_detach(m_threads[i])){//假如失败抛出异常，man pthread_detach查看返回值
                delete []m_threads;
                throw std::exception();
            }
            //构造函数结束，构造函数就是初始化数据并且设置线程分离
        }
    }

//析构函数定义，析构函数的定义应该是类模板的一部分。
template <typename T>
threadPool<T>:: ~threadPool(){
    delete []m_threads;//释放掉线程池数组
    m_stop = true;//停止标志位置为true，根据这个判断要不要继续

}

//添加任务函数，将新的任务添加到工作队列（m_workqueue）中，以便线程池中的线程可以取出并执行这些任务
//它允许外部代码通过append将任务提交给线程池进行处理，而线程池内部的线程则负责实际执行这些任务
template<typename T>
bool threadPool<T>::append(T* request)//前面要放上返回值
{
    //上锁失败，会返回错误
    m_queuelocker.lock();//上锁，工作队列是公共的，要确保安全
    //不可继续添加的时候，返回错误（比如死锁）
    if(m_workqueue.size()>m_max_requests){
        m_queuelocker.unlock();//超过了最大数目，解锁
        return false;
    }

    //添加
    m_workqueue.push_back(request);
    //添加之后解锁
    m_queuelocker.unlock();
    //增加了一个任务，信号量增加
    m_queuestat.post();

    //添加成功返回true
    return true;
}

//线程池的worker函数
template<typename T>
void* threadPool<T>::worker(void* arg){
    //不需要再写static了
    //静态的函数，不能直接访问类的成员m_threads！
    //此处的解决方式是把类的this指针作为参数，传递到worker函数里面，并且使用static_cast将void*的类型还原！
    threadPool* pool = static_cast<threadPool*>(arg);
    //线程池需要运行，从工作队列里面取数据，执行任务,pool就是this指针！
    pool->run();
    return pool;//返回值其实没意义
}


/*
这个线程池的run函数，wait阻塞函数必须放在lock之前，不能放在lock之后.
如果run里面先Lock后wait，就会导致线程一创建就直接给工作队列加锁，然后进入wait阻塞状态（因为此时信号量=0，没有生产者来增加这个信号量），
导致主线程调用append的时候，无法访问工作队列增加任务，就会发生死锁。
会导致主线程一直在append位置阻塞不运行。我在vscode 打断点调试的时候发现，append之前的断点都是正常的，append位置的断点会报错。从而发现是append发送任务到工作线程的时候发生了死锁。
*/

template<typename T>
void threadPool<T>::run(){
    //线程一直循环直到stop为止
    while(!m_stop){
        //队列中取出任务
        m_queuestat.wait();//取任务之前，看信号量有没有值，没有值就阻塞，有值就-1再返回。如果有的话直接返回，信号量-1
        //操作工作队列之前一定要先加锁
        m_queuelocker.lock();
        if(m_workqueue.empty()){
            //假设请求队列是空的
            m_queuelocker.unlock();
            continue;//继续检测标志位
        }
        //请求非空
        T* request = m_workqueue.front();//工作队列的最前一个取出来，执行
        m_workqueue.pop_front();//弹出前面的元素，list容器本来就是双向链表！
        m_queuelocker.unlock();

        if(!request){//这里是判断取出的任务是不是空，防止在空指针上调用方法
            continue;
        }
        request->process();//每个任务对应一个process方法，每个任务类都应该实现这个方法，以定义它们的具体行为。
        /*
        process方法的定义
    独立于线程池: process方法通常定义在任务类（T类型）中，而不是直接在线程池类中定义。这样做的目的是为了使任务逻辑独立于线程池的实现，从而使线程池更通用和灵活。
    任务类的责任: 当创建一个具体的任务时，你需要定义这个任务类，并实现它的process方法。这个方法包含了任务的实际执行逻辑。
        
        */
    }
}


#endif