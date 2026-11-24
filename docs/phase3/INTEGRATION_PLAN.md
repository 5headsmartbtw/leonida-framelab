# Конкретный план дальнейшего подключения Leonida

[English](INTEGRATION_PLAN.en.md)

Этот план не исполняется в Phase 3. Backend Leonida остаётся без изменений. Критерий перехода — подтверждённые данные и completion/lifetime, а не успех capability query.

1. **Довести независимый consumer до реальной handoff-синхронизации.** Реализовать timeline/binary wait path и необходимые queue-family acquire/release на synthetic multi-queue harness. IDs разрешать через авторитетные Vulkan allocations и semaphores, не через касты случайных значений. Тесты: разные command buffers/queues/families, failed submit, reuse, cancelled frame, repeated resize, resource release до/после fence. Сейчас эти случаи fail closed. Это нужно сделать до изменения Leonida, потому что same-command-buffer profile sample не соответствует отдельному producer replay и presenter recording.

2. **Определить semantic provider.** В отдельной согласованной будущей фазе исследовать известные game/renderer bindings без публикации payloads. Для одного frame index подтвердить final color, final-compatible depth, previous-minus-current MV и scale, jitter, unjittered matrices, near/far/FOV/depth direction, UI/HUD-less availability. Провести camera/static-object/moving-object/camera-cut контроль. Пока хотя бы один обязательный input не подтверждён, provenance=unavailable и FG off. Выбор «последнего depth attachment» или любого RG16F не является реализацией этого шага.

3. **Ввести настоящий ownership epoch/lease.** Будущий integration seam должен pin-ить реальный COM Resource/device/view owner (или эквивалент native allocation owner) и точный generation, а не копию ResourceBinding. Подтвердить GPU completion всех пользователей: NGX, real/generated presentation и history. Связать release с действительными queue timeline/fence tickets и existing deferred-deletion accounting; не продлевать срок неизвестным '+1 frame'. Отдельно протестировать held/deferred presents и ResourceId reuse.

4. **Получить completion-aware packet на общей границе Present.** Кандидат: VulkanPresenter::submit_now перед final blit или compose/copy, после resolve текущих resources. Использовать actual producer chain, context tracker и lock hierarchy resource_mutex→tracker_mutex→submit_mutex. Внешние waits выполнять без взаимоблокировки с producer Signal. Сохранять layout rollback, failure/give_back acquired image и обычный Present. Blank/minimised/stale/surface-lost frames должны bypass FG и invalidate history. Не создавать новые generic image layouts параллельно existing LayoutTracker.

5. **Согласовать Xbox/DXGI и pacing.** WindowPresenter/DXGI и XboxPresentation plain/planes должны поступать в один semantic policy. Уважать source/dest rects, composition, HDR/SDR и FramePacer suspension/token. generated frame не становится новой игровой симуляцией и не требует нового игрового frame token. Проверить отдельный real/render FPS, API present throughput, pacing на дисплее и latency; тестовый FIFO/Immediate loop sample не является production pacer.

6. **Минимальное включение только после gate-условий.** Начать с native SDR 720p, FG2X, известного single-sample profile и минимального in-flight. Проверить реальные allocation budgets, release/reset на resize/device loss, отказ памяти/NGX и немедленный обычный Present. Повторить image/readback tests, Vulkan validation и нужные узкие Leonida tests на изменённых местах. Полные suite запускать лишь при обоснованном влиянии, не автоматически.

## Точный критерий готовности

Пакет имеет подтверждённые semantics, реально владеющие leases, проверенные native state/extent/format и producer completion; consumer поддерживает этот handoff; generated output проверен и корректно представлен без нарушения token/lock/lifetime. До этого есть рабочий sample и подготовленный контракт, но нет готового игрового FG backend.

## Совместимость и неразрешённые вопросы

Новый нейтральный контракт расширяем, но renderer-specific discovery не реализован. Vulkan consumer пока намеренно ограничен tested native-size RGBA16F/D24S8/RG16F и одной командной записью; D3D12 consumer нового контракта не написан. Другой depth format (включая native D32S8 fallback Leonida), HDR, SR scaling, arrays/MSAA/subresources и UI plane требуют explicit conversions/profiles и тестов.

Baseline camera convention pair сохранена для отсутствия численного regression; калибровка произвольной game camera остаётся отдельной задачей. Frame delta хранится/валидируется и участвует в reset после паузы; новая input-latency model не создана. Потокобезопасность нескольких producers/consumers и asynchronous retirement без QueueWaitIdle не заявляются.

Original Streamline Vulkan FG plugin по-прежнему может сообщать AdapterUnsupported. Его capability result не обходился и не заменялся eOk. Direct NGX — самостоятельный consumer, использующий реальную NGX capability query. Ни новая версия NVIDIA DLL, ни бинарный патч не являются частью этого плана.
