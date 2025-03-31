#pragma once
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <atomic>

class ThreadPool
{
public:
	explicit ThreadPool(size_t numThreads);

	template <class F, class... Args>
	void Enqueue(F&& f, Args&&... args);

	void WaitAll();
	~ThreadPool();

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