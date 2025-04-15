#include "ThreadPool.h"

using namespace std;

ThreadPool::ThreadPool(size_t numThreads) : stop(false)
{
    if (numThreads == 0)
    {
        throw invalid_argument("ThreadPool must have at least one thread");
    }

    for (size_t i = 0; i < numThreads; i++)
    {
        workers.emplace_back([this]
            {
            while (true)
            {
                function<void()> task;
                {
                    unique_lock<mutex> lock(queueMutex);
                    condition.wait(lock, [this] {
                        return stop.load() || !tasks.empty();
                        });

                    if (stop && tasks.empty())
                    {
                        return;
                    }

                    task = move(tasks.front());
                    tasks.pop();
                }

                try
                {
                    if (task)
                    {
                        task();
                    }
                }
                catch (...){ }

                // 任务完成时更新计数器
                {
                    unique_lock<mutex> lock(queueMutex);
                    pendingTasks--;
                    if (pendingTasks == 0)
                    {
                        conditionFinished.notify_all();
                    }
                }
            }
            });
    }
}

void ThreadPool::WaitAll()
{
    unique_lock<mutex> lock(queueMutex);
    conditionFinished.wait(lock, [this]() { 
        return pendingTasks == 0;
        });
}

ThreadPool::~ThreadPool()
{
    // 温和关闭：先处理完当前队列内所有任务后再退出线程
    stop.store(true);
    condition.notify_all();

	for (thread& worker : workers)
	{
        if (worker.joinable())
        {
            worker.join();
        }
	}
}
