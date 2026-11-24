// SPDX-License-Identifier: MIT
#pragma once

#include <vulkan/vulkan.h>
#include <windows.h>
#include <psapi.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <vector>

class FrameMetrics {
public:
  using Clock=std::chrono::steady_clock;
  struct Counts { unsigned sr{}, fg{}, real{}, generated{}, verified{}, errors{}, fallbacks{}; };

  void init(const std::filesystem::path& path,VkPhysicalDevice physical,VkDevice device,
            uint32_t family,bool budgetEnabled,HWND window)
  {
    if(path.empty())return;
    file=_wfopen(path.c_str(),L"wb");
    if(!file)throw std::runtime_error("cannot open frame metrics");
    pd=physical;dev=device;budget=budgetEnabled;
    std::fprintf(file,"frame,frame_ms,gpu_ms,sr_delta,fg_delta,real_delta,generated_delta,verified_delta,error_delta,fallback_delta,rss_bytes,vram_bytes,render_width,render_height,output_width,output_height,refresh_hz\n");
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(pd,&properties);
    period=properties.limits.timestampPeriod;
    uint32_t count=0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd,&count,nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd,&count,families.data());
    if(family<count)timestampBits=families[family].timestampValidBits;
    if(timestampBits) {
      VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=4;
      if(vkCreateQueryPool(dev,&info,nullptr,&pool)!=VK_SUCCESS)
        throw std::runtime_error("cannot allocate Vulkan timestamp queries");
    }
    MONITORINFOEXW monitor{};
    monitor.cbSize=sizeof monitor;
    DEVMODEW mode{};
    mode.dmSize=sizeof mode;
    if(GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),reinterpret_cast<MONITORINFO*>(&monitor)) &&
       EnumDisplaySettingsW(monitor.szDevice,ENUM_CURRENT_SETTINGS,&mode))
      refresh=mode.dmDisplayFrequency;
    std::printf("FRAME_METRICS timestamp_bits=%u refresh_hz=%u memory_budget=%d\n",timestampBits,refresh,int(budget));
  }

  bool active() const {return file!=nullptr;}

  void start(VkCommandBuffer cmd)
  {
    hasExtra=false;
    if(!pool)return;
    vkCmdResetQueryPool(cmd,pool,0,4);
    vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,pool,0);
  }

  void end(VkCommandBuffer cmd)
  {
    if(pool)vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,pool,1);
  }

  void startExtra(VkCommandBuffer cmd)
  {
    hasExtra=true;
    if(pool)vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,pool,2);
  }

  void endExtra(VkCommandBuffer cmd)
  {
    if(pool)vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,pool,3);
  }

  void write(unsigned frame,Clock::time_point end,Counts c,VkExtent2D render,VkExtent2D output)
  {
    if(!file)return;
    if(previous==Clock::time_point{})previous=end;
    const double frameMs=std::chrono::duration<double,std::milli>(end-previous).count();
    previous=end;
    uint64_t values[4]{};
    double gpuMs=-1;
    if(pool) {
      const auto result=vkGetQueryPoolResults(dev,pool,0,hasExtra?4:2,sizeof values,values,sizeof(uint64_t),
                                              VK_QUERY_RESULT_64_BIT);
      if(result==VK_SUCCESS) {
        const uint64_t mask=timestampBits>=64?UINT64_MAX:((uint64_t(1)<<timestampBits)-1);
        const auto ticks=((values[1]-values[0])&mask)+(hasExtra?((values[3]-values[2])&mask):0);
        gpuMs=double(ticks)*period/1e6;
      }
    }
    if(frame==1||frame%30==0)sampleMemory();
    std::fprintf(file,"%u,%.6f,%.6f,%u,%u,%u,%u,%u,%u,%u,%llu,%llu,%u,%u,%u,%u,%lu\n",
        frame,frameMs,gpuMs,c.sr-last.sr,c.fg-last.fg,c.real-last.real,c.generated-last.generated,
        c.verified-last.verified,c.errors-last.errors,c.fallbacks-last.fallbacks,
        static_cast<unsigned long long>(rss),static_cast<unsigned long long>(vram),
        render.width,render.height,output.width,output.height,refresh);
    last=c;
    if(frame%60==0)std::fflush(file);
  }

  void destroy()
  {
    if(pool)vkDestroyQueryPool(dev,pool,nullptr);
    if(file)std::fclose(file);
    pool=VK_NULL_HANDLE;file=nullptr;
  }

private:
  FILE* file{};
  VkPhysicalDevice pd{};
  VkDevice dev{};
  VkQueryPool pool{};
  uint32_t timestampBits{};
  DWORD refresh{};
  double period{};
  bool budget{},hasExtra{};
  uint64_t rss{},vram{};
  Clock::time_point previous{};
  Counts last{};

  void sampleMemory()
  {
    PROCESS_MEMORY_COUNTERS process{};
    if(K32GetProcessMemoryInfo(GetCurrentProcess(),&process,sizeof process))rss=process.WorkingSetSize;
    if(!budget)return;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT data{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,&data};
    vkGetPhysicalDeviceMemoryProperties2(pd,&properties);
    vram=0;
    for(uint32_t i=0;i<properties.memoryProperties.memoryHeapCount;++i)
      if(properties.memoryProperties.memoryHeaps[i].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)vram+=data.heapUsage[i];
  }
};
