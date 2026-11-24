# Independent D3D12 baseline

These are the earlier synthetic direct-NGX FG and capability probes, plus a DXGI test presenter. They precede the API-neutral FrameInputs refactor and are not consumers of the new contract. Their .cmd build files retain the original validated E:/Dev/GTA6 dependency layout.

CreateFeature/Evaluate, output readback and generated/real Present were validated on RTX 2050. Streamline supplies Reflex/PCL in the presenter; FG computation is direct NGX. External SDK headers/libraries, proxy/runtime DLLs and kernel packs are intentionally absent. See the repository status matrix and evidence before generalizing these results to a game.
