# FrameInputs: контракт версии Phase 3

[English](CONTRACT.en.md)

Авторитетный новый контракт находится в `vk_streamline/fg/FrameInputs.hpp`. Старый `phase2/FrameInputs.hpp` изучен и оставлен как исторический Vulkan-зависимый проект, без изменения baseline. Общий header требует только стандартную библиотеку C++20. Публичные поля не содержат Vk/D3D/SL/NGX типов или платформенных handles.

## Общая часть

| Данные | Представление и правила |
|---|---|
| Идентичность | stream/device/generation — непрозрачные числовые IDs в пространстве producer; frameIndex — индекс исходного кадра, не количество generated frames |
| Изображения | color/depth/motionVectors, optional HUD-less/UI; ID, format, extent, frameIndex, абстрактное state, queue ownership ID и происхождение unavailable/synthetic_renderer/verified_engine_binding |
| Размеры | renderExtent и outputExtent; color соответствует output, depth/MV — render. Текущий Vulkan consumer дополнительно требует равных render/output |
| Camera | пять row-major матриц без jitter: viewToClip, inverse, lens, current→previous clip и inverse; position/up/right/forward, near/far/FOV/aspect, отдельные jitterPixels и MV scale, conventions, depth inversion, camera-motion inclusion |
| Время/история | истинный deltaSeconds, resetHistory с initial/resize/camera_cut/reactivated/discontinuity; resized/deviceLost. Sample для backend reset и паузы >250 мс использует discontinuity, generation описывает пересоздание ресурсов |
| Lifetime | shared allocationLease должен владеть реальной allocation/views и нужным allocator. ID/сырой handle сами по себе не продлевают жизнь |
| Synchronization | same_command_stream, host_complete, binary_signal, timeline_signal; queue/command-stream/signal IDs, timeline value и lease внешнего primitive. Наличие типа в общей схеме не означает его реализацию в consumer |

Семантика v1: color — scene-linear RGB; depth — post-projection depth в [0,1] с направлением из depthInverted; MV — previous-minus-current в UV с явным scale. Произвольный R32 или RG image не удовлетворяет этим семантикам только по формату. Перевод линейной view-space depth, другой MV convention и цветовых пространств должен выполнять/подтверждать producer до передачи. HDR/colorimetry, массивы/мультисэмплинг и произвольные subresources этим consumer не реализованы.

`validate` проверяет missing semantics, нулевые dimensions/IDs, format/semantic mismatch, frame mismatch, lease, неизвестное состояние/владение, finite camera/time, ненулевые матрицы/basis и описанную completion. Это структурная проверка, не доказательство физически верной камеры, обратимости всех матриц или истинности producer provenance.

`Sequence::check` дополнительно проверяет монотонный frame index/generation, reset первого/пропущенного кадра, смену ресурсов/размеров/lease owner с новым generation и reset. Snapshot содержит weak owner identities и не удерживает старые allocations. `accept` вызывается только после успешного NGX recording; неуспех/разрыв требует invalidate. Failed input не продвигает sequence.

## Vulkan-часть и поддержанный профиль

`FrameInputsVulkan.hpp` связывает IDs с VkDevice/VkQueue/семейством, producer VkCommandBuffer и image/view/format/layout/aspect/extent. Перед первой barrier проверяются identity, dimensions, format, layout, view aspect, aliasing и владение очередью. Native extent берётся producer из существующего allocation/GBuffer, не угадывается consumer по opaque handle.

Поддержан только один и тот же producer/consumer command buffer, command-stream token, device и queue/family; color RGBA16F в SHADER_READ_ONLY_OPTIMAL, depth D24S8 в DEPTH_STENCIL_ATTACHMENT_OPTIMAL с depth view, MV RG16F в COLOR_ATTACHMENT_OPTIMAL. Все входы — полные single-sample 2D кадры; SR off, optional UI/HUD-less inputs отклоняются. Backend сохраняет baseline-пару camera conventions: Vulkan Y-down projection и D3D-oriented Y-up reprojection. Другие пары отклоняются; универсальная конвертация здесь не реализована.

host_complete, binary/timeline signal и межсемейные transfers пока отвергаются даже при структурно корректном общем контракте. Нельзя объявлять producerCommands равным текущему command buffer, если реальный producer находится в другом submission. Для Leonida сначала потребуется новый проверенный путь синхронизации.

## Реальная передача в sample

main.cpp формирует пакет из существующих GBuffer и camera constants после рендера. Извлечение из sl::Constants ограничено producer; DirectNgxFG больше не принимает Streamline types. Переносятся те же численные camera параметры, что у baseline. generation увеличивается при пересоздании input images; source index растёт и во время FG off/fallback.

SampleFrameOwner содержит shared GBuffer и allocator; порядок удаления гарантирует views/images раньше allocator. DirectNgxFG копирует FrameInputs в inFlightInputs после успешного recording. После фактического vkQueueWaitIdle вызывается onGpuCompleted: проверяется readback, затем strong leases отпускаются. Дальнейший real Present также защищён собственным владельцем GBuffer и существующим ожиданием очереди. Device/context живут дольше всех packet consumers; shutdown/resize дожидаются GPU. Это схема одного кадра в полёте, не готовая многопоточная система без ожиданий.

При invalid input нет NGX call и нет новых image barriers. Применяется обычный real Present, sequence invalidates, следующий корректный пакет должен reset history. Отдельный guard не позволяет перезаписать ранее удержанный packet до GPU completion. Неуспех NGX сохраняет существующее отключение FG; device-lost recovery не добавлялся.
