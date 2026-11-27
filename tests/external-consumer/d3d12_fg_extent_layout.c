/* SPDX-License-Identifier: MIT */
/* A client compiled against the D3D12 FG header whose config ends at render_extent (before
 * color_format and flags were appended). The config is placed directly before an inaccessible
 * page, so reading past struct_size faults. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <framelab/d3d12_fg.h>

#include <stddef.h>
#include <string.h>

/* FrameLabD3D12FgConfig with render_extent and without the color fields. */
typedef struct ExtentConfig {
    uint32_t struct_size, abi_version;
    uint64_t device_id, queue_id;
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    ID3D12Fence* completion_fence;
    FrameLabExtent extent;
    const char* project_id;
    const char* engine_version;
    const wchar_t* ngx_runtime_directory;
    const wchar_t* log_directory;
    FrameLabD3D12InitReportFn report;
    void* report_user;
    FrameLabExtent render_extent;
} ExtentConfig;

#define SAME(field) (offsetof(ExtentConfig, field) == offsetof(FrameLabD3D12FgConfig, field))
_Static_assert(SAME(struct_size) && SAME(abi_version) && SAME(device_id) && SAME(queue_id) &&
                   SAME(device) && SAME(queue) && SAME(completion_fence) && SAME(extent) &&
                   SAME(project_id) && SAME(engine_version) && SAME(ngx_runtime_directory) &&
                   SAME(log_directory) && SAME(report) && SAME(report_user) && SAME(render_extent),
               "the render_extent prefix of FrameLabD3D12FgConfig moved");
_Static_assert(offsetof(FrameLabD3D12FgConfig, color_format) == offsetof(ExtentConfig, render_extent) + sizeof(FrameLabExtent) &&
                   sizeof(ExtentConfig) < sizeof(FrameLabD3D12FgConfig),
               "color_format and flags must be appended after render_extent");

int main(void)
{
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    const SIZE_T page = system.dwPageSize;
    unsigned char* pages = (unsigned char*)VirtualAlloc(NULL, 2 * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD previous = 0;
    if (!pages || !VirtualProtect(pages + page, page, PAGE_NOACCESS, &previous)) return 1;
    ExtentConfig* old = (ExtentConfig*)(pages + page - sizeof(ExtentConfig));
    memset(old, 0, sizeof *old);
    old->struct_size = (uint32_t)sizeof *old;
    old->abi_version = FRAMELAB_ABI_VERSION;
    old->extent.width = 1280;
    old->extent.height = 720;
    old->render_extent.width = 640;
    old->render_extent.height = 360;
    FrameLabD3D12Fg* fg = NULL;
    /* Accepted as the render_extent layout: create stops at the missing device instead of the
     * size check, without touching the color fields that this layout does not have. */
    int result = framelab_d3d12_fg_create((const FrameLabD3D12FgConfig*)old, &fg) == FRAMELAB_INVALID_ARGUMENT && !fg ? 0 : 2;
    /* One byte short of it is rejected. */
    if (!result) {
        old->struct_size -= 1;
        result = framelab_d3d12_fg_create((const FrameLabD3D12FgConfig*)old, &fg) == FRAMELAB_ABI_MISMATCH && !fg ? 0 : 3;
    }
    VirtualFree(pages, 0, MEM_RELEASE);
    return result;
}
