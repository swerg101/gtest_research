// ============================================================================
// Аналог тестов runtime expectations из GMock — на FakeIt.
//
// Цель: проверить, решает ли парадигма Arrange-Act-Assert (stub-then-verify)
// те же проблемы, что и RAII-модель Trompeloeil, или привносит свои
// ограничения.
//
// Используем GTest как test runner + FakeIt как mock framework.
// ============================================================================

#include <gtest/gtest.h>
#include <fakeit.hpp>
#include "async_service.h"

#include <atomic>
#include <thread>
#include <chrono>

using namespace fakeit;


// ============================================================================
// Проблема 1 (GMock): Невозможность использования runtime-значений
// ============================================================================
//
// В GMock: EXPECT_CALL(observer, onDataReceived(captured_id, _))
// копирует значение captured_id на момент объявления (== -1).
// Когда реальный вызов приходит с id=42, expectation не совпадает.
//
// FakeIt: парадигма Arrange-Act-Assert. Stubbing задаётся ДО вызова,
// но верификация (Verify) — ПОСЛЕ. На этапе верификации captured_id
// уже содержит реальное значение.
//
// ОДНАКО: на этапе стабирования значение по-прежнему должно быть
// известно заранее. Для динамического поведения — AlwaysDo с лямбдой.
//
// Замечание: аналогичный двухфазный подход (захватить значение в первой
// фазе, использовать во второй) возможен и в GMock — через SaveArg
// с последующим EXPECT_CALL. Принципиальное отличие FakeIt в том, что
// ClearInvocationHistory очищает только записанные вызовы, не затрагивая
// стабы, тогда как GMock-овский VerifyAndClearExpectations уничтожает всё.
// ============================================================================

TEST(FakeIt, DynamicId_ExpectationFrozenAtSetup) {
    Mock<IEventObserver> mockObserver;
    Mock<ITransport> mockTransport;
    int captured_id = -1;

    // Фаза 1: стабим с любыми аргументами,
    // захватываем id через лямбду
    When(Method(mockTransport, connect))
        .AlwaysDo([](int) { return true; });
    When(Method(mockObserver, onConnected))
        .AlwaysDo([&](int id) {
            captured_id = id;
        });

    {
        AsyncService service(mockObserver.get(),
                             mockTransport.get());
        service.start();
        service.postConnect(42);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100));
        service.stop();
    }

    // Верификация ПОСТФАКТУМ ---
    // captured_id уже содержит реальное значение
    ASSERT_NE(captured_id, -1);
    Verify(Method(mockObserver, onConnected)
        .Using(captured_id)).Exactly(1);

    // Фаза 2: captured_id уже известен ---
    // используем в стабе и верификации
    mockObserver.ClearInvocationHistory();
    mockTransport.ClearInvocationHistory();

    When(Method(mockTransport, send)
        .Using(captured_id, "hello")).Return(true);
    When(Method(mockObserver, onDataReceived))
        .AlwaysDo([](int, const std::string&) {});

    {
        AsyncService service(mockObserver.get(),
                             mockTransport.get());
        service.start();
        service.postSend(captured_id, "hello");
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100));
        service.stop();
    }

    Verify(Method(mockObserver, onDataReceived)
        .Using(captured_id,
               std::string("ack:hello"))).Exactly(1);
}


// ============================================================================
// Проблема 2 (GMock): VerifyAndClearExpectations — всё или ничего
// ============================================================================
//
// В GMock: VerifyAndClearExpectations(&observer) сбрасывает ВСЕ expectations.
// Нельзя сбросить expectation для одного метода, сохранив остальные.
//
// FakeIt: стабы и верификация — независимые операции.
// ClearInvocationHistory() очищает записанные вызовы, не трогая стабы.
// Верификация — отдельная фаза, вызывается по требованию.
//
// ОДНАКО: сбросить стаб для одного метода нельзя — только Reset() для всего.
// ============================================================================

TEST(FakeIt, VerifyAndClearIsAllOrNothing) {
    Mock<IEventObserver> mockObserver;
    Mock<ITransport> mockTransport;

    // Стабы на весь тест
    When(Method(mockTransport, connect))
        .AlwaysReturn(true);
    When(Method(mockTransport, send))
        .AlwaysReturn(true);
    Fake(Method(mockObserver, onConnected),
         Method(mockObserver, onDataReceived),
         Method(mockObserver, onError));

    // Фаза 1: connect
    {
        AsyncService service(mockObserver.get(),
                             mockTransport.get());
        service.start();
        service.postConnect(42);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100));
        service.stop();
    }

    // Верификация фазы 1
    Verify(Method(mockTransport, connect)
        .Using(42)).Exactly(1);
    Verify(Method(mockObserver, onConnected)
        .Using(42)).Exactly(1);
    Verify(Method(mockObserver, onError)).Never();

    // Сброс вызовов --стабы остаются!
    mockObserver.ClearInvocationHistory();
    mockTransport.ClearInvocationHistory();

    // Фаза 2: send --стабы по-прежнему действуют
    {
        AsyncService service(mockObserver.get(),
                             mockTransport.get());
        service.start();
        service.postSend(42, "hello");
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100));
        service.stop();
    }

    // Видим ТОЛЬКО вызовы фазы 2
    Verify(Method(mockTransport, send)
        .Using(42, std::string("hello"))).Exactly(1);
    Verify(Method(mockObserver, onDataReceived)
        .Using(42,
               std::string("ack:hello"))).Exactly(1);
    Verify(Method(mockObserver, onError)).Never();
}


// ============================================================================
// Проблема 3 (GMock): Условное ветвление в runtime
// ============================================================================
//
// Важно: в FakeIt НЕВОЗМОЖНО построить тест, эквивалентный GMock-версии,
// где expectations объявляются ДО выполнения и тест падает при несовпадении
// ветки. FakeIt не имеет аналога EXPECT_CALL(...).Times(N) — стабы через
// When(...) не верифицируют количество вызовов, а Verify работает только
// постфактум. Поэтому сразу покажем постфактум-подход.
// ============================================================================

TEST(FakeIt, ConditionalBranch_PostFactum) {
    Mock<IEventObserver> mockObserver;
    Mock<ITransport> mockTransport;

    // Транспорт решает вернуть ошибку
    When(Method(mockTransport, connect))
        .Return(false);
    // Стабим ОБА пути (иначе UnexpectedMethodCall)
    Fake(Method(mockObserver, onConnected),
         Method(mockObserver, onError));

    {
        AsyncService service(mockObserver.get(),
                             mockTransport.get());
        service.start();
        service.postConnect(1);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100));
        service.stop();
    }

    // Верификация ПОСТФАКТУМ: мы ЗНАЕМ,
    // что connect вернул false
    Verify(Method(mockObserver, onError)).Exactly(1);
    Verify(Method(mockObserver, onConnected)).Never();
}


// ============================================================================
// Проблема 3 (альтернатива): ручная условная верификация
// ============================================================================
//
// Если результат НЕ ИЗВЕСТЕН заранее (как в исходной формулировке П3),
// приходится выносить условную логику за пределы фреймворка.
// Нет аналога .When(): невозможно декларативно сказать
// «если состояние X — ожидай Y». Условная верификация — только if/else.
// ============================================================================

TEST(FakeIt, ConditionalBranch_ManualVerification) {
    Mock<IEventObserver> mockObserver;
    Mock<ITransport> mockTransport;

    // Результат неизвестен заранее
    std::atomic<bool> connect_succeeded{false};

    When(Method(mockTransport, connect))
        .AlwaysDo([&](int) -> bool {
            bool result = false;  // runtime-решение
            connect_succeeded.store(result);
            return result;
        });

    Fake(Method(mockObserver, onConnected),
         Method(mockObserver, onError));

    {
        AsyncService service(mockObserver.get(),
                             mockTransport.get());
        service.start();
        service.postConnect(1);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100));
        service.stop();
    }

    // Верификация: if/else ВНЕ фреймворка
    if (connect_succeeded.load()) {
        Verify(Method(mockObserver, onConnected))
            .Exactly(1);
        Verify(Method(mockObserver, onError)).Never();
    } else {
        Verify(Method(mockObserver, onError))
            .Exactly(1);
        Verify(Method(mockObserver, onConnected))
            .Never();
    }
}


// ============================================================================
// Дополнительно: Spy-паттерн — уникальная возможность FakeIt
// ============================================================================
//
// FakeIt — единственный из рассматриваемых фреймворков, поддерживающий
// spying: перехват вызовов реального объекта с записью для верификации.
//
// Примечание: spy работает только если передан реальный объект
// с виртуальными методами. В данном тесте используем простой класс
// для демонстрации механизма.
// ============================================================================

// Простая реализация для демонстрации spy
class SimpleTransport : public ITransport {
public:
    bool send(int, const std::string&) override { return true; }
    bool connect(int) override { return true; }
    void disconnect(int) override {}
};

TEST(FakeIt, SpyPattern) {
    SimpleTransport realTransport;
    Mock<ITransport> spy(realTransport);

    // Spy: перехватываем вызовы, но делегируем реальной реализации
    Spy(Method(spy, connect));
    Spy(Method(spy, send));

    ITransport& transport = spy.get();

    // Вызываем через spy — реальная реализация выполняется
    bool result = transport.connect(42);
    EXPECT_TRUE(result);  // реальная реализация вернула true

    transport.send(42, "hello");

    // Верификация: spy записал вызовы
    Verify(Method(spy, connect).Using(42)).Exactly(1);
    Verify(Method(spy, send).Using(42, std::string("hello"))).Exactly(1);

    // Spy позволяет проверить вызовы реального объекта без стабирования.
    // Это частично снимает проблему П1: не нужно заранее знать аргументы,
    // достаточно верифицировать постфактум.
}
