/* SPDX-License-Identifier: MIT */
/* GPU-free stand-ins for the shared FrameLab library, used only by the Rust runtime-loader tests.
 * Each build exports the core ABI symbols (or a chosen subset), reports FIXTURE_ABI_VERSION and
 * optionally exports the D3D12 SR and FG symbols. Every call other than the ABI query returns
 * FRAMELAB_UNSUPPORTED (14) and clears an output handle. No real FrameLab code is involved. */
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  define FIXTURE_API __declspec(dllexport)
#else
#  define FIXTURE_API __attribute__((visibility("default")))
#endif

#ifndef FIXTURE_ABI_VERSION
#  define FIXTURE_ABI_VERSION 1
#endif
#define UNSUPPORTED 14u

#define STUB(name) FIXTURE_API uint32_t name(void* a, void* b, void* c, void* d) \
    { (void)a; (void)b; (void)c; (void)d; return UNSUPPORTED; }
#define CREATE(name) FIXTURE_API uint32_t name(const void* config, void** out) \
    { (void)config; if (out) *out = NULL; return UNSUPPORTED; }

FIXTURE_API uint32_t framelab_abi_version(void) { return FIXTURE_ABI_VERSION; }
FIXTURE_API const char* framelab_status_string(uint32_t status) { (void)status; return "fixture"; }
STUB(framelab_lease_create)
FIXTURE_API void framelab_lease_destroy(void* lease) { (void)lease; }
FIXTURE_API uint32_t framelab_fg_stream_create(uint32_t abi, void** out) { (void)abi; if (out) *out = NULL; return UNSUPPORTED; }
STUB(framelab_fg_stream_destroy)
STUB(framelab_fg_prepare)
STUB(framelab_fg_mark_submitted)
STUB(framelab_fg_cancel)
STUB(framelab_fg_retire)
#ifndef FIXTURE_OMIT_CORE_EXPORT
STUB(framelab_fg_invalidate)
#endif

#ifdef FIXTURE_SR
CREATE(framelab_d3d12_sr_create)
STUB(framelab_d3d12_sr_destroy)
STUB(framelab_d3d12_sr_query_settings)
STUB(framelab_d3d12_sr_record_configure)
STUB(framelab_d3d12_sr_record)
STUB(framelab_d3d12_sr_mark_submitted)
STUB(framelab_d3d12_sr_cancel)
STUB(framelab_d3d12_sr_retire)
STUB(framelab_d3d12_sr_invalidate)
#  ifndef FIXTURE_SR_PARTIAL
FIXTURE_API uint32_t framelab_d3d12_sr_last_ngx_result(const void* sr) { (void)sr; return 0; }
#  endif
#endif

#ifdef FIXTURE_FG
CREATE(framelab_d3d12_fg_create)
STUB(framelab_d3d12_fg_destroy)
STUB(framelab_d3d12_fg_record_initialize)
STUB(framelab_d3d12_fg_record)
STUB(framelab_d3d12_fg_mark_submitted)
STUB(framelab_d3d12_fg_cancel)
STUB(framelab_d3d12_fg_retire)
STUB(framelab_d3d12_fg_invalidate)
FIXTURE_API uint32_t framelab_d3d12_fg_last_ngx_result(const void* fg) { (void)fg; return 0; }
#endif
