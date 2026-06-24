// ============================================================================
// Проблема 1: Невозможность обновления expectations в рантайме
// ============================================================================
//
// GMock требует, чтобы все EXPECT_CALL были заданы ДО вызова
// мокируемых методов. Это делает невозможным паттерн:
//
//   1) Поставить expectation A
//   2) Выполнить действие → callback вызывает мок
//   3) На основании результата ИЗМЕНИТЬ expectation
//   4) Выполнить следующее действие
//
// В реальных тестах мы часто не знаем, какой именно session_id
// будет возвращён сервером, пока не получим ответ. Или не знаем
// порядок колбэков в асинхронном пайплайне.
// ============================================================================

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "async_service.h"

using ::testing::_;
using ::testing::Return;
using ::testing::Invoke;
using ::testing::InSequence;
using ::testing::AnyNumber;
using ::testing::SaveArg;
using ::testing::Expectation;

// ---- Моки ----

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
// Демонстрация: что хотелось бы написать, но нельзя
// ============================================================================

// Сценарий: сервис подключается, получает session_id динамически,
// и дальнейшие expectations должны зависеть от этого id.

TEST(RuntimeExpectations, CannotUpdateExpectationAfterCall) {
    MockObserver observer;
    MockTransport transport;

    // Представим, что сервер при connect() назначает session_id
    // и мы узнаём его только в колбэке onConnected.

    int actual_session_id = -1;

    // Мы ВЫНУЖДЕНЫ ставить expectation ДО вызова,
    // но не знаем session_id заранее.
    // Приходится использовать матчер "_" (любой аргумент).
    EXPECT_CALL(observer, onConnected(_))
        .WillOnce(Invoke([&](int id) {
            actual_session_id = id;
        }));
	// Здесь мы ХОТЕЛИ бы поставить:
	//   EXPECT_CALL(observer, onDataReceived(actual_session_id, _));
	// Но GMock говорит: "нельзя ставить EXPECT_CALL после того,
	// как мок уже использовался" — это undefined behavior.

    // Подготовка: transport.connect вернёт true
    EXPECT_CALL(transport, connect(_)).WillOnce(Return(true));

    // ПРОБЛЕМА: мы должны поставить expectation на onDataReceived
    // ДО того, как узнали session_id.
    // Единственный выход — использовать "_", теряя точность проверки.
    EXPECT_CALL(observer, onDataReceived(_, _)).Times(AnyNumber());

    EXPECT_CALL(transport, send(_, _)).WillRepeatedly(Return(true));

    AsyncService service(observer, transport);
    service.start();

    service.postConnect(42);

    // Ждём обработки
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    service.postSend(42, "hello");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    service.stop();

    // actual_session_id теперь известен, но expectations уже отработали.
    // Мы НЕ МОЖЕМ ретроспективно проверить, что onDataReceived
    // был вызван именно с actual_session_id.
    EXPECT_EQ(actual_session_id, 42);
}

// ============================================================================
// Демонстрация 2: VerifyAndClearExpectations — неполное решение
// ============================================================================

TEST(RuntimeExpectations, VerifyAndClearIsAllOrNothing) {
    MockObserver observer;
    MockTransport transport;

    // Фаза 1: настраиваем expectations для connect
    EXPECT_CALL(observer, onConnected(42));
    EXPECT_CALL(transport, connect(42)).WillOnce(Return(true));

    // Допустим, нужно также установить "фоновые" expectations
    // для onError на весь тест
    EXPECT_CALL(observer, onError(_, _)).Times(0);

    AsyncService service(observer, transport);
    service.start();

    service.postConnect(42);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Хотим сбросить expectations для onConnected и поставить новые
    // для фазы send. Но VerifyAndClearExpectations сбрасывает ВСЕ:
    //
    //   testing::Mock::VerifyAndClearExpectations(&observer);
    //
    // Это уничтожит и expectation на onError(Times(0)),
    // который мы хотели сохранить на весь тест!
    //
    // Нет API для сброса expectation на ОДИН конкретный метод.

    // Поэтому приходится заново ставить ВСЕ expectations:
    testing::Mock::VerifyAndClearExpectations(&observer);

    // Заново ставим то, что хотели сохранить
    EXPECT_CALL(observer, onError(_, _)).Times(0);
    // И новые expectations для следующей фазы
    EXPECT_CALL(observer, onDataReceived(42, "ack:hello"));
    EXPECT_CALL(transport, send(42, "hello")).WillOnce(Return(true));

    service.postSend(42, "hello");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    service.stop();
}

// ============================================================================
// Демонстрация 3: Порядок expectations при условной логике
// ============================================================================

TEST(RuntimeExpectations, ConditionalExpectationWorkaround) {
    MockObserver observer;
    MockTransport transport;

    // Сценарий: если connect не удался — retry, иначе send.
    // Проблема: мы не можем на уровне EXPECT_CALL задать
    // "если connect вернул false, ожидай onError, затем второй connect".

    bool first_attempt = true;

    EXPECT_CALL(transport, connect(1))
        .WillOnce(Invoke([&](int) -> bool {
            if (first_attempt) {
                first_attempt = false;
                return false;  // первая попытка — неудача
            }
            return true;
        }))
        .WillOnce(Return(true));

    // Вынуждены описать ВСЮ последовательность заранее,
    // хотя логика зависит от рантайма:
    {
        InSequence seq;
        EXPECT_CALL(observer, onError(1, "connect failed"));
        EXPECT_CALL(observer, onConnected(1));
    }

    AsyncService service(observer, transport);
    service.start();

    // Первая попытка — fail
    service.postConnect(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Retry
    service.postConnect(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    service.stop();

    // Тест проходит, но мы были ВЫНУЖДЕНЫ знать последовательность
    // заранее. В реальных тестах retry-логика может зависеть от
    // таймаутов, состояния сети и т.д. — это невозможно предсказать.
}




// ============================================================================
// Аналоги тестов выше — версии, которые ДОЛЖНЫ проходить по смыслу,
// но ПАДАЮТ, потому что GMock не позволяет выставлять expectations в рантайме.
// ============================================================================

// --- Проблема 1: матчеры фиксируются по значению ---
//
// Полная декларативная попытка с After: используем Expectation handle
// и .After(conn), чтобы упорядочить expectations. GMock корректно
// обеспечивает порядок активации, но матчер всё равно содержит
// замороженное значение -1, а фактический вызов приходит с 42.
//
// ПАДАЕТ: "Unexpected mock function call: onDataReceived(42, "ack:hello")"
//
TEST(RuntimeExpectationsFailing, DynamicId_ExpectationFrozenAtSetup_FAILS) {
    MockObserver observer;
    MockTransport transport;

    int captured_id = -1;

    // (1) Полный декларативный сценарий с упорядочением:
    Expectation conn = EXPECT_CALL(observer, onConnected(_))
        .WillOnce(SaveArg<0>(&captured_id));

    EXPECT_CALL(transport, connect(_))
        .WillOnce(Return(true));
    EXPECT_CALL(transport, send(_, _))
        .WillOnce(Return(true));

    // (2) After(conn) гарантирует, что этот expectation
    //     активируется только ПОСЛЕ onConnected.
    //     Но captured_id == -1 в момент объявления!
    //     GMock запоминает ЗНАЧЕНИЕ -1, а не ссылку.
    EXPECT_CALL(observer, onDataReceived(captured_id, _))
        .Times(1)
        .After(conn);
    //                                   ^^^^^^^^^^^
    //           == Eq(-1), хотя в runtime будет 42

    AsyncService service(observer, transport);
    service.start();
    service.postConnect(42);
    std::this_thread::sleep_for(
        std::chrono::milliseconds(100));
    service.postSend(42, "hello");
    std::this_thread::sleep_for(
        std::chrono::milliseconds(100));
    service.stop();

    EXPECT_EQ(captured_id, 42); // пройдёт, но тест
                                // уже упал выше
}

// --- Проблема 2: expectation после фактического вызова ---
//
// Попытка поставить expectation из Invoke: session_id уже известен,
// но EXPECT_CALL выполняется внутри g_gmock_mutex, который уже захвачен.
// Результат: deadlock или undefined behavior.
//
// ВНИМАНИЕ: этот тест закомментирован, т.к. при запуске вызывает deadlock.
// Раскомментируйте для демонстрации проблемы.
//
// TEST(RuntimeExpectationsFailing, PhaseExpectation_ViaInvoke_FAILS) {
//     MockObserver observer;
//     MockTransport transport;
//
//     EXPECT_CALL(transport, connect(_))
//         .WillOnce(Return(true));
//     EXPECT_CALL(transport, send(_, _))
//         .WillOnce(Return(true));
//
//     // Пытаемся выставить ожидание "по месту":
//     // внутри Invoke session_id уже известен.
//     EXPECT_CALL(observer, onConnected(_))
//         .WillOnce(Invoke([&](int session_id) {
//             // К этому моменту session_id == 42.
//             // Но EXPECT_CALL выполняется внутри
//             // g_gmock_mutex, который уже захвачен!
//             EXPECT_CALL(observer,
//                 onDataReceived(session_id, _))
//                 .Times(1);
//         }));
//
//     AsyncService service(observer, transport);
//     service.start();
//     service.postConnect(42);
//     std::this_thread::sleep_for(
//         std::chrono::milliseconds(100));
//     service.postSend(42, "hello");
//     std::this_thread::sleep_for(
//         std::chrono::milliseconds(100));
//     service.stop();
// }

// --- Проблема 2 (наивный вариант): запоздалый expectation в теле теста ---
//
// Альтернативный подход: ставим expectation не из Invoke, а в теле теста
// после ожидания завершения фазы. Deadlock не возникает, но результат
// аналогичен: вызов произошёл до регистрации expectation.
//
// ПАДАЕТ: "Expected: to be called once. Actual: never called."
//
TEST(RuntimeExpectationsFailing, PhaseExpectation_SetAfterActualCall_FAILS) {
    MockObserver observer;
    MockTransport transport;

    EXPECT_CALL(observer, onConnected(42));
    EXPECT_CALL(transport, connect(42)).WillOnce(Return(true));
    EXPECT_CALL(transport, send(42, "hello")).WillOnce(Return(true));

    AsyncService service(observer, transport);
    service.start();

    service.postConnect(42);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Фаза 1 завершена. Теперь — фаза 2.
    // Отправляем и ждём завершения — onDataReceived уже вызван:
    service.postSend(42, "hello");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Пытаемся "по месту" поставить expectation — слишком поздно:
    EXPECT_CALL(observer, onDataReceived(42, "ack:hello")).Times(1);

    service.stop();

    // ОЖИДАЕМЫЙ ПРОВАЛ:
    // - "Expected: to be called once. Actual: never called."
    //   (вызов произошёл до регистрации expectation, поэтому
    //    GMock его не засчитал)
}

// --- Проблема 3: условное ветвление в runtime ---
//
// Транспорт решает вернуть ошибку, но тест ожидает успешный путь.
// Два провала одновременно: onConnected "never called" и
// onError "called 1 time, expected 0".
//
TEST(RuntimeExpectationsFailing, ConditionalBranch_WrongAssumption_FAILS) {
    MockObserver observer;
    MockTransport transport;

    // Транспорт решает вернуть ошибку
    EXPECT_CALL(transport, connect(1))
        .WillOnce(Return(false));

    // Мы ожидаем успешный путь:
    EXPECT_CALL(observer, onConnected(1)).Times(1);
    EXPECT_CALL(observer, onError(1, _)).Times(0);

    AsyncService service(observer, transport);
    service.start();
    service.postConnect(1);
    std::this_thread::sleep_for(
        std::chrono::milliseconds(100));
    service.stop();
}
// --- Решение проблемы 3 с помощью предлагаемого расширения .When() ---
//
// ВНИМАНИЕ: этот тест использует метод .When(), который НЕ существует
// в стандартном GMock. Он демонстрирует, как выглядело бы решение
// с предлагаемым расширением (см. статью, раздел "Предлагаемое решение 1").
// Для компиляции требуется модифицированная версия gmock-spec-builders.h
// с добавленным методом When(std::function<bool()>).
//
TEST(RuntimeExpectationsFixed, ConditionalBranch_WithWhen) {
    MockObserver observer;
    MockTransport transport;

    // Состояние, которое обновляется в runtime
    std::atomic<bool> connect_succeeded{false};

    // Транспорт решает вернуть ошибку — как и раньше
    EXPECT_CALL(transport, connect(1))
        .WillOnce(Invoke([&](int) {
            bool result = false;  // или true — неважно, тест корректен в обоих случаях
            connect_succeeded.store(result);
            return result;
        }));

    // Путь A: успешное подключение
    EXPECT_CALL(observer, onConnected(1))
        .When([&] { return connect_succeeded.load(); })
        .Times(::testing::AtMost(1));

    // Путь B: ошибка подключения
    EXPECT_CALL(observer, onError(1, _))
        .When([&] { return !connect_succeeded.load(); })
        .Times(1);

    AsyncService service(observer, transport);
    service.start();
    service.postConnect(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    service.stop();

    // connect вернул false →
    //   connect_succeeded == false →
    //   onConnected.When() возвращает false → expectation невидим для матчера →
    //   onError.When() возвращает true → expectation активен, Times(1) удовлетворён
    //
    // Если бы connect вернул true — зеркально:
    //   onConnected активен, onError невидим
    //
    // Тест проходит В ОБОИХ СЛУЧАЯХ.
}
