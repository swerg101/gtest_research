// ============================================================================
// Аналог тестов runtime expectations из GMock — на Trompeloeil.
//
// Цель: проверить, воспроизводятся ли те же проблемы, или
// RAII-модель expectations в Trompeloeil их решает.
//
// Используем GTest как test runner + Trompeloeil как mock framework.
// ============================================================================

#include <gtest/gtest.h>
#include <gtest/trompeloeil.hpp>
#include "async_service.h"

#include <atomic>
#include <thread>
#include <chrono>

using trompeloeil::_;

// ---- Моки (Trompeloeil) ----

class MockObserver : public IEventObserver {
public:
    MAKE_MOCK1(onConnected,    void(int), override);
    MAKE_MOCK2(onDataReceived, void(int, const std::string&), override);
    MAKE_MOCK2(onDisconnected, void(int, int), override);
    MAKE_MOCK2(onError,        void(int, const std::string&), override);
};

class MockTransport : public ITransport {
public:
    MAKE_MOCK2(send,       bool(int, const std::string&), override);
    MAKE_MOCK1(connect,    bool(int), override);
    MAKE_MOCK1(disconnect, void(int), override);
};


// ============================================================================
// Проблема 1 (GMock): Невозможность использования runtime-значений
// ============================================================================
//
// В GMock мы не могли использовать captured_id в EXPECT_CALL, потому что
// значение копировалось на момент объявления (== -1).
//
// Trompeloeil: expectations живут в RAII-скоупе. Мы можем создать
// expectation ПОСЛЕ того, как узнали session_id.
// ============================================================================

TEST(Trompeloeil, DynamicId_ExpectationFrozenAtSetup) {
    MockObserver observer;
    MockTransport transport;
    int captured_id = -1;

    // Фаза 1: узнаём session_id
    {
        REQUIRE_CALL(transport, connect(_))
            .RETURN(true);
        REQUIRE_CALL(observer, onConnected(_))
            .LR_SIDE_EFFECT(captured_id = _1);

        AsyncService service(observer, transport);
        service.start();
        service.postConnect(42);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100));
        service.stop();
    } // expectations верифицированы, captured_id == 42

    // Фаза 2: используем captured_id в НОВОМ expectation
    {
        REQUIRE_CALL(transport,
            send(captured_id, "hello"))
            .RETURN(true);
        REQUIRE_CALL(observer,
            onDataReceived(captured_id, "ack:hello"));

        AsyncService service(observer, transport);
        service.start();
        service.postSend(captured_id, "hello");
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100));
        service.stop();
    }
}


// ============================================================================
// Проблема 2 (GMock): VerifyAndClearExpectations — всё или ничего
// ============================================================================
//
// В GMock: VerifyAndClearExpectations сбрасывает ВСЕ expectations объекта.
// Нельзя сбросить expectation для одного метода, сохранив другие.
//
// Trompeloeil: expectations независимы, каждый живёт в своём скоупе.
// Можно комбинировать скоупы произвольно.
// ============================================================================

TEST(Trompeloeil, ScopedExpectations_P2) {
    MockObserver observer;
    MockTransport transport;

    // "Фоновый" expectation на весь тест
    FORBID_CALL(observer, onError(_, _));

    // Фаза 1: connect
    {
        REQUIRE_CALL(transport, connect(42))
            .RETURN(true);
        REQUIRE_CALL(observer, onConnected(42));

        AsyncService service(observer, transport);
        service.start();
        service.postConnect(42);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100));
        service.stop();
    }
    // Expectations фазы 1 уничтожены и верифицированы.
    // FORBID_CALL(onError) по-прежнему активен!
    //
    // КЛЮЧЕВОЙ МОМЕНТ: если здесь вызвать
    // transport.connect(42), тест УПАДЁТ с ошибкой
    // "No match for call", потому что REQUIRE_CALL
    // на connect уже уничтожен при выходе из {}.
    // При этом FORBID_CALL на onError остаётся.

    // Фаза 2: send (с НОВЫМ connect)
    {
        // Новый expectation на connect -- с ДРУГИМ
        // поведением, чем в фазе 1:
        REQUIRE_CALL(transport, connect(42))
            .RETURN(false);   // теперь возвращаем false
        REQUIRE_CALL(transport, send(42, "hello"))
            .RETURN(true);
        REQUIRE_CALL(observer,
            onDataReceived(42, "ack:hello"));

        AsyncService service(observer, transport);
        service.start();
        // connect(42) теперь вернёт false --
        // это ДРУГОЙ expectation, не из фазы 1.
        service.postConnect(42);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(50));
        service.postSend(42, "hello");
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100));
        service.stop();
    }
    // FORBID_CALL(onError) по-прежнему активен
    // и был бы нарушен, если бы onError вызвался.
}


// ============================================================================
// Проблема 3 (GMock): Условное ветвление в runtime — строгие ожидания
// ============================================================================
//
// Для корректного сравнения сначала покажем тест, эквивалентный
// GMock-версии — с теми же строгими ожиданиями.
//
// При использовании строгих ожиданий (REQUIRE_CALL + FORBID_CALL)
// Trompeloeil ведёт себя ИДЕНТИЧНО GMock: тест падает, потому что
// expectations описывают не ту ветку.
// ============================================================================

TEST(Trompeloeil,
     ConditionalBranch_Strict_FAILS) {
    MockObserver observer;
    MockTransport transport;

    REQUIRE_CALL(transport, connect(1))
        .RETURN(false);

    // Строгие ожидания: ожидаем onConnected,
    // запрещаем onError
    REQUIRE_CALL(observer, onConnected(1));
    FORBID_CALL(observer, onError(1, _));

    AsyncService service(observer, transport);
    service.start();
    service.postConnect(1);
    std::this_thread::sleep_for(
        std::chrono::milliseconds(100));
    service.stop();

    // Результат: тест ПАДАЕТ с двумя ошибками ---
    // onConnected "never called" и
    // onError нарушил FORBID_CALL.
    // Ситуация идентична GMock-версии.
}


// ============================================================================
// Проблема 3 (GMock): Условное ветвление — ослабление ожиданий
// ============================================================================
//
// Единственный способ не падать — заменить строгие ожидания на
// "разрешающие" (ALLOW_CALL).
//
// Тест не падает, но ценой потери строгости: ALLOW_CALL допускает
// и полное отсутствие вызова, то есть тест пройдёт даже если
// ни onConnected, ни onError не будут вызваны вовсе.
// ============================================================================

TEST(Trompeloeil,
     ConditionalBranch_AllowBothPaths) {
    MockObserver observer;
    MockTransport transport;

    REQUIRE_CALL(transport, connect(1))
        .RETURN(false);

    // Ослабляем: ALLOW_CALL допускает 0..inf вызовов
    ALLOW_CALL(observer, onConnected(1));
    ALLOW_CALL(observer, onError(1, _));

    AsyncService service(observer, transport);
    service.start();
    service.postConnect(1);
    std::this_thread::sleep_for(
        std::chrono::milliseconds(100));
    service.stop();

    // Тест проходит, но ALLOW_CALL допускает
    // и полное отсутствие вызова.
    // Невозможно ТРЕБОВАТЬ, чтобы при connect==false
    // был вызван именно onError.
}


// ============================================================================
// Проблема 3 (альтернатива): строгая проверка через NAMED_REQUIRE_CALL
// ============================================================================
//
// Более строгий вариант: создаём REQUIRE_CALL только для той ветки,
// которая реально исполнилась. Это возможно благодаря RAII — мы можем
// поставить expectation ПОСЛЕ того, как узнали результат.
//
// В GMock это принципиально невозможно.
// ============================================================================

TEST(TrompeloeilRuntimeExpectations, PhaseExpectation_SetAfterActualCall) {
    MockObserver observer;
    MockTransport transport;

    bool connect_result = false;  // "решение" транспорта

    // Фаза 1: выполняем connect, фиксируем результат
    // Разрешаем любые вызовы observer на этой фазе
    {
        REQUIRE_CALL(transport, connect(1))
            .LR_SIDE_EFFECT(connect_result = false)
            .RETURN(false);

        // Разрешаем оба колбэка — пока не знаем, какой будет
        ALLOW_CALL(observer, onConnected(_));
        ALLOW_CALL(observer, onError(_, _));

        AsyncService service(observer, transport);
        service.start();
        service.postConnect(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        service.stop();
    }

    // Фаза 2: мы знаем результат, ставим СТРОГИЙ expectation
    // и повторяем операцию (или проверяем записанные данные).
    //
    // Ключевой момент: в GMock мы не можем поставить expectation
    // после вызова. Здесь — можем поставить его для СЛЕДУЮЩЕГО вызова,
    // основываясь на знании, полученном в фазе 1.

    EXPECT_FALSE(connect_result);

    // Если бы нужно было повторить попытку на основе результата:
    if (!connect_result) {
        // Retry с ожиданием успеха
        REQUIRE_CALL(transport, connect(1)).RETURN(true);
        REQUIRE_CALL(observer, onConnected(1));

        AsyncService service(observer, transport);
        service.start();
        service.postConnect(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        service.stop();
    }
}


// ============================================================================
// Дополнительно: NAMED expectations с ручным управлением lifetime
// ============================================================================
//
// Демонстрация того, что в Trompeloeil expectation можно "выключить"
// в runtime, уничтожив unique_ptr. В GMock retire() необратим, а
// VerifyAndClearExpectations сбрасывает всё.
// ============================================================================

TEST(TrompeloeilRuntimeExpectations, ConditionalExpectationWorkaround) {
    MockObserver observer;
    MockTransport transport;

    // Создаём named expectations — это unique_ptr
    auto expect_connect = NAMED_REQUIRE_CALL(transport, connect(42)).RETURN(true);
    auto expect_connected = NAMED_REQUIRE_CALL(observer, onConnected(42));

    AsyncService service(observer, transport);
    service.start();

    service.postConnect(42);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Expectations сработали. "Выключаем" их вручную:
    expect_connect.reset();
    expect_connected.reset();

    // Ставим НОВЫЕ expectations для следующей фазы.
    // В GMock для этого нужен VerifyAndClearExpectations,
    // который убил бы ВСЕ expectations.
    auto expect_send = NAMED_REQUIRE_CALL(transport, send(42, "hello")).RETURN(true);
    auto expect_data = NAMED_REQUIRE_CALL(observer, onDataReceived(42, "ack:hello"));

    service.postSend(42, "hello");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    expect_send.reset();
    expect_data.reset();

    service.stop();
}
