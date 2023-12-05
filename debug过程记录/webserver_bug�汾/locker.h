//真正开发是分开的，这里写一起了

//防止头文件的重复包含
#ifndef LOCKER_H
#define LOCKER_H

#include <pthread.h>//互斥锁在这个头文件里面
#include <exception>//抛出异常的头文件
#include <semaphore.h>//信号量的头文件


//线程同步机制封装类，面向对象思想,这里只是包装了一下接口

//互斥锁类
class locker{
public:
    //成员方法：构造函数，传入成员变量地址
    locker(){
        if(pthread_mutex_init(&m_mutex,NULL)!=0){//返回值不为0，就是出错了
            //出错就抛出异常
            throw std::exception();//直接抛出一个异常对象
        }
    }
    //析构函数
    ~locker(){
        //注意传入的时候都需要引用传递！也就是传入地址！
        pthread_mutex_destroy(&m_mutex);
    }
    //上锁函数
    bool lock(){
        //判断是不是==0意味着上锁成功
        return pthread_mutex_lock(&m_mutex)==0;
    }
    //解锁函数
    bool unlock(){
        return pthread_mutex_unlock(&m_mutex)==0;
    }
    //get函数返回成员指针（获取互斥量）
    pthread_mutex_t* get(){
        return &m_mutex;
    }
private:
//这个类实际上就是封装一下pthread_mutex_t这个类型的互斥锁
    pthread_mutex_t m_mutex;
};


//条件变量类
//怎么判断队列有数据？就要用到条件变量和信号量
class cond{
public:
    //构造函数
    cond(){
        if(pthread_cond_init(&m_cond,NULL)!=0){ //第二个参数NULL是属性
            //如果返回值不是0那么创建失败
            throw std::exception();//抛出异常
        }
    }
    //析构函数：销毁
    ~cond(){
        pthread_cond_destroy(&m_cond);
    }
    //封装条件变量wait函数
    bool wait(pthread_mutex_t* mutex){//条件变量要配合互斥锁使用，所以我们传入一个互斥锁
        //返回判断
        return pthread_cond_wait(&m_cond,mutex)==0;
    }
    //封装time_wait函数，超时时间
    bool timewait(pthread_mutex_t* mutex,struct timespec t){
        //返回判断
        return pthread_cond_timedwait(&m_cond,mutex,&t)==0;
    }
    //封装signal函数，让条件变量增加，唤醒一个/多个线程
    bool signal(){
        //返回判断
        return pthread_cond_signal(&m_cond)==0;
    }
    //broadcast：唤醒所有线程
    bool broadCast(){
        return pthread_cond_broadcast(&m_cond)==0;
    }


private:
//封装一个成员
    pthread_cond_t m_cond;

};


//信号量类
class sem{
public:
    //构造函数，构造函数就是去创建
    sem(){
        //一种构造方式，值一开始全部指定成0
        if(sem_init(&m_sem,0,0)!=0){
            throw std::exception();
        }
    }
    //另一种构造的方式，传入num
    sem(int num){
        if(sem_init(&m_sem,0,num)!=0){
            throw std::exception();
        }
    }
    //析构函数
    ~sem(){
        sem_destroy(&m_sem);
    }
    //等待信号量，wait功能
    bool wait(){
        return sem_wait(&m_sem)==0;
    }
    //增加信号量
    bool post(){
        return sem_post(&m_sem)==0;
    }

private:
    sem_t m_sem;

};



#endif
