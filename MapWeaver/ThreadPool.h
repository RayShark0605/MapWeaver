#pragma once
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <atomic>
#include <future>

// 生产者-消费者模式的线程池
class ThreadPool
{
public:
	explicit ThreadPool(size_t numThreads);
	~ThreadPool();

	template <class F, class... Args>
	auto Enqueue(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>;

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
auto ThreadPool::Enqueue(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>
{
	using return_type = std::invoke_result_t<F, Args...>;

	auto task = std::make_shared<std::packaged_task<return_type()>>(
		std::bind(std::forward<F>(f), std::forward<Args>(args)...)
	);

	std::future<return_type> res = task->get_future();

	{
		std::unique_lock<std::mutex> lock(queueMutex);
		if (stop.load())
		{
			throw std::runtime_error("Enqueue on stopped ThreadPool");
		}

		tasks.emplace([task]() { (*task)(); });
		pendingTasks++;
	}

	condition.notify_one();
	return res;
}