# Independent Vulkan handoff validation

**Contributor:** [@akimfomin](https://github.com/akimfomin) — [PortBridge](https://github.com/akimfomin/PortBridge)

**Date:** 2026-11-24

**Tested revision:** `82f069e82197e61b97f60bbcbcb7a3af7d3f8e77`

| Parameter | Configuration |
|---|---|
| GPU | NVIDIA RTX 4070 Ti SUPER |
| OS | Windows 11 build 28000 |
| Driver | 617.14 |
| Vulkan loader | 1.4.341 |
| Vulkan device API | 1.4.351 |
| Validation layer | 1.3.285 |
| Compiler | Clang 23.1.6 |

## Results

| Test | Result |
|---|---|
| Same-family binary semaphore | PASS |
| Same-family timeline semaphore | PASS |
| Cross-family binary semaphore | PASS |
| Cross-family timeline semaphore | PASS |

- CTest: 1/1 passed.
- Image readback: 64/64 pixels matched in each case.
- Consumer fence completion: successful in all four cases.
- Validation diagnostics: 0 in the successful run.
- Initial run required resolving a missing validation layer.

This independently reproduces the standalone Vulkan handoff mechanism on a second NVIDIA hardware configuration. It does not establish DLSS FG or SR compatibility on this GPU.

Special thanks to @akimfomin and PortBridge for FrameLab's first independent hardware validation.

[Original test report](https://github.com/5headsmartbtw/leonida-framelab/pull/12#issuecomment-7142089405)

[PortBridge continuity pilot](https://github.com/akimfomin/PortBridge/issues/41)
