// ============================================================================
// Проблема 3: Death Tests — ограничения и подводные камни
// ============================================================================
//
// Death tests в GTest проверяют, что код «умирает» (abort/exit/segfault).
// Механизм: fork()/clone() + re-exec дочернего процесса.
//
// Проблемы:
//   - fork() в многопоточной программе — UB по POSIX
//   - clone() доступен только на Linux
//   - «threadsafe» стиль перезапускает весь бинарник — очень медленно
//   - Side effects в дочернем процессе не видны родителю
//   - Не работает с sanitizers и Valgrind без специальных флагов
// ============================================================================

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <thread>
#include <atomic>
#include <csignal>

// ============================================================================
// Демонстрация 1: Death test + потоки = проблемы
// ============================================================================

class ResourceManager {
public:
    void initialize() {
        // Имитация: запускаем фоновый поток при инициализации
        worker_ = std::thread([this] {
            while (!stop_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }

    void shutdown() {
        stop_ = true;
        if (worker_.joinable()) worker_.join();
    }

    void criticalOperation() {
        if (!initialized_) {
            // В реальном коде — abort() или std::terminate()
            std::abort();
        }
    }

    ~ResourceManager() {
        stop_ = true;
        if (worker_.joinable()) worker_.join();
    }

private:
    std::thread worker_;
    std::atomic<bool> stop_{false};
    bool initialized_ = false;
};

TEST(DeathTests, BasicDeathTest) {
    // Простой death test — работает
    EXPECT_DEATH({
        ResourceManager rm;
        rm.criticalOperation();  // initialized_ == false → abort()
    }, "");
}

TEST(DeathTests, DeathTestWithThreadsWarning) {
    // Проблема: если в процессе есть другие потоки,
    // GTest выдаст предупреждение:
    //   "Death tests use fork(), which is unsafe particularly
    //    in a threaded context."
    //
    // Если ResourceManager уже запущен (initialize() вызван),
    // то fork() скопирует состояние мьютексов/потоков некорректно.

    ResourceManager rm;
    rm.initialize();  // Запустили фоновый поток

    // Death test в этом контексте опасен:
    // fork() создаст копию процесса, но потоки НЕ копируются.
    // Мьютексы могут остаться в locked-состоянии → deadlock.
    //
    // В «threadsafe» режиме GTest перезапустит весь бинарник,
    // но это значительно медленнее.

    // Закомментировано, т.к. может привести к зависанию:
    // EXPECT_DEATH({
    //     rm.criticalOperation();
    // }, "");

    rm.shutdown();

    // Вывод: death tests несовместимы с многопоточным окружением.
    // Для функциональных тестов, где потоки — норма, это серьёзное
    // ограничение.
}

// ============================================================================
// Демонстрация 2: Side effects не видны из death test
// ============================================================================

TEST(DeathTests, SideEffectsLostInChildProcess) {
    int counter = 0;

    EXPECT_DEATH({
        counter = 42;  // Это изменение в ДОЧЕРНЕМ процессе
        std::abort();
    }, "");

    // counter по-прежнему 0 в родительском процессе!
    EXPECT_EQ(counter, 0);

    // Это значит: нельзя использовать death tests для проверки
    // состояния ПЕРЕД аварийным завершением.
    // Например, нельзя проверить, что перед abort() был записан лог.
}

// ============================================================================
// Демонстрация 3: Death test + GMock
// ============================================================================

class ICrashHandler {
public:
    virtual ~ICrashHandler() = default;
    virtual void onPreCrash(const std::string& reason) = 0;
};

class MockCrashHandler : public ICrashHandler {
public:
    MOCK_METHOD(void, onPreCrash, (const std::string& reason), (override));
};

TEST(DeathTests, GMockExpectationsInDeathTest) {
    // Проблема: expectations на мок внутри death test
    // не видны снаружи (они в дочернем процессе).

    MockCrashHandler handler;

    EXPECT_DEATH({
        // Этот EXPECT_CALL живёт в дочернем процессе
        EXPECT_CALL(handler, onPreCrash("fatal error"));
        handler.onPreCrash("fatal error");
        std::abort();
    }, "");

    // Mock::VerifyAndClearExpectations(&handler) здесь
    // ничего не проверит — expectations были в другом процессе.
    //
    // Невозможно комбинировать "проверку вызовов мока"
    // и "проверку что код упал".
}

// ============================================================================
// Демонстрация 4: Порядок запуска death tests
// ============================================================================

// GTest запускает death tests ПЕРВЫМИ (до остальных тестов).
// Это задокументированное поведение, но оно создаёт проблему:
// если death test зависит от state, установленного в SetUpTestSuite(),
// порядок может быть неожиданным.

class MyDeathTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Этот код выполнится перед death tests,
        // но если есть другие test suites, их SetUp ещё не был вызван.
        setup_called_ = true;
    }

    static bool setup_called_;
};

bool MyDeathTest::setup_called_ = false;

TEST_F(MyDeathTest, VerifySetupOrder) {
    // OK: SetUpTestSuite вызван
    EXPECT_TRUE(setup_called_);
}
