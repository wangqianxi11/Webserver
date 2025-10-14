
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <functional>
class ThreadPool {
public:
    explicit ThreadPool(size_t threadCount = 8): pool_(std::make_shared<Pool>()) {
            for(size_t i = 0; i < threadCount; i++) {
                std::thread([pool = pool_] {
                    std::unique_lock<std::mutex> locker(pool->mtx); // 获取互斥锁
                    while(true) {
                        if(!pool->tasks.empty()) { // 如果任务队列非空
                            auto task = std::move(pool->tasks.front()); // 取出任务
                            pool->tasks.pop(); // 移除任务
                            locker.unlock(); // 释放锁，允许其他线程访问队列
                            task(); // 执行任务
                            locker.lock(); // 重新获取锁
                        } 
                        else if(pool->isClosed) break; // 如果线程池关闭，退出循环
                        else pool->cond.wait(locker); // 否则，等待新任务
                    }
                }).detach(); // 线程分离，主线程不会等待工作线程结束，如果主线程退出，工作线程仍会继续运行（造成资源泄露）
            }
    }

    ThreadPool() = default;

    ThreadPool(ThreadPool&&) = default;
    
    ~ThreadPool() {
        if(static_cast<bool>(pool_)) {
            {
                std::lock_guard<std::mutex> locker(pool_->mtx); // 
                pool_->isClosed = true; // 设置关闭标志
            }
            pool_->cond.notify_all(); // 通知所有等待的线程，完成当前任务后自行退出
        }
    }

    template<class F>
    void AddTask(F&& task) {
        {
            std::lock_guard<std::mutex> locker(pool_->mtx); // 加锁，保护任务队列安全
            pool_->tasks.emplace(std::forward<F>(task)); // 构造任务队列，保证完美转发，不多拷贝
        }
        pool_->cond.notify_one(); // 唤醒一个等待线程来处理
    }

private:
    struct Pool {
        std::mutex mtx; // 互斥锁
        std::condition_variable cond; // 条件变量，用于线程间的任务通知机制
        bool isClosed; // 标志位，线程池是否关闭
        std::queue<std::function<void()>> tasks; // 任务队列
    };
    std::shared_ptr<Pool> pool_;
};


#endif //THREADPOOL_H