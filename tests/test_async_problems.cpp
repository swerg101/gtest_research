// ============================================================================
// Проблема 2: Асинхронность и многопоточность — отсутствие поддержки
// ============================================================================
//
// GTest/GMock спроектированы для синхронных unit-тестов.
// Для функциональных тестов с async-кодом приходится писать
// собственную инфраструктуру синхронизации.
//
// Основные проблемы:
//   - Нет встроенного WaitFor/WaitUntil для expectations
//   - Assertions из не-основного потока не всегда корректно работают
//   - sleep_for — единственный «простой» способ ожидания, он ненадёжен
//   - Нет timeout на уровне отдельного expectation
// ============================================================================

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "async_service.h"

#include <future>
#include <atomic>
#include <latch>

using ::testing::_;
using ::testing::Return;
using ::testing::Invoke;
using ::testing::AnyNumber;

class MockObserver : public IEventObserver {
public:
    MOCK_METHOD(void, onConnected,    (int session_id),                          (override));
    MOCK_METHOD(void, onDataReceived, (int session_id, const std::string& data), (override));
    MOCK_METHOD(void, onDisconnected, (int session_id, int reason_code),         (override));
    MOCK_METHOD(void, onError,        (int session_id, const std::string& error),(override));
};

class MockTransport : public ITransport {
public:
    MOCK_METHOD(bool, send,       (int session_id, const std::string& data), (override));
    MOCK_METHOD(bool, connect,    (int session_id),                          (override));
    MOCK_METHOD(void, disconnect, (int session_id),                          (override));
};

// ============================================================================
// Демонстрация 1: sleep_for — ненадёжная синхронизация
// ============================================================================

TEST(AsyncProblems, SleepBasedSyncIsFlaky) {
    MockObserver observer;
    MockTransport transport;

    EXPECT_CALL(transport, connect(_)).WillOnce(Return(true));
    EXPECT_CALL(observer, onConnected(42));

    AsyncService service(observer, transport);
    service.start();
    service.postConnect(42);

    // ПРОБЛЕМА: какой таймаут выбрать?
    // - Слишком маленький → тест флейкует на нагруженной CI-машине
    // - Слишком большой → тесты тормозят
    // - На разных платформах нужны разные значения
    //
    // GTest не предоставляет API вида:
    //   EXPECT_CALL(...).WaitFor(std::chrono::seconds(5));
    // или
    //   ASSERT_THAT_EVENTUALLY(condition, timeout);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    service.stop();
}

// ============================================================================
// Демонстрация 2: Самодельный барьер — workaround
// ============================================================================

TEST(AsyncProblems, ManualBarrierWorkaround) {
    MockObserver observer;
    MockTransport transport;
    TestBarrier barrier;

    EXPECT_CALL(transport, connect(_)).WillOnce(Return(true));
    EXPECT_CALL(observer, onConnected(42))
        .WillOnce(Invoke([&](int) { barrier.signal(); }));

    AsyncService service(observer, transport);
    service.start();
    service.postConnect(42);

    // Работает, но:
    // 1) Нужно писать TestBarrier самим — GTest его не предоставляет
    // 2) Каждый expectation нужно оборачивать в Invoke + signal
    // 3) Barrier — per-expectation, нет единого WaitForAll
    // 4) Если забыть signal — тест зависнет навсегда (или до timeout)
    ASSERT_TRUE(barrier.wait(std::chrono::milliseconds(5000)))
        << "Timed out waiting for onConnected";

    service.stop();
}

// ============================================================================
// Демонстрация 3: Assertion из другого потока
// ============================================================================

TEST(AsyncProblems, AssertionFromWorkerThread) {
    // GTest утверждает, что assertions thread-safe на POSIX.
    // Но EXPECT_* из не-главного потока ведёт себя неочевидно:
    // - Fatal failure (ASSERT_*) НЕ прерывает текущую функцию
    //   в другом потоке, а только помечает тест как failed.
    // - Это может привести к use-after-free, если тест
    //   "продолжится" после ASSERT_*.

    std::atomic<bool> passed{false};

    auto future = std::async(std::launch::async, [&] {
        // Этот EXPECT сработает, но если бы здесь был ASSERT_TRUE(false),
        // он НЕ прервёт этот поток — только пометит тест как failed.
        // Код после ASSERT_* продолжит выполняться!
        EXPECT_EQ(2 + 2, 4);
        passed = true;
    });

    future.wait();
    EXPECT_TRUE(passed);

    // В реальном async-коде это приводит к тому, что тест
    // "прошёл" (не упал по segfault), но проверки были неполными,
    // или наоборот — проверка была в другом потоке, но не дождались.
}

// ============================================================================
// Демонстрация 4: Гонка между expectations и вызовами
// ============================================================================

TEST(AsyncProblems, RaceBetweenExpectAndCall) {
    MockObserver observer;
    MockTransport transport;

    // Сценарий: быстрый producer и expectations, которые
    // ещё не все установлены.

    EXPECT_CALL(transport, connect(_)).WillRepeatedly(Return(true));
    EXPECT_CALL(transport, send(_, _)).WillRepeatedly(Return(true));

    // Если мы ставим expectations в одном потоке,
    // а мок вызывается из другого — есть окно гонки.
    //
    // GMock документирует:
    //   "Setting expectations and calling mock functions from
    //    different threads is NOT safe."
    //
    // Но в реальных тестах это неизбежно: expectations ставятся
    // в тестовом потоке, а колбэки приходят из worker thread.

    EXPECT_CALL(observer, onConnected(_)).Times(AnyNumber());
    EXPECT_CALL(observer, onDataReceived(_, _)).Times(AnyNumber());
    EXPECT_CALL(observer, onError(_, _)).Times(AnyNumber());

    AsyncService service(observer, transport);
    service.start();

    // Множественные параллельные операции
    for (int i = 0; i < 10; ++i) {
        service.postConnect(i);
        service.postSend(i, "data_" + std::to_string(i));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    service.stop();

    // Тест "проходит", но мы фактически отказались от точных
    // проверок (Times(AnyNumber)), потому что не можем гарантировать
    // порядок и количество вызовов в async-окружении.
}

// ============================================================================
// Демонстрация 5: Нет встроенного timeout для всего теста
// ============================================================================

TEST(AsyncProblems, NoBuiltInTestTimeout) {
    // GTest не имеет встроенного per-test timeout.
    // Если async-операция зависнет, тест зависнет навсегда.
    //
    // Workaround: внешние скрипты (timeout команда), или
    // самодельный watchdog thread, или CI-level timeouts.
    //
    // Альтернативные фреймворки (Catch2 с плагинами, Boost.Test)
    // тоже не решают это полностью на уровне API.

    // Пример: если бы здесь был реальный deadlock —
    // тест повисел бы навсегда.
    std::promise<int> promise;
    auto future = promise.get_future();

    // Используем wait_for, потому что GTest не защитит нас
    auto status = future.wait_for(std::chrono::milliseconds(100));
    EXPECT_EQ(status, std::future_status::timeout);

    // Устанавливаем значение, чтобы тест корректно завершился
    promise.set_value(42);
}
