// ============================================================================
// Проблема 4: Проблемный API GMock
// ============================================================================
//
// GMock API имеет ряд дизайнерских проблем:
//   - Sticky expectations: неинтуитивное поведение по умолчанию
//   - Нельзя откатить (retire) одно конкретное expectation
//   - Нет ClearExpectations (только Verify+Clear)
//   - RetiresOnSaturation — «goto» мира GMock
//   - Порядок матчинга: LIFO (последний EXPECT_CALL проверяется первым)
//   - Нет WaitForAndClearExpectations для async
// ============================================================================

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "async_service.h"

using ::testing::_;
using ::testing::Return;
using ::testing::Invoke;
using ::testing::InSequence;
using ::testing::Expectation;
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
// Демонстрация 1: Sticky expectations и LIFO-порядок
// ============================================================================

TEST(GMockAPI, StickyExpectationsAreConfusing) {
    MockObserver observer;

    // Expectations проверяются в ОБРАТНОМ порядке (LIFO):
    // последний EXPECT_CALL для данного метода проверяется первым.

    // Это expectation будет "перекрыто" следующим:
    EXPECT_CALL(observer, onConnected(1))
        .Times(1);

    // Это expectation проверяется ПЕРВЫМ (LIFO):
    EXPECT_CALL(observer, onConnected(2))
        .Times(1);

    // Вызов с id=2 — ОК, матчит второй EXPECT_CALL
    observer.onConnected(2);

    // Вызов с id=1 — ОК, матчит первый EXPECT_CALL
    observer.onConnected(1);

    // Но если бы мы вызвали onConnected(2) дважды:
    //   observer.onConnected(2);
    //   observer.onConnected(2);  // ← upper bound violated!
    //
    // Потому что второй EXPECT_CALL "sticky" — после насыщения
    // он продолжает матчить и выдаёт ошибку.
    //
    // Это неинтуитивно: казалось бы, после "насыщения" должен
    // перейти к следующему, но нет.
}

// ============================================================================
// Демонстрация 2: RetiresOnSaturation — «goto GMock»
// ============================================================================

TEST(GMockAPI, RetiresOnSaturationPitfall) {
    MockObserver observer;

    // RetiresOnSaturation снимает проблему sticky, но скрывает баги:
    EXPECT_CALL(observer, onConnected(_))
        .Times(1)
        .RetiresOnSaturation();

    EXPECT_CALL(observer, onConnected(42))
        .Times(1);

    observer.onConnected(42);  // матчит второй (LIFO)
    observer.onConnected(99);  // второй retired → матчит первый

    // "Работает", но:
    // 1) Порядок вызовов неявно зависит от порядка EXPECT_CALL
    // 2) Если добавить третий вызов — непредсказуемое поведение
    // 3) RetiresOnSaturation — по сути goto: "решает" проблему
    //    текущего теста, но маскирует архитектурные проблемы
}

// ============================================================================
// Демонстрация 3: Невозможно откатить одно expectation
// ============================================================================

TEST(GMockAPI, CannotRetireOneExpectation) {
    MockObserver observer;
    MockTransport transport;

    // Фаза 1: подключение
    EXPECT_CALL(observer, onConnected(1)).Times(1);
    EXPECT_CALL(observer, onError(_, _)).Times(0);  // фоновая проверка

    // Фаза 2: после подключения хотим изменить expectations
    // только для onConnected, оставив onError.
    //
    // Нет API для:
    //   auto& exp = EXPECT_CALL(observer, onConnected(1));
    //   exp.Retire();  // ← не существует
    //
    // Есть только:
    //   Mock::VerifyAndClearExpectations(&observer);
    // Но это сбросит ВСЕ expectations, включая onError.

    observer.onConnected(1);

    // Чтобы "откатить" одно expectation, приходится
    // пересоздавать весь набор. Это verbose и error-prone.
}

// ============================================================================
// Демонстрация 4: Нет WaitForAndClearExpectations
// ============================================================================

TEST(GMockAPI, NoWaitForExpectation) {
    MockObserver observer;
    MockTransport transport;
    TestBarrier barrier;

    EXPECT_CALL(transport, connect(_)).WillOnce(Return(true));
    EXPECT_CALL(observer, onConnected(42))
        .WillOnce(Invoke([&](int) { barrier.signal(); }));

    AsyncService service(observer, transport);
    service.start();
    service.postConnect(42);

    // Хотелось бы:
    //   Mock::WaitForAndClearExpectations(&observer, timeout);
    //
    // Issue #1967 на GitHub: запрос этой фичи был открыт,
    // но не реализован. Вместо этого — ручной barrier.

    ASSERT_TRUE(barrier.wait(std::chrono::milliseconds(5000)));
    testing::Mock::VerifyAndClearExpectations(&observer);

    service.stop();
}

// ============================================================================
// Демонстрация 5: MOCK_METHOD и variadic functions
// ============================================================================

// Нельзя мокать variadic функции (printf-like API):
//
// class ILogger {
//     virtual void log(const char* fmt, ...) = 0;
// };
//
// class MockLogger : public ILogger {
//     MOCK_METHOD(void, log, (const char* fmt, ...), (override));
//     // ^^^ Ошибка компиляции! GMock не поддерживает variadic.
// };
//
// Workaround: оборачивать в не-variadic интерфейс.

// ============================================================================
// Демонстрация 6: Отсутствие per-method expectation count
// ============================================================================

TEST(GMockAPI, NoPerMethodExpectationQuery) {
    MockObserver observer;

    EXPECT_CALL(observer, onConnected(_)).Times(AnyNumber());

    observer.onConnected(1);
    observer.onConnected(2);
    observer.onConnected(3);

    // Хотелось бы:
    //   int count = Mock::GetCallCount(observer, &IEventObserver::onConnected);
    //   EXPECT_EQ(count, 3);
    //
    // Такого API нет. Приходится использовать manual counter:
    int manual_count = 0;
    testing::Mock::VerifyAndClearExpectations(&observer);

    EXPECT_CALL(observer, onConnected(_))
        .WillRepeatedly(Invoke([&](int) { manual_count++; }));

    observer.onConnected(10);
    observer.onConnected(20);

    EXPECT_EQ(manual_count, 2);
}
