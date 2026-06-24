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
    // - Слишком маленький → тест flacky на нагруженной CI-машине
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

TEST(AsyncProblems, ManualproxyWorkaround) {
    MockObserver observer;
    MockTransport transport;
    TestProxy proxy;

    EXPECT_CALL(transport, connect(_)).WillOnce(Return(true));
    EXPECT_CALL(observer, onConnected(42))
        .WillOnce(Invoke([&](int) { proxy.signal(); }));

    AsyncService service(observer, transport);
    service.start();
    service.postConnect(42);

    // Работает, но:
    // 1) Нужно писать TestProxy самим — GTest его не предоставляет
    // 2) Каждый expectation нужно оборачивать в Invoke + signal
    // 3) proxy — per-expectation, нет единого WaitForAll
    // 4) Если забыть signal — тест зависнет навсегда (или до timeout)
    ASSERT_TRUE(proxy.wait(std::chrono::milliseconds(5000)))
        << "Timed out waiting for onConnected"; 
		// Как только worker вызовет signal(), cv разбудит тестовый поток,
		// wait вернёт true, ASSERT_TRUE пройдёт.

    service.stop();
}

// ============================================================================
// Демонстрация 3: Гонка между expectations и вызовами
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

    EXPECT_CALL(observer, onConnected(_)).Times(AnyNumber()); // Вынуждены так ставить
    EXPECT_CALL(observer, onDataReceived(_, _)).Times(AnyNumber()); // Вынуждены так ставить
    EXPECT_CALL(observer, onError(_, _)).Times(AnyNumber()); // Вынуждены так ставить

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
// Демонстрация 4: Нет встроенного timeout для всего теста
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
