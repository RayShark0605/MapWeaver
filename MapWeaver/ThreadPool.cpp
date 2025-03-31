#include "ThreadPool.h"

using namespace std;

ThreadPool::ThreadPool(size_t numThreads) : stop(false)
{
    for (size_t i = 0; i < numThreads; i++)
    {
        workers.emplace_back([this]
            {
            while (true)
            {
                function<void()> task;
                {
                    unique_lock<mutex> lock(this->queueMutex);
                    this->condition.wait(lock, [this] {
                        return this->stop || !this->tasks.empty();
                        });

                    if (this->stop && this->tasks.empty())
                    {
                        return;
                    }

                    task = move(this->tasks.front());
                    this->tasks.pop();
                }

                try
                {
                    if (task)
                    {
                        task();
                    }
                }
                catch (const exception& e)
                {
                }

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
	{
		unique_lock<mutex> lock(queueMutex);
		stop = true;
	}

	condition.notify_all();
	for (thread& worker : workers)
	{
		worker.join();
	}
}
