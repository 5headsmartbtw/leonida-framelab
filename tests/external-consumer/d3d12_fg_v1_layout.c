/* SPDX-License-Identifier: MIT */
/* A client compiled against the v0.4.0 D3D12 FG header, whose config ends at report_user. The
 * config is placed directly before an inaccessible page, so reading past struct_size faults. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <framelab/d3d12_fg.h>

#include <stddef.h>
#include <string.h>

/* FrameLabD3D12FgConfig exactly as published in v0.4.0. */
typedef struct V1Config {
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
} V1Config;

#define SAME(field) (offsetof(V1Config, field) == offsetof(FrameLabD3D12FgConfig, field))
_Static_assert(SAME(struct_size) && SAME(abi_version) && SAME(device_id) && SAME(queue_id) &&
                   SAME(device) && SAME(queue) && SAME(completion_fence) && SAME(extent) &&
                   SAME(project_id) && SAME(engine_version) && SAME(ngx_runtime_directory) &&
                   SAME(log_directory) && SAME(report) && SAME(report_user),
               "the v1 prefix of FrameLabD3D12FgConfig moved");
_Static_assert(offsetof(FrameLabD3D12FgConfig, render_extent) == offsetof(V1Config, report_user) + sizeof(void*) &&
                   sizeof(V1Config) < sizeof(FrameLabD3D12FgConfig),
               "render_extent must be appended after the v1 layout");

int main(void)
{
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    const SIZE_T page = system.dwPageSize;
    unsigned char* pages = (unsigned char*)VirtualAlloc(NULL, 2 * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD previous = 0;
    if (!pages || !VirtualProtect(pages + page, page, PAGE_NOACCESS, &previous)) return 1;
    V1Config* v1 = (V1Config*)(pages + page - sizeof(V1Config));
    memset(v1, 0, sizeof *v1);
    v1->struct_size = (uint32_t)sizeof *v1;
    v1->abi_version = FRAMELAB_ABI_VERSION;
    v1->extent.width = 1280;
    v1->extent.height = 720;
    FrameLabD3D12Fg* fg = NULL;
    /* Accepted as a v1 layout: create stops at the missing device instead of the size check. */
    int result = framelab_d3d12_fg_create((const FrameLabD3D12FgConfig*)v1, &fg) == FRAMELAB_INVALID_ARGUMENT && !fg ? 0 : 2;
    /* One byte short of the v1 layout is rejected. */
    if (!result) {
        v1->struct_size -= 1;
        result = framelab_d3d12_fg_create((const FrameLabD3D12FgConfig*)v1, &fg) == FRAMELAB_ABI_MISMATCH && !fg ? 0 : 3;
    }
    VirtualFree(pages, 0, MEM_RELEASE);
    return result;
}
