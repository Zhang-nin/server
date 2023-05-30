// #include <time.h>
// #include <stdio.h>
// #include <sys/time.h>
// #include<mutex>
// #include<deque>
// #include<condition_variable>

// std::mutex mtx;
// std::condition_variable cv;

// int main(void)
// {
//     // 它不仅可以使用在简单的临界区代码段的互斥操作中，还能用在函数调用过程中
//     std::unique_lock<std::mutex> locker(mtx);
//     cv.wait(locker);//#1.使线程进入等待状态 #2.1ck.unlock可以把mtx给释放掉
//     //不可能用在函数参数传递或者返回过程中，只能用在简单的临界区代码段的互斥操作中
//     // std::lock_guard<std::mutex> locker(mtx);

//     /*
//     通知在cv上等待的线程，条件成立了，起来干活了！
//     其它在cv上等待的线程，收到通知，从等待状态=》阻塞状态=》获取互斥锁了=》线程继续wait
//     */
//     cv.notify_all();

//     cv.notify_one();

//     return 0;
// }


