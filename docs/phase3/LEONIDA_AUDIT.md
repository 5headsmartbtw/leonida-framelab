# Leonida Vulkan audit — только чтение

Аудит статического исходного кода commit `8575d36edc479218972497a7bded8651a42856c0`. Никакая игра/игровой frame capture не запускались; игровые данные, shader dumps или ресурсные payloads не исследовались. Проверка относится к этому checkout, не к текущему upstream или любому игровому кадру.

| Требование | Что существует в коде | Чего пока нет для FG |
|---|---|---|
| Color | VulkanPresenter::present принимает ResourceId и разрешает color image; present_planes принимает до двух color planes, source/destination rectangles, HDR/composition параметры | Нет semantic mapping color→соответствующие финальные depth/MV/camera. Plane 1 нельзя автоматически назвать UI, а plane 0 — HUD-less |
| Depth | Форматы/aspects/mips/layers и native image в ResourceBinding; CreateDepthStencilView, OMSetRenderTargets сохраняют depth view; replay записывает depth attachments | Не установлено, какой depth принадлежит финальному post-processed color и остаётся пригодным к этому frame index |
| Motion vectors | Backend умеет generic RG textures, render targets, descriptors и shader replay | Не найден explicit MV semantic binding/contract. Наличие RG16F текстуры не доказывает velocity, её units, знак, camera-motion inclusion или jitter policy |
| Camera/jitter | Root constants/CBV/descriptor-table addresses регистрируются и replay-ятся | Нет подтверждённой схемы matrices/camera/jitter/near/far/fov с привязкой к Present. Наличие constant-buffer bytes не определяет их семантику |
| Layouts | LayoutTracker с per-resource/subresource состоянием, COMMON/GENERAL mapping, barriers и rollback при failed submission | FG должен участвовать в том же tracker/rollback протоколе; отдельный независимый tracker/угаданный layout недостаточен |
| Ownership | COM Resource управляет VkImage/VkDeviceMemory; context caches views; deferred deletion/timeline retirement уже есть | ResourceBinding — plain metadata, без владеющего resource lease. Для отложенного FG нужен explicit pin реальных Resource/device/views до завершения всех потребителей |
| Synchronization | VkQueues, submit_mutex, progress timelines, queue waits/signals, held batches; presenter имеет acquire/ready semaphores и frame fences | Нет экспортированного FG completion contract для color/depth/MV. Sample same-command-buffer consumer не соответствует отдельным renderer/presenter submissions |
| Present/frame metadata | WindowPresenter связывает frontend resource с VulkanPresenter; XboxPresentation имеет FramePacer/present token, planes, suspension | Нет общего packet с resource generation, FG reset/camera semantics; дополнительный generated Present не должен расходовать новый игровой token или ошибочно увеличивать rendered-frame index |

## Проверенные места исходников

- `runtime/src/vulkan_replay.h:59`: ResourceBinding — image/format/aspects/dimensions/mips/layers/samples/buffer/VA и flags; **не** владеющая ссылка, camera или semantic name. `:397` image_layout учитывает simultaneous/general-state policy.
- `runtime/shims/d3d12_com.cpp:312–325`: graphics root constants/CBV/descriptor tables. `:393`: OMSetRenderTargets копирует target/depth descriptors. Эти записи описывают bindings конкретного draw, но не выбирают конечные FG inputs.
- `runtime/shims/d3d12_vulkan_device.cpp:6021`: CreateDepthStencilView. `:257` и `:985` — format mapping и D24 fallback; native depth может отличаться от исходного DXGI формата, что важно для NGX resource metadata.
- `runtime/shims/d3d12_vulkan_device.cpp:1716`: Resource destructor удаляет context binding и откладывает уничтожение image/buffer/memory. `:1731`: ResourceId производится из resource object identity; отдельный FG generation epoch ещё нужен, IDs могут переиспользоваться после удаления.
- `runtime/shims/d3d12_com.cpp:1381`: pin_lists удерживает PSO/root signatures, **не ресурсы**: по комментарию существующий D3D12 caller держит их до fence. Новый FG consumer не может молча полагаться на дополнительный срок после исходного fence.
- `runtime/src/vulkan_backend.h:1161`: remove_resource убирает table entry, откладывает views и забывает tracker. `:1419` defer_destroy, `:1479` record_submit_locked связывают retirement с queue timeline tickets. `:3036` VulkanQueue::submit; `:3111` progress timeline; `:3137` ticketed pins.
- `runtime/src/vulkan_scratch.h:74`: retire(timeline,ticket), `:95` collect. Это transient scratch; FG temporal history не должна освобождаться вместе с одноразовым scratch.
- `runtime/src/vulkan_present.h:228`: plain color present. `:263`: plane composition. `:350`: submit, `:408` run_deferred повторно разрешает ResourceId; pending queue не является FG resource lease. `:422` submit_now.
- `runtime/src/vulkan_present.h:486–528`: resource_mutex → tracker_mutex, разрешение текущих bindings/layouts. `:529` rollback. `:574–579` vkQueueSubmit / vkQueuePresentKHR под submit_mutex. `:683–733`: обычный blit, переход source в TRANSFER_SRC и восстановление прежнего layout. FG следует рассматривать до окончательного blit/composition-copy при полностью известных inputs.
- `runtime/shims/d3d12_vulkan_device.cpp:8521`: WindowPresenter::present принимает back_buffer и передаёт ResourceId. `:8637` создаёт VulkanPresenter на info.queue/info.queue_family. `:8699` standalone device setup, `:9354` vkCreateDevice — будущие extension requirements должны быть согласованы до этой точки.
- `runtime/shims/xbox_present.cpp:240` и `:271`: console planes/plain present, затем commit_present_token. DXGI/PresentX/blank/compose ветки должны иметь согласованную future FG policy; одной подмены DXGI Present недостаточно.

Номера приведены для закреплённого commit; точные SHA256 проверенных source files находятся в AUDIT_SOURCE_MANIFEST.json. Диапазоны выше описывают блоки, а не новые изменения.

## Очереди: нельзя предполагать один VkQueue

У presenter есть собственная очередь, а комментарий обычного blit описывает порядок прежней работы в ней. При этом actual device creation (`d3d12_vulkan_device.cpp:9025–9058`) создаёт несколько command queues и выбирает dedicated compute/copy families с sparse-capability fallback. `vulkan_backend.h:2446` содержит специальные waits для bindless/storage uploads. Поэтому утверждение «любой producer всегда находится на том же queue/family, достаточно одной image barrier» аудитом не подтверждается. Нужны реальная producer chain, completion values и ownership resolution конкретных inputs.

## Где начинается будущая интеграция

Первый кандидат — управляемая граница в VulkanPresenter::submit_now до record/blit/composition после разрешения actual ResourceBindings, с сохранением resource→tracker→submit lock order и rollback. Это **кандидат**, не готовый FG hook: camera/MV/depth semantics и их lifetime/completion пока отсутствуют. Блокировка ожидания чужой очереди под этих mutex недопустима: Signal может зависеть от той же CPU/queue chain. Для held/deferred presents packet нужно либо формировать в момент фактической отправки, либо заранее pin-ить точный resource generation и семантические inputs.

Никаких bindless slots, GPU VA или camera offsets игры этот аудит не выдумывает. Из наличия renderer plumbing не следует готовность игровых FG inputs. Все неидентифицированные semantic inputs остаются unavailable, поэтому FG в Leonida остаётся выключенным.

## Дополнительная проверка camera symbols

Поиск по всему runtime/src и runtime/shims обнаружил mods::CameraView в runtime/src/mods.h:57 и использование в main.cpp:138. Это настройка enum first-person/default view mode, а не GPU camera matrices/jitter или association с представленным color. Эти symbols не закрывают missing camera input для FG.
