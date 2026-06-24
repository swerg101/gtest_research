# gtest_research

Репозиторий с демонстрационным кодом к статье **«Анализ архитектурных ограничений GMock в контексте runtime expectations»**.

## О чём

Статья анализирует три архитектурных ограничения Google Mock (GMock), препятствующих тестированию асинхронного кода:

1. **Замороженные matchers** -- значение matcher фиксируется при объявлении `EXPECT_CALL`, а не при вызове мока
2. **Грубая гранулярность сброса** -- `VerifyAndClearExpectations` уничтожает все expectations разом
3. **Отсутствие условного ветвления** -- невозможно описать альтернативные пути поведения

Для каждой проблемы представлены тесты на трёх фреймворках: GMock, Trompeloeil и FakeIt. Также предложены два решения: минимальное расширение `.When()` и концептуальная модель State Machine.

## Структура проекта

```
gtest_research/
├── MODULE.bazel                    # Bzlmod: зависимости (googletest)
├── BUILD.bazel                     # Root package
├── src/
│   ├── BUILD.bazel
│   └── async_service.h             # AsyncService + интерфейсы IEventObserver, ITransport
├── tests/
│   ├── gmock/
│   │   ├── BUILD.bazel
│   │   ├── test_runtime_expectations.cpp   # П1, П2, П3 на GMock + .When() скетч
│   │   ├── test_async_problems.cpp         # * смежные проблемы (см. ниже)
│   │   ├── test_death_tests.cpp            # * смежные проблемы (см. ниже)
│   │   └── test_gmock_api_problems.cpp     # * смежные проблемы (см. ниже)
│   ├── trompeloeil/
│   │   ├── BUILD.bazel
│   │   └── test_runtime_expectations.cpp   # П1, П2, П3 на Trompeloeil
│   └── fakeit/
│       ├── BUILD.bazel
│       └── test_runtime_expectations.cpp   # П1, П2, П3 на FakeIt
├── fakeit/                         # FakeIt single-header (vendored)
├── trompeloeil/                    # Trompeloeil (vendored)
└── googletest/                     # Google Test/Mock (vendored)
```

## Требования

- Bazel >= 7.0 (или [Bazelisk](https://github.com/bazelbuild/bazelisk) -- рекомендуется)
- C++17-совместимый компилятор (GCC 9+, Clang 10+, MSVC 2019+)

## Сборка и запуск

```bash
cd gtest_research

# Собрать всё
bazel build //...

# Запустить все тесты
bazel test //tests/...

# GMock тесты
bazel test //tests/gmock:test_runtime_expectations --test_output=all

# Trompeloeil тесты
bazel test //tests/trompeloeil:test_runtime_expectations --test_output=all

# FakeIt тесты
bazel test //tests/fakeit:test_runtime_expectations --test_output=all
```

## Ожидаемое поведение тестов

### GMock (`tests/gmock/test_runtime_expectations.cpp`)

| Тест | Результат | Проблема |
|------|-----------|----------|
| `CannotUpdateExpectationAfterCall` | PASS | Обходной путь с `_` -- теряется точность |
| `VerifyAndClearIsAllOrNothing` | PASS | Демонстрация грубого сброса |
| `ConditionalExpectationWorkaround` | PASS | Обходной путь с InSequence |
| `DynamicId_ExpectationFrozenAtSetup_FAILS` | **FAIL** | П1: matcher заморожен, `.After()` не помогает |
| `PhaseExpectation_ViaInvoke_FAILS` | **DEADLOCK** | П2: `EXPECT_CALL` внутри `Invoke` -- deadlock на `g_gmock_mutex` |
| `PhaseExpectation_SetAfterActualCall_FAILS` | **FAIL** | П2: expectation после фактического вызова |
| `ConditionalBranch_WrongAssumption_FAILS` | **FAIL** | П3: неверное предположение о ветке |
| `ConditionalBranch_WithWhen` | PASS* | Решение П3 через предлагаемый `.When()` |

\* Требует модифицированный GMock с расширением `.When()`.

### Trompeloeil (`tests/trompeloeil/test_runtime_expectations.cpp`)

| Тест | Результат | Проблема |
|------|-----------|----------|
| `DynamicId_ExpectationFrozenAtSetup` | PASS | П1: решена через 2-фазный подход + RAII |
| `ScopedExpectations_P2` | PASS | П2: решена через RAII-скоупы |
| `ConditionalBranch_Strict_FAILS` | **FAIL** | П3: строгие ожидания падают как в GMock |
| `ConditionalBranch_AllowBothPaths` | PASS | П3: ALLOW_CALL проходит, но теряет строгость |

### FakeIt (`tests/fakeit/test_runtime_expectations.cpp`)

| Тест | Результат | Проблема |
|------|-----------|----------|
| `DynamicId_ExpectationFrozenAtSetup` | PASS | П1: решена через постфактум-верификацию |
| `VerifyAndClearIsAllOrNothing` | PASS | П2: ClearInvocationHistory гранулярнее GMock |
| `ConditionalBranch_PostFactum` | PASS | П3: постфактум-верификация без строгости |
| `ConditionalBranch_ManualVerification` | PASS | П3: ручная if/else верификация |

## Прочие тесты GMock

Помимо `test_runtime_expectations.cpp`, в каталоге `tests/gmock/` находятся дополнительные тесты:

- `test_async_problems.cpp` -- проблемы GMock при тестировании многопоточного и асинхронного кода
- `test_death_tests.cpp` -- ограничения death-тестов в контексте моков
- `test_gmock_api_problems.cpp` -- прочие шероховатости API GMock

Эти тесты фиксируют смежные проблемы, которые выходят за рамки статьи о runtime expectations, но также представляют интерес для дальнейшего исследования.

## Связь со статьёй

Каждый тест в `test_runtime_expectations.cpp` (GMock, Trompeloeil, FakeIt) соответствует листингу в статье. Названия тестов (`TEST(Suite, Name)`) совпадают с названиями в тексте. Комментарии в коде объясняют, какую проблему демонстрирует тест и какой результат ожидается.
