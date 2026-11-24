# Attribution and licensing map

Copyright (c) 2026 5headsmartbtw for original Leonida FrameLab code and documentation. Original contributions are MIT-licensed; retain LICENSE when reusing them.

| Component | Origin | License / attribution |
|---|---|---|
| Vulkan sample, wrapper, CMake framework use, synthetic GLSL scene | NVIDIA nvpro-samples/vk_streamline, upstream 02fe6f70dfc9ce809cb8bca312fe26c8a69a3cd9 | Apache-2.0; original NVIDIA file headers retained; LICENSES/Apache-2.0.txt |
| Windows Vulkan layer and adapted native probe | dniminenn/dlssg-open, 1ab86b9fe6b13ac70cf1812fbf0e5c5e6ffb64ac | MIT, Copyright (c) 2026 JR Lanteigne; LICENSES/MIT-dlssg-open.txt |
| FrameInputs, binding validation, tests, new project documentation | 5headsmartbtw / this project | MIT; LICENSE |
| Direct NGX experimental code | This project, informed by the dlssg-open MIT probe | Original MIT notice preserved in LICENSES/MIT-dlssg-open.txt; new contributions MIT |

The imported sample is modified: SR/PCL diagnostics, direct NGX FG, VSync, safe swapchain initialization, FrameInputs and lease validation. The root MIT license does not erase existing Apache-2.0/MIT requirements for imported portions. This repository does not claim ownership of third-party code.

NVIDIA DLSS/NGX/Streamline SDKs, runtime DLLs and NVIDIA kernel payloads are external dependencies with their own terms. The inspected SM86 binary distribution did not provide a standalone license file; it is not redistributed here. No game data, carved game shaders, secrets or driver binaries are included. NVIDIA and game/project names identify interoperability targets; this is an independent research project.

Development note: the new code and documentation were prepared with AI assistance. Hardware results are limited to the actual local checks recorded in the evidence, not inferred compatibility claims.
