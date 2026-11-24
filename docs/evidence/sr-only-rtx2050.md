# SR-only Vulkan validation on the reference GPU

GPU: RTX 2050 Laptop, Windows 11 build 26200, NVIDIA 617.14, Vulkan SDK 1.4.363.1, Streamline 2.14.1 and DLSS SDK v310.9.1. A fresh MSVC x64 Release sample build used these external dependencies.

The standalone SR check loads the Streamline SR path without the Streamline FG feature. It avoids the legacy Vulkan buffer-device-address extension with the core feature, declares the Present ID dependency, transitions images after acquisition, uses a depthless overlay pipeline and allocates a Present wait semaphore for each swapchain image.

```powershell
& $exe --validation --verify-sr
```

```text
VERIFY_SR successes=120 errors=0 gpu_idle=0 output=1280x720 passed=1
SAMPLE_VALIDATION_SUMMARY errors=0 warnings=0
```

On the same build, the existing FG runner passed the Immediate, FIFO and missing-depth recovery cases. The preserved synthetic readback matched SHA-256 `caa4a0256f6920a8fc596fc414680d9544b72abff5c7f6e3c148a6d6924d96a2`. SR visual readback, SR swapchain fault injection, multi-queue and combined SR + FG were not run.
