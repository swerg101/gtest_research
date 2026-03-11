#pragma once

#include <functional>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <vector>
#include <future>

// ---------------------------------------------------------------------------
// Упрощённая модель асинхронного сервиса, аналогичная тому,
// что встречается в продакшн-коде: колбэки, события, очереди.
// Используется для демонстрации проблем GTest/GMock.
// ---------------------------------------------------------------------------

// Интерфейс наблюдателя — то, что мокаем через GMock
class IEventObserver {
public:
    virtual ~IEventObserver() = default;

    virtual void onConnected(int session_id) = 0;
    virtual void onDataReceived(int session_id, const std::string& data) = 0;
    virtual void onDisconnected(int session_id, int reason_code) = 0;
    virtual void onError(int session_id, const std::string& error) = 0;
};

// Интерфейс транспорта
class ITransport {
public:
    virtual ~ITransport() = default;
    virtual bool send(int session_id, const std::string& data) = 0;
    virtual bool connect(int session_id) = 0;
    virtual void disconnect(int session_id) = 0;
};

// ---------------------------------------------------------------------------
// AsyncService — имитация асинхронного сервиса с рабочим потоком.
// В реальном проекте это может быть сетевой клиент, брокер сообщений и т.п.
// ---------------------------------------------------------------------------
class AsyncService {
public:
    explicit AsyncService(IEventObserver& observer, ITransport& transport)
        : observer_(observer)
        , transport_(transport)
        , running_(false)
    {}

    ~AsyncService() {
        stop();
    }

    void start() {
        running_ = true;
        worker_ = std::thread([this] { workerLoop(); });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            running_ = false;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    // Поставить задачу в очередь (вызывается из клиентского потока)
    void postConnect(int session_id) {
        enqueue([this, session_id] {
            if (transport_.connect(session_id)) {
                observer_.onConnected(session_id);
            } else {
                observer_.onError(session_id, "connect failed");
            }
        });
    }

    void postSend(int session_id, const std::string& data) {
        enqueue([this, session_id, data] {
            if (transport_.send(session_id, data)) {
                observer_.onDataReceived(session_id, "ack:" + data);
            } else {
                observer_.onError(session_id, "send failed");
            }
        });
    }

    void postDisconnect(int session_id, int reason) {
        enqueue([this, session_id, reason] {
            transport_.disconnect(session_id);
            observer_.onDisconnected(session_id, reason);
        });
    }

private:
    void enqueue(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return !running_ || !tasks_.empty(); });
                if (!running_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    IEventObserver& observer_;
    ITransport&     transport_;
    std::thread     worker_;
    std::mutex      mu_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> tasks_;
    std::atomic<bool> running_;
};

// ---------------------------------------------------------------------------
// Вспомогательная утилита: барьер для синхронизации тестов с async кодом.
// Этого в GTest/GMock нет «из коробки» — приходится писать самим.
// ---------------------------------------------------------------------------
class TestBarrier {
public:
    void signal() {
        std::lock_guard<std::mutex> lk(mu_);
        ready_ = true;
        cv_.notify_all();
    }

    bool wait(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [this] { return ready_; });
    }

    void reset() {
        std::lock_guard<std::mutex> lk(mu_);
        ready_ = false;
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool ready_ = false;
};
