# Validation Phase 3

[English](VALIDATION.en.md) · [Актуальная сборка и GPU-тесты](../BUILD.md)

Windows 11 build 26200, RTX 2050 Laptop / SM86, NVIDIA driver 617.14 (nvidia-smi), MSVC 2022. Сохранены Streamline SDK 2.14.1, NVIDIA/DLSS v310.9.1 headers/API 0x15, Bundled DLSS-G runtime 310.9.16, SM86 mod 0.4.2 и существующий Windows cubin layer. Никаких установок/обновлений SDK, драйвера или системных настроек не выполнялось. App-local environment остаётся прежним.

## CPU

Два standalone CTest targets: portable `frame_inputs_test` не включает графические/vendor headers, `frame_inputs_vulkan_test` проверяет typed bindings без Vulkan device или NGX calls. Release, 2/2 PASS; 43+16=59 проверок. Missing depth/MV/lease, NaN/camera, stale frame, dimensions/format/state, sync primitives, history/generation/index continuity, смена lease owner, сохранение lifetime копией пакета, wrong device/queue/command buffer/layout/aspect/native extent и unsupported profiles. Tests используют собственный CHECK, а не отключаемый Release assert.

## GPU с readback и validation

| Сценарий | Исходных Present | NGX Evaluate | Проверенных интерполяций / generated Present | Rejected | Validation |
|---|---:|---:|---:|---:|---|
| VSync Off / Immediate | 6 | 6 | 5 / 5 | 0 | 0 errors / 0 warnings |
| VSync On / FIFO | 6 | 6 | 5 / 5 | 0 | 0 errors / 0 warnings |
| Missing depth на source frame 2 | 6 | 5 | 3 / 3 | 1 ожидаемый | 0 errors / 0 warnings |

В negative test нет FRAMEINPUTS_CONSUMED для source frame 2; source frame 3 приходит с reset=1. Счётчик NGX evaluations отличается от source-frame index и не подменяет его. Первый reset-кадр не считается интерполяцией. После GPU completion in_flight=0 во всех случаях. Log пары retained/released показывают срок удержания packet; CPU lifetime test отдельно проверяет реальное освобождение shared owner. Одна проверка image/frame API metadata сама по себе не доказывает правильность произвольного внешнего VkImage handle.

Контрольный generated-5.rgba16f baseline и Phase 3 имеют один SHA256: `caa4a0256f6920a8fc596fc414680d9544b72abff5c7f6e3c148a6d6924d96a2`. Это один детерминированный sample output, не обещание качества на всех сценах.

## Живые переключения и resize

Под validation проверены V key: Immediate→FIFO, checkbox: FIFO→Immediate, resize 1280×720→1920×1009, FG disable→enable. При выключенном FG real counter вырос с 5970 до 7499, generated остался 5876; после включения generated снова рос (6015 на следующем наблюдении). При resize новый generation и reset позволили продолжить Evaluate. Итог: 10605 source frames, 7380 Evaluate, 10605 real и 7375 generated API Present, FG errors=0, input rejections=0, in_flight=0. Validation 0 errors / 0 warnings, stderr пуст.

В этой длинной серии нет readback verification каждого кадра, поэтому verified=0 означает отключённый диагностический readback, а не неработающий FG. Проверки изображений выполнены отдельно в bounded tests. API Present counts не являются scanout/latency measurements; FPS gain и input latency в этой фазе не измерялись.

## Воспроизведение

Ниже зафиксированы исторические команды исходного checkout. Для публичного репозитория используйте [текущую инструкцию](../BUILD.md), где путь к EXE, каталог результатов и необязательный контрольный кадр задаются явно.

```powershell
. E:\Dev\GTA6\Enter-Dev.ps1
& $env:GTA6_CMAKE -S E:\Dev\GTA6\vk_streamline\tests -B E:\Dev\GTA6\build\phase3-contract-tests -G 'Visual Studio 17 2022' -A x64
& $env:GTA6_CMAKE --build E:\Dev\GTA6\build\phase3-contract-tests --config Release --parallel 2
& $env:GTA6_CTEST --test-dir E:\Dev\GTA6\build\phase3-contract-tests -C Release --output-on-failure
& $env:GTA6_CMAKE --build E:\Dev\GTA6\build\vk_streamline --config Release --parallel 2
& E:\Dev\GTA6\vk_streamline\tests\run_gpu_checks.ps1
```

Перед сборкой закрыть sample. GPU script ограничивает каждый процесс 60 секундами, проверяет stdout/stderr/counters, baseline SHA и сохраняет gpu-results.json. Все входы — собственная synthetic scene. Нужен сохранённый baseline frame по пути phase3/baseline/generated-5.rgba16f; он локальный, не часть дистрибутива SDK. Logs/evidence приложены; полный дорогостоящий набор Leonida повторно не запускался, поскольку Leonida не менялся.

Проверки не покрывают device-lost recovery, долгий memory-pressure soak, HDR/SR+FG, другие GPU/runtime, arbitrary queue transfers или игровые inputs. Реальный отказ драйвера/NGX fault-injection не выполнялся; sticky runtime-error fallback сохранён из baseline, новый metadata fallback реально проверен.
