# Windows Vulkan kernel-loading layer

The Windows layer/probe are adapted from dniminenn/dlssg-open at commit 1ab86b9fe6b13ac70cf1812fbf0e5c5e6ffb64ac. Preserve LICENSES/MIT-dlssg-open.txt (Copyright 2026 JR Lanteigne).

The layer intercepts vkCreateCuModuleNVX. Compatible CUDA module entries are selected/retargeted in process memory; driver/runtime files are not modified on disk. The validated configuration also needs the external Windows SM86 proxy/backend: this layer alone is not a complete compatibility solution. No NVIDIA cubin/fatbin/runtime payload is included.

The legacy build command and probe retain validated E:/Dev/GTA6 paths. The optional root CMake target can compile just the layer with installed Vulkan headers and MSVC. A local explicit-layer JSON manifest must point to the compiled DLL; do not register it system-wide. Kernel dumping was not used for the published evidence.
