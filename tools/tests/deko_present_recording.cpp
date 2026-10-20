// SPDX-License-Identifier: MIT
// Exercise the production renderer with a recording deko API. GPU shaders and
// LSFG computation are tested separately; this checks image ownership/pacing,
// late framebuffer capture, UI placement and original/generated ordering.
#define __SO_UTIL_H__
typedef struct so_module so_module;
#include "../../source/drastic_deko_renderer.cpp"
#include <stdexcept>
#include <iostream>
#include <cstdarg>

namespace dk::detail {
struct Device {};
struct MemBlock { unsigned size, flags; void *storage; std::vector<uint8_t> cpu; };
struct CmdBuf {
  bool pending{}, blend{}; unsigned ui{}, copy_kind{}; const DkImage *target{};
  const void *parameters{}; std::vector<DkFence *> signals;
};
struct Queue { unsigned flags; CmdBuf *submitted{}; };
struct Swapchain { unsigned count, cursor{}; int acquired=-1; std::vector<const DkImage *> images; };
}
namespace {
bool pref_low{}, pref_fg{}, initializing{};
unsigned acquired_serial{}, captured_serial{}, presented{}, allocated_blocks{}, scanline_draws{};
unsigned next_kind{}, next_id=1;
int expected_rotation{};
alignas(4096) uint8_t core_pool[DRASTIC_NATIVE_POOL_BYTES];
std::vector<DkCmdBuf> commands;
struct Meta { unsigned width, height, format, flags, id, kind; };
struct Fence { DkCmdBuf cmd; bool submitted; };
void check(bool value,const char *why) { if(!value) throw std::runtime_error(why); }
template<class T,class U> T get(const U *obj) { T value; static_assert(sizeof(T)<=sizeof(U)); std::memcpy(&value,obj,sizeof(value)); return value; }
template<class T,class U> void put(U *obj,T value) { static_assert(sizeof(T)<=sizeof(U)); std::memset(obj,0,sizeof(U)); std::memcpy(obj,&value,sizeof(value)); }
}

extern "C" {
int panel_width=1280,panel_height=720,screen_width=1280,screen_height=720;
bool prefs_get_bool(const char *key,bool fallback) {
  if(!std::strcmp(key,"Wrapper/LowLatency")) return pref_low;
  if(!std::strcmp(key,"Wrapper/LSFGEnabled")) return pref_fg;
  return fallback;
}
float prefs_get_float(const char *,float fallback) { return fallback; }
void fatal_error_set_graphics_active(int) {}
void fatal_error(const char *fmt,...) {
  char message[1024]; va_list args; va_start(args,fmt); std::vsnprintf(message,sizeof(message),fmt,args); va_end(args);
  throw std::runtime_error(message);
}
bool drastic_native_frame_lock(DrasticNativeFrame *frame) {
  if(!initializing) check(acquired_serial>captured_serial,"captured core before image acquisition");
  captured_serial=acquired_serial;
  *frame={}; frame->allocation=core_pool; frame->bank=acquired_serial%2;
  frame->dirty=3; frame->bytes_per_pixel=4;
  for(unsigned i=0;i<2;++i) { frame->width[i]=256; frame->height[i]=192; frame->pixels[i]=core_pool+frame->bank*DRASTIC_NATIVE_BANK_BYTES+i*DRASTIC_NATIVE_SCREEN_BYTES; }
  return true;
}
void drastic_native_frame_unlock(void *lease,DrasticNativeFrameWait,unsigned uploaded) {
  if(uploaded) check(lease&&get<Fence>(static_cast<DkFence *>(lease)).submitted,"published unsubmitted DMA fence");
}
void drastic_native_frame_detach() {}
bool drastic_config_map_stylus(const DrasticRuntimeConfig *,int,int,float *x,float *y) { *x=100; *y=100; return true; }
void drastic_custom_shader_destroy(DrasticCustomShader *) {}
bool drastic_custom_shader_load(const char *,unsigned,DrasticCustomShader *,char *,size_t) { return false; }
DkDevice dkDeviceCreate(const DkDeviceMaker *) { return new dk::detail::Device; }
void dkDeviceDestroy(DkDevice d) { delete d; }
DkQueue dkQueueCreate(const DkQueueMaker *m) { return new dk::detail::Queue{m->flags}; }
void dkQueueDestroy(DkQueue q) { delete q; }
bool dkQueueIsInErrorState(DkQueue) { return false; }
void dkQueueWaitIdle(DkQueue) { for(auto *c:commands) c->pending=false; }
void dkQueueFlush(DkQueue) {}
DkMemBlock dkMemBlockCreate(const DkMemBlockMaker *m) {
  auto *b=new dk::detail::MemBlock{m->size,m->flags,m->storage,{}};
  if(!m->storage&&!(m->flags&DkMemBlockFlags_Image)) b->cpu.resize(m->size);
  ++allocated_blocks; return b;
}
void dkMemBlockDestroy(DkMemBlock b) { --allocated_blocks; delete b; }
void *dkMemBlockGetCpuAddr(DkMemBlock b) { return b->storage?b->storage:b->cpu.data(); }
DkGpuAddr dkMemBlockGetGpuAddr(DkMemBlock b) { return reinterpret_cast<uintptr_t>(dkMemBlockGetCpuAddr(b)); }
uint32_t dkMemBlockGetSize(DkMemBlock b) { return b->size; }
DkResult dkMemBlockFlushCpuCache(DkMemBlock b,uint32_t off,uint32_t size) { check(uint64_t(off)+size<=b->size,"invalid DMA flush"); return DkResult_Success; }
void dkImageLayoutInitialize(DkImageLayout *l,const DkImageLayoutMaker *m) { put(l,Meta{m->dimensions[0],m->dimensions[1],unsigned(m->format),m->flags,0,0}); }
uint64_t dkImageLayoutGetSize(const DkImageLayout *l) { const auto m=get<Meta>(l); return uint64_t(m.width)*m.height*4; }
uint32_t dkImageLayoutGetAlignment(const DkImageLayout *) { return 4096; }
void dkImageInitialize(DkImage *i,const DkImageLayout *l,DkMemBlock b,uint32_t off) { check(dkImageLayoutGetSize(l)+off<=b->size,"image out of bounds"); auto m=get<Meta>(l); m.id=next_id++; put(i,m); }
void dkImageDescriptorInitialize(DkImageDescriptor *d,const DkImageView *v,bool,bool) { put(d,get<Meta>(v->pImage).id); }
void dkSamplerDescriptorInitialize(DkSamplerDescriptor *,const DkSampler *) {}
void dkShaderInitialize(DkShader *s,const DkShaderMaker *m) { const auto *p=static_cast<const uint32_t *>(m->control); put(s,p[6]==1?4u:0u); }
bool dkShaderIsValid(const DkShader *) { return true; }
DkStage dkShaderGetStage(const DkShader *s) { return DkStage(get<unsigned>(s)); }
DkCmdBuf dkCmdBufCreate(const DkCmdBufMaker *) { auto *c=new dk::detail::CmdBuf; c->signals.reserve(4); commands.push_back(c); return c; }
void dkCmdBufDestroy(DkCmdBuf c) { check(!c->pending,"destroying pending commands"); commands.erase(std::find(commands.begin(),commands.end(),c)); delete c; }
void dkCmdBufClear(DkCmdBuf c) { check(!c->pending,"overwriting an in-flight slot"); c->signals.clear(); c->ui=c->copy_kind=0; c->target=nullptr; }
void dkCmdBufAddMemory(DkCmdBuf,DkMemBlock,uint32_t,uint32_t) {}
DkCmdList dkCmdBufFinishList(DkCmdBuf c) { return reinterpret_cast<uintptr_t>(c); }
void dkCmdBufSignalFence(DkCmdBuf c,DkFence *f,bool) { put(f,Fence{c,false}); c->signals.push_back(f); }
DkResult dkFenceWait(DkFence *f,int64_t) { const auto v=get<Fence>(f); check(v.cmd&&v.submitted,"waiting on unsubmitted fence"); v.cmd->pending=false; return DkResult_Success; }
void dkQueueSubmitCommands(DkQueue q,DkCmdList list) {
  auto *c=reinterpret_cast<DkCmdBuf>(list); c->pending=true;
  for(auto *f:c->signals) put(f,Fence{c,true}); q->submitted=c;
}
DkSwapchain dkSwapchainCreate(const DkSwapchainMaker *m) {
  auto *s=new dk::detail::Swapchain; s->count=m->numImages;
  s->images.assign(m->pImages,m->pImages+m->numImages);
  check(s->count==(pref_low?2u:3u),"wrong presentation queue depth"); return s;
}
void dkSwapchainDestroy(DkSwapchain s) { check(s->acquired<0,"destroyed acquired image"); delete s; }
void dkSwapchainSetSwapInterval(DkSwapchain,unsigned) {}
int dkQueueAcquireImage(DkQueue,DkSwapchain s) {
  check(s->acquired<0,"two simultaneously acquired VI images"); ++acquired_serial;
  return s->acquired=int(s->cursor++%s->count);
}
void dkQueuePresentImage(DkQueue q,DkSwapchain s,int index) {
  check(index==s->acquired,"presented unowned image");
  check(q->submitted&&q->submitted->target==s->images[index],"wrong presentation target");
  check(q->submitted->copy_kind==next_kind,"wrong original/generated order");
  check(q->submitted->ui==5,"UI missing from presented frame");
  next_kind=next_kind==2?1:0; ++presented; s->acquired=-1;
}
void dkCmdBufBindRenderTargets(DkCmdBuf c,const DkImageView *const v[],uint32_t n,const DkImageView *) { check(n==1,"target count"); c->target=v[0]->pImage; }
void dkCmdBufBindUniformBuffers(DkCmdBuf c,DkStage stage,uint32_t,const DkBufExtents b[],uint32_t n) { check(n==1,"uniform count"); if(stage==DkStage_Fragment)c->parameters=reinterpret_cast<void *>(b[0].addr); }
void dkCmdBufBindColorState(DkCmdBuf c,const DkColorState *s) { c->blend=s->blendEnableMask; }
void dkCmdBufDraw(DkCmdBuf c,DkPrimitive,uint32_t count,uint32_t,uint32_t,uint32_t) {
  check(c->target&&count==6,"invalid graphics draw");
  if(c->blend) { ++c->ui; return; }
  DrawParameters p; std::memcpy(&p,c->parameters,sizeof(p));
  if(p.effect==3&&p.mode==2) {
    check(p.texture_size[0]==256&&p.texture_size[1]==192,"scanline source dimensions");
    check(p.target_size[0]==(expected_rotation&1?450:600)&&
          p.target_size[1]==(expected_rotation&1?600:450),"scanline target rotation"); ++scanline_draws;
  }
}
void dkCmdBufCopyImage(DkCmdBuf c,const DkImageView *src,const DkImageRect *sr,const DkImageView *dst,const DkImageRect *dr,uint32_t) {
  const auto a=get<Meta>(src->pImage),b=get<Meta>(dst->pImage);
  check(a.width==b.width&&a.height==b.height&&sr->width==dr->width&&sr->height==dr->height,"wrong LSFG copy extent"); c->copy_kind=a.kind;
}
void dkCmdBufCopyBufferToImage(DkCmdBuf,const DkCopyBuf *,const DkImageView *,const DkImageRect *,uint32_t) {}
#define NOOP(name,args) void name args {}
NOOP(dkCmdBufBarrier,(DkCmdBuf,DkBarrier,uint32_t))
NOOP(dkCmdBufBindImageDescriptorSet,(DkCmdBuf,DkGpuAddr,uint32_t))
NOOP(dkCmdBufBindSamplerDescriptorSet,(DkCmdBuf,DkGpuAddr,uint32_t))
NOOP(dkCmdBufBindShaders,(DkCmdBuf,uint32_t,const DkShader *const[],uint32_t))
NOOP(dkCmdBufBindVtxBuffers,(DkCmdBuf,uint32_t,const DkBufExtents[],uint32_t))
NOOP(dkCmdBufBindTextures,(DkCmdBuf,DkStage,uint32_t,const DkResHandle[],uint32_t))
NOOP(dkCmdBufSetViewports,(DkCmdBuf,uint32_t,const DkViewport[],uint32_t))
NOOP(dkCmdBufSetScissors,(DkCmdBuf,uint32_t,const DkScissor[],uint32_t))
NOOP(dkCmdBufClearColor,(DkCmdBuf,uint32_t,uint32_t,const void *))
NOOP(dkCmdBufBindRasterizerState,(DkCmdBuf,const DkRasterizerState *))
NOOP(dkCmdBufBindDepthStencilState,(DkCmdBuf,const DkDepthStencilState *))
NOOP(dkCmdBufBindMultisampleState,(DkCmdBuf,const DkMultisampleState *))
NOOP(dkCmdBufBindColorWriteState,(DkCmdBuf,const DkColorWriteState *))
NOOP(dkCmdBufBindBlendStates,(DkCmdBuf,uint32_t,const DkBlendState[],uint32_t))
NOOP(dkCmdBufBindVtxAttribState,(DkCmdBuf,const DkVtxAttribState[],uint32_t))
NOOP(dkCmdBufBindVtxBufferState,(DkCmdBuf,const DkVtxBufferState[],uint32_t))
#undef NOOP
}

struct DrasticDekoLsfg::State { DkImage history[3],generated; unsigned phase{},warmup{}; };
DrasticDekoLsfg::DrasticDekoLsfg(std::unique_ptr<State> s):state_(std::move(s)) {}
DrasticDekoLsfg::~DrasticDekoLsfg()=default;
std::unique_ptr<DrasticDekoLsfg> DrasticDekoLsfg::create(DkDevice,unsigned w,unsigned h,bool,unsigned,const char *,const char *,std::string &) {
  auto s=std::make_unique<State>(); for(auto &i:s->history) put(&i,Meta{w,h,0,0,next_id++,1});
  put(&s->generated,Meta{w,h,0,0,next_id++,2}); return std::unique_ptr<DrasticDekoLsfg>(new DrasticDekoLsfg(std::move(s)));
}
const DkImage *DrasticDekoLsfg::input() const { return &state_->history[state_->phase%3]; }
void DrasticDekoLsfg::reset() { state_->warmup=0; }
bool DrasticDekoLsfg::record(DkCmdBuf c,const DkImage *&generated,const DkImage *&original,std::string &) {
  check(c->target&&get<Meta>(c->target).id==get<Meta>(input()).id&&c->ui==0,"UI was interpolated or game missed history target");
  original=input(); generated=state_->warmup++>=2?&state_->generated:nullptr;
  next_kind=generated?2:1; ++state_->phase; return true;
}

int main() {
  try {
    const std::vector<uint32_t> overlay_pixels(1280*720);
    for(unsigned width:{1280u,1920u}) for(bool low:{false,true}) for(bool fg:{false,true}) {
      pref_low=low; pref_fg=fg; panel_width=width; panel_height=width*9/16;
      DrasticRuntimeConfig config{}; config.screen_count=2; config.stylus_visible=true;
      config.screens[0]={20,20,600,450,0,0}; config.screens[1]={640,20,600,450,1,1};
      initializing=true; check(drastic_deko_renderer_init(&config),drastic_deko_renderer_last_error()); initializing=false;
      const auto blocks=allocated_blocks;
      for(int rotation=0;rotation<4;++rotation) {
        config.rotation=expected_rotation=rotation; config.video_filter=DRASTIC_FILTER_SCANLINE;
        DrasticOverlayFrame overlay{overlay_pixels.data(),rotation&1?720:1280,rotation&1?1280:720,unsigned(rotation),true};
        for(unsigned i=0;i<8;++i) { drastic_deko_renderer_present(&config,nullptr,nullptr,nullptr,&overlay,true); check(allocated_blocks==blocks,"GPU allocation in presentation"); }
        // Cached menu draws must bypass generation, then reprime on resume.
        config.video_filter=DRASTIC_FILTER_NEAREST;
        next_kind=0; drastic_deko_renderer_present(&config,nullptr,nullptr,nullptr,&overlay,false);
        drastic_deko_renderer_suspend(); drastic_deko_renderer_resume();
      }
      drastic_deko_renderer_shutdown(); check(allocated_blocks==0&&commands.empty(),"native resource leak");
    }
    check(scanline_draws==512,"scanline path did not run in all rotations/resolutions");
    std::cout<<"Native presentation passed: 8 queue/LSFG/output configurations, "<<presented<<" presents; late capture, VI ownership, scanline, overlays and resource reuse\n";
  } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
