# Сборка проекта (Bazel)

## Требования
- Bazel >= 7.0 (или Bazelisk — рекомендуется)
- C++17-совместимый компилятор (GCC 9+, Clang 10+, MSVC 2019+)

## Установка Bazel

```bash
# Через Bazelisk (рекомендуется — автоматически подтягивает нужную версию Bazel)
# Linux:
wget https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64 -O ~/bin/bazel
chmod +x ~/bin/bazel

# macOS:
brew install bazelisk
```

## Сборка и запуск

```bash
cd gtest_research

# Собрать всё
bazel build //...

# Запустить все тесты
bazel test //tests/...

# Запустить конкретный тест
bazel test //tests:test_runtime_expectations
bazel test //tests:test_async_problems
bazel test //tests:test_death_tests
bazel test //tests:test_gmock_api_problems

# Verbose output (показать stdout/stderr даже при success)
bazel test //tests:test_runtime_expectations --test_output=all

# Запустить конкретный test case внутри бинарника
bazel test //tests:test_runtime_expectations --test_arg=--gtest_filter='RuntimeExpectations.*'
```

## Структура проекта

```
gtest_research/
├── MODULE.bazel              # Bzlmod: зависимость googletest из BCR
├── BUILD.bazel               # Root package
├── .bazelrc                  # Флаги компиляции (C++17, warnings)
├── src/
│   ├── BUILD.bazel           # cc_library: async_service (header-only)
│   └── async_service.h
├── tests/
│   ├── BUILD.bazel           # cc_test для каждой демонстрации
│   ├── test_runtime_expectations.cpp
│   ├── test_async_problems.cpp
│   ├── test_death_tests.cpp
│   └── test_gmock_api_problems.cpp
└── docs/
```
