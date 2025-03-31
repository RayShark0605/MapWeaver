#pragma once
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <atomic>

// 生产者-消费者模式的线程池
class ThreadPool
{
public:
	explicit ThreadPool(size_t numThreads);
	~ThreadPool();

	template <class F, class... Args>
	void Enqueue(F&& f, Args&&... args);

	// 等待所有线程执行完毕
	void WaitAll();

private:
	std::vector<std::thread> workers;
	std::queue<std::function<void()>> tasks;

	std::mutex queueMutex;
	std::condition_variable condition;
	std::condition_variable conditionFinished;
	std::atomic<bool> stop;
	int pendingTasks = 0;
};


template <class F, class... Args>
void ThreadPool::Enqueue(F&& f, Args&&... args)
{
	{
		std::unique_lock<std::mutex> lock(queueMutex);
		tasks.emplace([f, args...]() mutable {
			std::invoke(f, args...);
			});
		pendingTasks++;
	}
	condition.notify_one();
}