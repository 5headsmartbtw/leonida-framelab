// Windows experimental port, 2026-11-23. Original MIT license: ../../LICENSES/MIT-dlssg-open.txt
// dlssg-open native: Vulkan layer VK_LAYER_DLSSG_OPEN_cubin.
// The native-Linux counterpart of patches/dxvk-nvapi-ampere-dlssg.patch. On native Vulkan NGX hands its CUDA kernels
// to the driver itself through VK_NVX_binary_import, so the rewrite sits on vkCreateCuModuleNVX:
//   - fatbins: the LZ4 PTX entry's ".target sm_NN" literal and arch field are rewritten to DLSSG_OPEN_RETARGET_SM;
//   - bare ELF cubins above that SM are replaced by the sm_<target> twin the loaded runtime already carries.
// Both happen in memory. Nothing of NVIDIA's is modified on disk.
//
// DLSSG_OPEN_RETARGET_SM=86   enable the rewrite for that SM (unset: the layer passes everything through)
// DLSSG_OPEN_DUMP_DIR=<dir>   write every uploaded cubin/fatbin there
// DLSSG_OPEN_LOG=1 / DLSSG_OPEN_LOG_PATH=<file>   log every retarget and substitution
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <cctype>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

void logf(const char* fmt, ...) {
    static FILE* f = [] {
        const char* p = getenv("DLSSG_OPEN_LOG_PATH");
        if (p && *p) return fopen(p, "a");
        return getenv("DLSSG_OPEN_LOG") ? stderr : (FILE*)nullptr;
    }();
    if (!f) return;
    static std::mutex mu;
    std::scoped_lock lock(mu);
    va_list ap;
    va_start(ap, fmt);
    fputs("dlssg-open layer: ", f), vfprintf(f, fmt, ap), fputc('\n', f), fflush(f);
    va_end(ap);
}

// ---- CUDA fatbin PTX retargeting ----------------------------------------------------------------
int retargetSm() {
    static int sm = [] {
        const char* v = getenv("DLSSG_OPEN_RETARGET_SM");
        return v && *v ? atoi(v) : 0;
    }();
    return sm;
}

void dumpCubin(const void* data, size_t size) {
    static const std::string dir = getenv("DLSSG_OPEN_DUMP_DIR") ? getenv("DLSSG_OPEN_DUMP_DIR") : "";
    if (dir.empty() || !data || !size) return;
    static std::atomic<int> seq{0};
    int n = seq++;
    std::string fname = dir + "/" + std::to_string(n) + ".bin";
    FILE* f = fopen(fname.c_str(), "wb");
    if (!f) return;
    fwrite(data, 1, size, f);
    fclose(f);
}

std::optional<std::vector<uint8_t>> RetargetFatbin(const void* data, size_t size) {
    auto sm = retargetSm();
    if (sm < 10 || sm > 99 || !data || size < 16) return std::nullopt;

    auto src = static_cast<const uint8_t*>(data);
    uint32_t magic; memcpy(&magic, src, 4);
    uint16_t ver; memcpy(&ver, src + 4, 2);
    uint16_t hdr; memcpy(&hdr, src + 6, 2);
    if (magic != 0xBA55ED50u || ver != 1 || hdr != 16) {
        logf("Cubin upload: not a fatbin (magic %#x, size %zu)", magic, size);
        return std::nullopt;
    }
    {
        std::string desc;
        bool runsAsIs = false;
        size_t q = hdr;
        while (q + 64 <= size) {
            uint16_t k; memcpy(&k, src + q, 2);
            uint32_t hs; memcpy(&hs, src + q + 4, 4);
            uint64_t pd; memcpy(&pd, src + q + 8, 8);
            uint32_t ar; memcpy(&ar, src + q + 28, 4);
            if ((k != 1 && k != 2) || hs < 64 || q + hs + pd > size) break;
            desc += (k == 1 ? " ptx" : " elf") + std::string("_sm") + std::to_string(ar);
            runsAsIs |= ar <= static_cast<uint32_t>(sm);
            q += hs + pd;
        }
        logf("Cubin upload: fatbin size %zu entries:%s%s", size, desc.c_str(), runsAsIs ? " (has an entry for this GPU: left as it is)" : "");
        // (not in the dxvk-nvapi patch: a fatbin that already carries an entry the device runs, as DLSS Super
        // Resolution's do, goes to the driver untouched, so the driver's choice among its entries stays what it is
        // without this layer)
        if (runsAsIs) return std::nullopt;
    }

    std::vector<uint8_t> buf(src, src + size);
    char want[16]; snprintf(want, sizeof(want), ".target sm_%d", sm);
    const std::string wantStr(want);
    bool changed = false;
    size_t p = hdr;
    while (p + 64 <= buf.size()) {
        uint16_t kind; memcpy(&kind, &buf[p], 2);
        uint32_t hsz; memcpy(&hsz, &buf[p + 4], 4);
        uint64_t padded; memcpy(&padded, &buf[p + 8], 8);
        uint32_t real; memcpy(&real, &buf[p + 16], 4);
        uint32_t arch; memcpy(&arch, &buf[p + 28], 4);
        if ((kind != 1 && kind != 2) || hsz < 64 || p + hsz + padded > buf.size()) break;
        if (kind == 1 && arch > static_cast<uint32_t>(sm) && arch < 100) {
            auto begin = buf.begin() + p + hsz;
            auto end = begin + std::min<uint64_t>(real, padded);
            const std::string needle = ".target sm_" + std::to_string(arch);
            auto it = std::search(begin, end, needle.begin(), needle.end());
            if (it != end) {
                std::copy(wantStr.begin(), wantStr.end(), it);
                uint32_t a = sm; memcpy(&buf[p + 28], &a, 4);
                changed = true;
                logf("Retargeted fatbin PTX entry sm_%u -> sm_%d", arch, sm);
            } else {
                logf("Fatbin PTX entry sm_%u: '.target' literal not found, left untouched", arch);
            }
        }
        p += hsz + padded;
    }
    if (!changed) return std::nullopt;
    return buf;
}

// ---- raw ELF cubin substitution -----------------------------------------------------------------
namespace elf {
struct Cubin {
    std::vector<uint8_t> data;
    uint32_t sm = 0;
    std::string kernel;
    size_t textOff = 0, textSize = 0;
    std::vector<uint8_t> c2;
    size_t c0Size = 0;
    int sharedShdr = -1;   // index of .nv.shared.<k> section header
    int rwPhdr = -1;       // index of RW PT_LOAD
};

bool parse(const uint8_t* d, size_t n, Cubin& out) {
    if (n < 64 || memcmp(d, "\x7f" "ELF", 4) != 0 || d[4] != 2) return false;
    uint16_t machine; memcpy(&machine, d + 18, 2);
    if (machine != 190) return false;
    uint32_t flags; memcpy(&flags, d + 48, 4);
    uint64_t phoff, shoff; memcpy(&phoff, d + 32, 8); memcpy(&shoff, d + 40, 8);
    uint16_t phentsize, phnum, shentsize, shnum, shstrndx;
    memcpy(&phentsize, d + 54, 2); memcpy(&phnum, d + 56, 2);
    memcpy(&shentsize, d + 58, 2); memcpy(&shnum, d + 60, 2); memcpy(&shstrndx, d + 62, 2);
    size_t end = std::max<size_t>(shoff + size_t(shentsize) * shnum, phoff + size_t(phentsize) * phnum);
    if (end > n || shstrndx >= shnum || shentsize < 64) return false;
    auto sh = [&](int i, int field) { uint64_t v; memcpy(&v, d + shoff + size_t(i) * shentsize + field, 8); return v; };
    auto sh32 = [&](int i, int field) { uint32_t v; memcpy(&v, d + shoff + size_t(i) * shentsize + field, 4); return v; };
    uint64_t strOff = sh(shstrndx, 24), strSize = sh(shstrndx, 32);
    if (strOff + strSize > n) return false;
    out.data.assign(d, d + end);
    out.sm = flags & 0xff;
    for (int i = 0; i < shnum; i++) {
        uint32_t nameIdx = sh32(i, 0);
        if (nameIdx >= strSize) continue;
        std::string name(reinterpret_cast<const char*>(d + strOff + nameIdx));
        uint64_t off = sh(i, 24), size = sh(i, 32);
        if (name.rfind(".text.", 0) == 0 && out.kernel.empty()) { out.kernel = name.substr(6); out.textOff = off; out.textSize = size; }
        else if (name.rfind(".nv.constant2.", 0) == 0 && off + size <= n) out.c2.assign(d + off, d + off + size);
        else if (name.rfind(".nv.constant0.", 0) == 0) out.c0Size = size;
        else if (name.rfind(".nv.shared.", 0) == 0) out.sharedShdr = i;
    }
    for (int i = 0; i < phnum; i++) {
        uint32_t type, pflags; memcpy(&type, d + phoff + size_t(i) * phentsize, 4); memcpy(&pflags, d + phoff + size_t(i) * phentsize + 4, 4);
        if (type == 1 && (pflags & 2) && !(pflags & 1)) out.rwPhdr = i;   // PT_LOAD, W, not X
    }
    return !out.kernel.empty() && out.textOff + out.textSize <= n;
}

uint64_t sharedSize(const Cubin& c) {
    if (c.sharedShdr < 0) return 0;
    uint64_t shoff; memcpy(&shoff, c.data.data() + 40, 8);
    uint16_t shentsize; memcpy(&shentsize, c.data.data() + 58, 2);
    uint64_t v; memcpy(&v, c.data.data() + shoff + size_t(c.sharedShdr) * shentsize + 32, 8); return v;
}

void setSharedSize(Cubin& c, uint64_t v) {
    if (c.sharedShdr < 0) return;
    uint64_t shoff; memcpy(&shoff, c.data.data() + 40, 8);
    uint16_t shentsize; memcpy(&shentsize, c.data.data() + 58, 2);
    memcpy(c.data.data() + shoff + size_t(c.sharedShdr) * shentsize + 32, &v, 8);
    if (c.rwPhdr >= 0) {
        uint64_t phoff; memcpy(&phoff, c.data.data() + 32, 8);
        uint16_t phentsize; memcpy(&phentsize, c.data.data() + 54, 2);
        memcpy(c.data.data() + phoff + size_t(c.rwPhdr) * phentsize + 40, &v, 8);   // p_memsz
    }
}

double score(const Cubin& a, const Cubin& b) {
    if (a.textSize != b.textSize || a.textSize == 0) return 0.0;
    size_t chunks = a.textSize / 16, eq = 0;
    for (size_t i = 0; i < chunks; i++)
        if (memcmp(a.data.data() + a.textOff + i * 16, b.data.data() + b.textOff + i * 16, 16) == 0) eq++;
    return chunks ? double(eq) / double(chunks) : 0.0;
}
}   // namespace elf

std::mutex g_twinMutex;
std::vector<elf::Cubin> g_twins;
std::vector<std::string> g_twinSources;   // (the runtimes indexed already)

// The DLSS runtimes mapped into this process (libnvidia-ngx-dlssg.so.*, libnvidia-ngx-dlss.so.*), as file images
// rebuilt from their mappings. NGX loads them from a memfd copy ("/memfd:<path> (deleted)" in /proc/self/maps), so
// there is no file to open: each readable mapping is copied to its file offset instead.
struct RuntimeImage {
    std::string name;
    std::vector<uint8_t> data;
};

// Windows counterpart of /proc/self/maps: only the process's mapped FG runtime.
// ReadProcessMemory avoids dereferencing a module that disappears during enumeration.
std::vector<RuntimeImage> runtimeImages() {
    std::vector<RuntimeImage> out;
    HMODULE modules[1024]; DWORD needed=0;
    HANDLE process=GetCurrentProcess();
    if(!EnumProcessModules(process,modules,sizeof(modules),&needed))return out;
    const size_t count=std::min<size_t>(needed/sizeof(HMODULE),1024);
    for(size_t i=0;i<count;++i){
        char path[32768]={};if(!GetModuleFileNameA(modules[i],path,sizeof(path)))continue;
        std::string name(path), lower(name);for(char&c:lower)c=char(std::tolower(static_cast<unsigned char>(c)));
        const size_t slash=lower.find_last_of("\\/");
        if(lower.substr(slash==std::string::npos?0:slash+1)!="nvngx_dlssg.dll")continue;
        MODULEINFO info{};if(!GetModuleInformation(process,modules[i],&info,sizeof(info)))continue;
        if(info.SizeOfImage>256u*1024u*1024u)continue;
        RuntimeImage image{name,std::vector<uint8_t>(info.SizeOfImage)};
        uintptr_t begin=reinterpret_cast<uintptr_t>(info.lpBaseOfDll), pos=begin, finish=begin+info.SizeOfImage;
        while(pos<finish){
            MEMORY_BASIC_INFORMATION mbi{};if(!VirtualQuery(reinterpret_cast<void*>(pos),&mbi,sizeof(mbi)))break;
            const uintptr_t regionEnd=std::min(finish,reinterpret_cast<uintptr_t>(mbi.BaseAddress)+mbi.RegionSize);
            if(regionEnd<=pos)break;
            if(mbi.State==MEM_COMMIT && !(mbi.Protect&(PAGE_NOACCESS|PAGE_GUARD))){
                SIZE_T read=0;ReadProcessMemory(process,reinterpret_cast<void*>(pos),image.data.data()+pos-begin,regionEnd-pos,&read);
            }
            pos=regionEnd;
        }
        out.push_back(std::move(image));
    }
    return out;
}

// The sm_<target> cubins of every runtime mapped now and not indexed yet. Called again when an upload finds no twin:
// a runtime NGX loaded after the first look is indexed then.
size_t loadTwins(int targetSm) {
    std::scoped_lock lock(g_twinMutex);
    size_t added = 0;
    for (const auto& img : runtimeImages()) {
        if (std::find(g_twinSources.begin(), g_twinSources.end(), img.name) != g_twinSources.end()) continue;
        g_twinSources.push_back(img.name);
        const auto& file = img.data;
        size_t found = 0;
        for (size_t i = 0; i + 64 <= file.size(); i++) {
            if (file[i] != 0x7f || memcmp(&file[i], "\x7f" "ELF", 4) != 0) continue;
            elf::Cubin c;
            if (elf::parse(file.data() + i, file.size() - i, c) && int(c.sm) == targetSm) { g_twins.push_back(std::move(c)); found++; }
        }
        logf("Cubin twins: indexed %zu sm_%d cubins from %s", found, targetSm, img.name.c_str());
        added += found;
    }
    return added;
}

std::optional<std::vector<uint8_t>> SubstituteCubin(const void* data, size_t size) {
    auto sm = retargetSm();
    if (sm < 10 || sm > 99 || !data || size < 64) return std::nullopt;
    elf::Cubin up;
    if (!elf::parse(static_cast<const uint8_t*>(data), size, up)) return std::nullopt;
    if (int(up.sm) <= sm) return std::nullopt;
    const elf::Cubin* best = nullptr; double bestScore = -1; int strictCount = 0;
    const auto find = [&] {
        std::scoped_lock lock(g_twinMutex);
        for (auto& t : g_twins) {
            if (t.kernel != up.kernel) continue;
            bool strict = t.textSize == up.textSize && t.c2 == up.c2 && t.c0Size == up.c0Size;
            if (strict) strictCount++;
            double s = elf::score(up, t) + (strict ? 1.0 : 0.0);
            if (s > bestScore) { bestScore = s; best = &t; }
        }
    };
    find();
    if (!best && loadTwins(sm)) find();   // (not indexed yet: the runtimes mapped by now, then once more)
    std::scoped_lock lock(g_twinMutex);
    if (!best) {
        logf("Cubin substitute: no sm_%d twin for '%s' (sm_%u, size %zu)", sm, up.kernel.c_str(), up.sm, size);
        return std::nullopt;
    }
    elf::Cubin twin = *best;
    uint64_t shared = elf::sharedSize(up);
    if (shared) elf::setSharedSize(twin, shared);
    logf("Cubin substitute: '%s' sm_%u size %zu -> sm_%u twin size %zu (score %g, strict candidates %d, shared %llu)", up.kernel.c_str(),
         up.sm, size, twin.sm, twin.data.size(), bestScore, strictCount, (unsigned long long)shared);
    return twin.data;
}

std::optional<std::vector<uint8_t>> RewriteCubin(const void* data, size_t size) {
    if (auto r = RetargetFatbin(data, size)) return r;
    return SubstituteCubin(data, size);
}

// ---- the layer ----------------------------------------------------------------------------------
struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkDestroyInstance destroy = nullptr;
};
struct DeviceData {
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkDestroyDevice destroy = nullptr;
    PFN_vkCreateCuModuleNVX createCuModule = nullptr;
    PFN_vkDestroyCuModuleNVX destroyCuModule = nullptr;
};

std::mutex g_mutex;
std::unordered_map<void*, InstanceData> g_instances;
std::unordered_map<void*, DeviceData> g_devices;
struct Retargeted {
    void* device;   // (key(VkDevice): the buffers of a device are dropped with it)
    std::vector<uint8_t> data;
};
std::mutex g_retargetMutex;
std::unordered_map<uint64_t, Retargeted> g_retargetedCubins;

template <class T> void* key(T handle) { return *reinterpret_cast<void**>(handle); }

void KeepRetargeted(void* device, VkCuModuleNVX handle, std::vector<uint8_t>&& buf) {
    std::scoped_lock lock(g_retargetMutex);
    g_retargetedCubins.insert_or_assign((uint64_t)handle, Retargeted{device, std::move(buf)});
}

void DropRetargetedOf(void* device) {
    std::scoped_lock lock(g_retargetMutex);
    for (auto it = g_retargetedCubins.begin(); it != g_retargetedCubins.end();)
        it = it->second.device == device ? g_retargetedCubins.erase(it) : std::next(it);
}

void DropRetargeted(VkCuModuleNVX handle) {
    std::scoped_lock lock(g_retargetMutex);
    g_retargetedCubins.erase((uint64_t)handle);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {
    auto* ci = (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;
    while (ci && !(ci->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && ci->function == VK_LAYER_LINK_INFO)) ci = (VkLayerInstanceCreateInfo*)ci->pNext;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = ci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    ci->u.pLayerInfo = ci->u.pLayerInfo->pNext;
    auto create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create(pCreateInfo, pAllocator, pInstance);
    if (r != VK_SUCCESS) return r;
    InstanceData d;
    d.instance = *pInstance, d.gipa = gipa;
    d.destroy = (PFN_vkDestroyInstance)gipa(*pInstance, "vkDestroyInstance");
    std::scoped_lock lock(g_mutex);
    g_instances[key(*pInstance)] = d;
    logf("instance created (retarget sm %d)", retargetSm());
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    if (!instance) return;
    InstanceData d;
    {
        std::scoped_lock lock(g_mutex);
        d = g_instances[key(instance)];
        g_instances.erase(key(instance));
    }
    if (d.destroy) d.destroy(instance, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                                            VkDevice* pDevice) {
    auto* ci = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
    while (ci && !(ci->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && ci->function == VK_LAYER_LINK_INFO)) ci = (VkLayerDeviceCreateInfo*)ci->pNext;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = ci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = ci->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    ci->u.pLayerInfo = ci->u.pLayerInfo->pNext;
    VkInstance instance = VK_NULL_HANDLE;
    {
        std::scoped_lock lock(g_mutex);
        auto it = g_instances.find(key(physicalDevice));
        if (it != g_instances.end()) instance = it->second.instance;
    }
    auto create = (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (r != VK_SUCCESS) return r;
    DeviceData d;
    d.gdpa = gdpa;
    d.destroy = (PFN_vkDestroyDevice)gdpa(*pDevice, "vkDestroyDevice");
    d.createCuModule = (PFN_vkCreateCuModuleNVX)gdpa(*pDevice, "vkCreateCuModuleNVX");
    d.destroyCuModule = (PFN_vkDestroyCuModuleNVX)gdpa(*pDevice, "vkDestroyCuModuleNVX");
    std::scoped_lock lock(g_mutex);
    g_devices[key(*pDevice)] = d;
    logf("device created (VK_NVX_binary_import %s)", d.createCuModule ? "present" : "not enabled");
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    if (!device) return;
    DeviceData d;
    void* const k = key(device);
    {
        std::scoped_lock lock(g_mutex);
        d = g_devices[k];
        g_devices.erase(k);
    }
    if (d.destroy) d.destroy(device, pAllocator);
    DropRetargetedOf(k);
}

DeviceData deviceData(VkDevice device) {
    std::scoped_lock lock(g_mutex);
    auto it = g_devices.find(key(device));
    return it == g_devices.end() ? DeviceData{} : it->second;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateCuModuleNVX(VkDevice device, const VkCuModuleCreateInfoNVX* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                                                 VkCuModuleNVX* pModule) {
    DeviceData d = deviceData(device);
    if (!d.createCuModule) return VK_ERROR_EXTENSION_NOT_PRESENT;
    dumpCubin(pCreateInfo->pData, pCreateInfo->dataSize);
    auto retargeted = RewriteCubin(pCreateInfo->pData, pCreateInfo->dataSize);
    VkCuModuleCreateInfoNVX local = *pCreateInfo;
    if (retargeted) {
        local.pData = retargeted->data();
        local.dataSize = retargeted->size();
    }
    VkResult r = d.createCuModule(device, &local, pAllocator, pModule);
    logf("vkCreateCuModuleNVX size=%zu rewritten=%d result=%d",local.dataSize,retargeted.has_value(),r);
    if (r != VK_SUCCESS) {
        logf("vkCreateCuModuleNVX failed %d size=%zu%s", r, local.dataSize, retargeted ? " (rewritten)" : "");
        return r;
    }
    if (retargeted) KeepRetargeted(key(device), *pModule, std::move(*retargeted));
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyCuModuleNVX(VkDevice device, VkCuModuleNVX module, const VkAllocationCallbacks* pAllocator) {
    DeviceData d = deviceData(device);
    if (d.destroyCuModule) d.destroyCuModule(device, module, pAllocator);
    DropRetargeted(module);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* pName);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* pName);

PFN_vkVoidFunction deviceIntercept(const char* name) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)GetDeviceProcAddr;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)DestroyDevice;
    if (!strcmp(name, "vkCreateCuModuleNVX")) return (PFN_vkVoidFunction)CreateCuModuleNVX;
    if (!strcmp(name, "vkDestroyCuModuleNVX")) return (PFN_vkVoidFunction)DestroyCuModuleNVX;
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* pName) {
    DeviceData d = deviceData(device);
    if (auto f = deviceIntercept(pName)) {
        // (the NVX entry points only where the device has the extension: a null here is what the driver would answer)
        if (strstr(pName, "CuModuleNVX") && !d.createCuModule) return nullptr;
        return f;
    }
    return d.gdpa ? d.gdpa(device, pName) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!strcmp(pName, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)GetInstanceProcAddr;
    if (!strcmp(pName, "vkCreateInstance")) return (PFN_vkVoidFunction)CreateInstance;
    if (!strcmp(pName, "vkDestroyInstance")) return (PFN_vkVoidFunction)DestroyInstance;
    if (!strcmp(pName, "vkCreateDevice")) return (PFN_vkVoidFunction)CreateDevice;
    if (auto f = deviceIntercept(pName)) return f;
    if (!instance) return nullptr;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    {
        std::scoped_lock lock(g_mutex);
        auto it = g_instances.find(key(instance));
        if (it != g_instances.end()) gipa = it->second.gipa;
    }
    return gipa ? gipa(instance, pName) : nullptr;
}

}   // namespace

extern "C" __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL dlssg_open_negotiate(VkNegotiateLayerInterface* pVersionStruct) {
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (pVersionStruct->loaderLayerInterfaceVersion > 2) pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetInstanceProcAddr = GetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = GetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
