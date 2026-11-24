# Phase 3 — FrameInputs и подготовка Leonida FG

Проверено 24 ноября 2026. Изменения находятся только в vk_streamline, ветка `experiment/phase3-frame-inputs`, от baseline `a175d35f01a7c172429f79af27b6c6d43704b6b9`. Leonida `8575d36edc479218972497a7bded8651a42856c0` не изменён. Push/PR не выполнялись, бинарные зависимости NVIDIA не распространяются, установленное окружение не менялось.

## Результат

- Реализован переносимый C++20 контракт `fg/FrameInputs.hpp` без типов и заголовков графических API/NGX/Streamline.
- Vulkan-привязки вынесены в `fg/FrameInputsVulkan.hpp`. DirectNgxFG принимает пакет FrameInputs вместо набора сырых image handles и sl::Constants.
- Sample создаёт пакет из собственных color/depth/MV/camera. Allocation lease удерживает настоящие GBuffer и allocator до GPU completion. Пакет с отсутствующими данными отклоняется до NGX/barriers с сохранением real Present и reset при восстановлении.
- CPU: 2/2 CTest, 59 проверок (43 общего контракта, 16 Vulkan-привязок). GPU: VSync Off/On и missing-depth fallback прошли с validation 0/0. Контрольный RGBA16F кадр побайтно совпал с baseline.
- Живая проверка VSync, resize и выключения/повторного включения FG: 10605 исходных кадров, 7380 Evaluate, 7375 generated Present, ошибок FG=0, rejected inputs=0, validation errors=0 warnings=0; незавершённых packet leases нет.

## Что установлено в Leonida

Доступны generic image metadata, color Present, depth attachments, root constants/CBV, layout tracker, очереди/timelines и механизмы отложенного удаления. Не найдена подтверждённая привязка финального color к игровым depth/MV/camera/jitter. Raw ResourceBinding не предоставляет FG allocation lease, а существующий consumer пока не поддерживает внешние producer waits и межочередные ownership transfers. Поэтому включать FG в Leonida сейчас нельзя без угадывания данных.

Читайте [CONTRACT.md](CONTRACT.md), [LEONIDA_AUDIT.md](LEONIDA_AUDIT.md), [VALIDATION.md](VALIDATION.md), [INTEGRATION_PLAN.md](INTEGRATION_PLAN.md). Исходники, тесты и эти отчёты сохранены в отдельной локальной ветке. Точный конечный commit и SHA256 находятся в DELIVERY_MANIFEST.json, создаваемом при упаковке.

## Сохранённый baseline

Git ref `baseline/phase2-validated-a175d35` указывает на `a175d35f01a7c172429f79af27b6c6d43704b6b9`. Локальная копия EXE: E:\Dev\GTA6\phase3\baseline\vk_streamline-a175d35.exe, SHA256 `6029f3d818faf2931296f54cdab1cf62c132ef536b75f476fe5996b30d23dce9`. Это снимок для восстановления в прежней bin_x64/Release с уже имеющимися DLL, не автономный дистрибутив. Альтернатива — переключить сохранённую baseline-ветку и собрать существующим окружением. Сначала закрыть работающий sample. DLL в резервную копию/архив не добавлялись.
