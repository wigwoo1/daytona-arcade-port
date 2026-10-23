#include "app/gpu_geometry.h"
#include "app/gpu_geometry_spirv.h"
#include <bit>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <filesystem>

namespace app {
GpuGeometry::GpuGeometry(SDL_GPUDevice *device, std::span<const uint8_t> private_shader) : device_(device) {
    if (!(SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_SPIRV)) return;
    SDL_GPUComputePipelineCreateInfo info{};
    info.code = reinterpret_cast<const Uint8 *>(gpu_geometry_spirv);
    info.code_size = sizeof(gpu_geometry_spirv);
    if (!private_shader.empty()) {
        if(private_shader.size()<20 || private_shader.size()%4!=0) return;
        uint32_t magic=0;std::memcpy(&magic,private_shader.data(),4);
        if(magic!=0x07230203) return;
        info.code=private_shader.data();info.code_size=private_shader.size();
    }
    info.entrypoint = "main"; info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.num_readonly_storage_buffers = 2;
    info.num_readwrite_storage_textures = 1;
    info.num_uniform_buffers = 1;
    info.threadcount_x = 8; info.threadcount_y = 8; info.threadcount_z = 1;
    pipeline_ = SDL_CreateGPUComputePipeline(device_, &info);
    if (!pipeline_) SDL_Log("GPU geometry unavailable: %s", SDL_GetError());
    if (pipeline_) load_replacements(false);
}
void GpuGeometry::load_replacements(bool include_pack) {
    if(replacement_buffer_)SDL_ReleaseGPUBuffer(device_,replacement_buffer_);
    if(replacement_upload_)SDL_ReleaseGPUTransferBuffer(device_,replacement_upload_);
    replacement_buffer_=nullptr;replacement_upload_=nullptr;
    replacement_uploaded_=false;
    replacements_.clear();
    replacement_candidates_.clear();
    replacement_pixels_.assign(9,0);
    std::ifstream file;
    if(include_pack)file.open(std::filesystem::path(SDL_GetBasePath())/"TexturePacks/300pct/pack.dtai",std::ios::binary);
    char magic[8]{}; uint32_t count=0;
    file.read(magic,8);file.read(reinterpret_cast<char *>(&count),4);
    const bool v9=std::memcmp(magic,"DTAI3009",8)==0;
    const bool v8=v9 || std::memcmp(magic,"DTAI3008",8)==0;
    const bool v7=v8 || std::memcmp(magic,"DTAI3007",8)==0;
    const bool v6=v7 || std::memcmp(magic,"DTAI3006",8)==0;
    const bool v5=v6 || std::memcmp(magic,"DTAI3005",8)==0;
    const bool v4=v5 || std::memcmp(magic,"DTAI3004",8)==0;
    const bool v3=v4 || std::memcmp(magic,"DTAI3003",8)==0;
    const bool v2=v3 || std::memcmp(magic,"DTAI3002",8)==0;
    uint32_t aliases=0;
    if(v3)file.read(reinterpret_cast<char *>(&aliases),4);
    if(file && !v4)SDL_Log("Texture pack lacks source identity; retaining original textures until pack is updated");
    if(file && v4 && count<=4096) {
        if(aliases>4096-count)aliases=0,file.setstate(std::ios::failbit);
        replacement_pixels_.resize(size_t(count+aliases+1)*9);
        for(uint32_t id=1;id<=count;id++) {
            uint64_t key=0;uint32_t dimensions[4]{};
            file.read(reinterpret_cast<char *>(&key),8);file.read(reinterpret_cast<char *>(dimensions),16);
            uint32_t mode=0;if(v2)file.read(reinterpret_cast<char *>(&mode),4);
            if(mode>1 && !(v6 && mode==2) && !(v8 && mode==3) && !(v9 && mode==4)){file.setstate(std::ios::failbit);break;}
            uint64_t source_hash=0;file.read(reinterpret_cast<char *>(&source_hash),8);
            uint32_t scale=3;
            if(v7)file.read(reinterpret_cast<char *>(&scale),4);
            if(scale!=3 && scale!=4){file.setstate(std::ios::failbit);break;}
            const auto [ow,oh,w,h]=std::array{dimensions[0],dimensions[1],dimensions[2],dimensions[3]};
            if(!file||!ow||!oh||ow>4096||oh>4096||!w||!h||w>8192||h>8192||uint64_t(w)*h>16777216) {
                file.setstate(std::ios::failbit);break;
            }
            std::vector<std::array<uint32_t,4>> allowed;
            if(v5) {
                uint32_t n=0;file.read(reinterpret_cast<char *>(&n),4);
                if(!file || n>256){file.setstate(std::ios::failbit);break;}
                allowed.resize(n);
                for(auto &r:allowed) {
                    file.read(reinterpret_cast<char *>(r.data()),16);
                    if(!file || r[0]>=r[2] || r[1]>=r[3] || r[2]>ow || r[3]>oh) {
                        file.setstate(std::ios::failbit);break;
                    }
                }
                if(!file)break;
            }
            if(mode==3 && (scale!=3 || allowed.empty())){file.setstate(std::ios::failbit);break;}
            // Mode4 blends a source-matched road repaint with native tones.
            // It requires an explicit paint-protection mask.
            if(mode==4 && (scale!=3 || allowed.empty())){file.setstate(std::ios::failbit);break;}
            std::vector<uint32_t> rgba(size_t(w)*h);
            file.read(reinterpret_cast<char *>(rgba.data()),rgba.size()*4);
            if(!file)break;
            const uint32_t tw=ow*scale,th=oh*scale;
            const uint64_t texels=uint64_t(tw)*th;
            const uint64_t words=mode!=0?(texels+1)/2:texels;
            if(replacement_pixels_.size()+words>33554432) {
                SDL_Log("Repainted texture pack exceeds the 128 MiB storage limit; remaining entries use originals");
                file.setstate(std::ios::failbit);
                break;
            }
            auto *source=SDL_CreateSurfaceFrom(w,h,SDL_PIXELFORMAT_RGBA32,rgba.data(),w*4);
            auto *target=SDL_CreateSurface(tw,th,SDL_PIXELFORMAT_ARGB8888);
            if(!source||!target){if(source)SDL_DestroySurface(source);if(target)SDL_DestroySurface(target);continue;}
            SDL_SetSurfaceBlendMode(source,SDL_BLENDMODE_NONE);
            if(SDL_BlitSurfaceScaled(source,nullptr,target,nullptr,SDL_SCALEMODE_LINEAR)) {
                uint32_t base=uint32_t(replacement_pixels_.size());
                replacement_pixels_[id*9]=base;replacement_pixels_[id*9+1]=tw;replacement_pixels_[id*9+2]=th;
                replacement_pixels_[id*9+3]=ow;replacement_pixels_[id*9+4]=oh;
                replacement_pixels_[id*9+5]=mode;
                replacement_pixels_[id*9+6]=tw;
                if(mode!=0) {
                    replacement_pixels_.resize(size_t(base)+size_t(words),0);
                    std::vector<uint8_t> road_guard;
                    if(mode==4) {
                        // Guard the union: rectangle seams are not paint boundaries.
                        std::vector<uint8_t> mask(size_t(ow)*oh,0);
                        for(const auto &r:allowed)
                            for(uint32_t y=r[1];y<r[3];++y)
                                std::fill(mask.begin()+size_t(y)*ow+r[0],mask.begin()+size_t(y)*ow+r[2],1);
                        road_guard.resize(mask.size(),0);
                        for(uint32_t y=1;y+1<oh;++y)for(uint32_t x=1;x+1<ow;++x) {
                            bool inside=true;
                            for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)
                                inside=inside && mask[size_t(int(y)+dy)*ow+uint32_t(int(x)+dx)]!=0;
                            road_guard[size_t(y)*ow+x]=inside;
                        }
                    }
                    for(uint32_t y=0;y<th;y++) {
                        const auto *row=reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(target->pixels)+y*target->pitch);
                        for(uint32_t x=0;x<tw;x++) {
                            const uint64_t texel=uint64_t(y)*tw+x;
                            uint32_t alpha=row[x]>>24;
                            if(mode==4) {
                                if(!road_guard[size_t(y/scale)*ow+x/scale])alpha=0;
                            } else if(!allowed.empty()) {
                                // Keep a native-texel guard around protected boundaries,
                                // including the shader's bilinear sampling footprint.
                                bool inside=false;
                                for(const auto &r:allowed)
                                    if(x>=(r[0]+1)*scale && x+scale<r[2]*scale && y>=(r[1]+1)*scale && y+scale<r[3]*scale)inside=true;
                                if(!inside)alpha=0;
                            }
                            const uint32_t luminance_alpha=((row[x]>>16)&255)|(alpha<<8);
                            replacement_pixels_[size_t(base+texel/2)]|=luminance_alpha<<((texel&1)*16);
                        }
                    }
                } else {
                    for(uint32_t y=0;y<th;y++) {
                        const auto *row=reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(target->pixels)+y*target->pitch);
                        replacement_pixels_.insert(replacement_pixels_.end(),row,row+tw);
                    }
                }
                replacement_candidates_.emplace(key,Replacement{id,source_hash});
            }
            SDL_DestroySurface(source);SDL_DestroySurface(target);
        }
        for(uint32_t i=0;i<aliases && file;i++) {
            uint64_t key=0;std::array<uint32_t,5> region{};
            file.read(reinterpret_cast<char *>(&key),8);
            file.read(reinterpret_cast<char *>(region.data()),20);
            uint64_t source_hash=0;file.read(reinterpret_cast<char *>(&source_hash),8);
            const auto [parent,x,y,w,h]=region;
            if(!file || !parent || parent>count || !w || !h)continue;
            const size_t p=size_t(parent)*9;
            if(!replacement_pixels_[p] || (replacement_pixels_[p+5]!=1 && replacement_pixels_[p+5]!=2 && replacement_pixels_[p+5]!=3 && replacement_pixels_[p+5]!=4) ||
               uint64_t(x)+w>replacement_pixels_[p+3] || uint64_t(y)+h>replacement_pixels_[p+4])continue;
            const uint32_t alias_scale=replacement_pixels_[p+1]/replacement_pixels_[p+3];
            if((alias_scale!=3 && alias_scale!=4) ||
               replacement_pixels_[p+1]!=replacement_pixels_[p+3]*alias_scale ||
               replacement_pixels_[p+2]!=replacement_pixels_[p+4]*alias_scale)continue;
            const uint32_t id=count+i+1;
            const size_t a=size_t(id)*9;
            replacement_pixels_[a]=replacement_pixels_[p];
            replacement_pixels_[a+1]=w*alias_scale;replacement_pixels_[a+2]=h*alias_scale;
            replacement_pixels_[a+3]=w;replacement_pixels_[a+4]=h;
            replacement_pixels_[a+5]=replacement_pixels_[p+5];replacement_pixels_[a+6]=replacement_pixels_[p+6];
            replacement_pixels_[a+7]=x*alias_scale;replacement_pixels_[a+8]=y*alias_scale;
            replacement_candidates_.emplace(key,Replacement{id,source_hash});
        }
    }
    SDL_GPUBufferCreateInfo bi{};bi.usage=SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;bi.size=uint32_t(replacement_pixels_.size()*4);
    replacement_buffer_=SDL_CreateGPUBuffer(device_,&bi);
    SDL_GPUTransferBufferCreateInfo ti{};ti.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;ti.size=bi.size;
    replacement_upload_=SDL_CreateGPUTransferBuffer(device_,&ti);
    if(include_pack)SDL_Log("Repainted texture pack: %zu materials at 300%%, %zu storage bytes",replacement_candidates_.size(),replacement_pixels_.size()*4);
}
uint32_t GpuGeometry::match_replacement(uint64_t key,const rt::VideoMem &memory) {
    if(auto cached=replacements_.find(key);cached!=replacements_.end())return cached->second;
    const auto [first,last]=replacement_candidates_.equal_range(key);
    if(first==last){replacements_.emplace(key,0);return 0;}
    const uint32_t header=uint32_t(key&0xffff),location=uint32_t((key>>32)&0xffff);
    const uint32_t w=32u<<(header&7),h=32u<<((header>>3)&7);
    const uint32_t bx=32*(location&63),by=32*((location>>6)&31);
    const uint32_t *sheet=(location&4096)?memory.tex1:memory.tex0;
    uint64_t hash=14695981039346656037ULL;
    for(uint32_t y=0;y<h;y++)for(uint32_t x=0;x<w;x++) {
        uint32_t xx=bx+x,yy=by+y;
        if(xx>=1024){xx-=1024;yy^=1024;}
        const uint32_t address=(yy/2)*512+xx/2;
        const uint32_t shift=(address&1)*16+(!(y&1)?8:0)+(!(x&1)?4:0);
        hash^=(sheet[(address>>1)&0x7ffff]>>shift)&15;
        hash*=1099511628211ULL;
    }
    uint32_t id=0;
    for(auto entry=first;entry!=last;entry++)if(entry->second.source_hash==hash){id=entry->second.id;break;}
    replacements_.emplace(key,id);return id;
}
GpuGeometry::~GpuGeometry() {
    if (upload_) SDL_ReleaseGPUTransferBuffer(device_, upload_);
    if (buffer_) SDL_ReleaseGPUBuffer(device_, buffer_);
    if (pipeline_) SDL_ReleaseGPUComputePipeline(device_, pipeline_);
    if (replacement_buffer_) SDL_ReleaseGPUBuffer(device_,replacement_buffer_);
    if (replacement_upload_) SDL_ReleaseGPUTransferBuffer(device_,replacement_upload_);
}
bool GpuGeometry::render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, const rt::Video &video,
                         int width, int height, bool depth, int texture_quality, bool diagnose_repaint,
                         bool experiment_repaint_detail) {
    if(pipeline_ && texture_quality==300 && !replacement_pack_checked_) {
        replacement_pack_checked_=true;
        load_replacements(true);
    }
    if (!pipeline_ || !replacement_buffer_ || !replacement_upload_) return false;
    const auto scene = video.presentation_scene();
    if (scene.back.size() != 496*384 || scene.front.size() != 496*384 || !scene.memory.tex0) return false;
    if(texture_quality==300 && (scene.memory.texture_revision==UINT64_MAX ||
       scene.memory.texture_revision!=matched_texture_revision_ || scene.memory.tex0!=matched_texture_base_)) {
        replacements_.clear();matched_texture_revision_=scene.memory.texture_revision;matched_texture_base_=scene.memory.tex0;
    }
    rows_.resize(height); for (auto &row : rows_) row.clear();
    replacement_spans_ = 0;
    coverage_.set_target(width,height,video.widescreen());
    coverage_.set_depth_buffer(depth);
    coverage_.set_span_sink([&](int renderer,int y,int x0,int x1,const float *start,const float *dx,const rt::Raster::Extra &o) {
        std::array<uint32_t,24> s{};
        s[0]=x0;s[1]=x1;
        for(int i=0;i<3;i++){s[2+i]=std::bit_cast<uint32_t>(start[i]);s[5+i]=std::bit_cast<uint32_t>(dx[i]);}
        s[8]=renderer;s[9]=o.checker|(o.utex<<1);s[10]=o.lumabase;s[11]=o.colorbase;
        if(texture_quality==300) {
            uint32_t id=match_replacement(o.texture_key,scene.memory);
            if(!id)id=match_replacement(o.texture_key&0x00001fff0000003fULL,scene.memory);
            if(id){s[9]|=id<<2;++replacement_spans_;}
        }
        s[12]=o.texwidth;s[13]=o.texheight;s[14]=o.texx;s[15]=o.texy;
        s[16]=o.texwrapx|(o.texwrapy<<1)|(o.texmirrorx<<2)|(o.texmirrory<<3);
        s[17]=uint32_t(o.texlod);s[18]=o.luma;s[19]=o.utexminlod;s[20]=o.utexx;s[21]=o.utexy;
        s[22]=o.texsheet[0]==scene.memory.tex1;s[23]=o.window;
        rows_[y].push_back(s);
    });
    coverage_.render(scene.polys,scene.windows,scene.memory,scene.crtc_x,scene.crtc_y,scene.render_x,scene.render_y,
                     video.widescreen() ? -83 : 0, video.widescreen() ? 578 : 495,0,383);
    coverage_.set_span_sink({});
    packet_.clear(); packet_.resize(height+1);
    uint32_t count=0;
    for(int y=0;y<height;y++){packet_[y]=count;count+=uint32_t(rows_[y].size());}
    packet_[height]=count;
    struct alignas(16) Uniforms { uint32_t dims[4], offsets[4], layers[4]; } u{};
    u.dims[0]=width;u.dims[1]=height;u.dims[2]=0;
    const uint32_t quality=texture_quality==50?1:texture_quality==25?2:texture_quality==10?3:0;
    u.dims[3]=uint32_t(depth)|(uint32_t(quality!=0)<<1)|(uint32_t(video.widescreen())<<3)|(quality<<4)|(uint32_t(diagnose_repaint)<<6)|(uint32_t(experiment_repaint_detail)<<7);
    u.offsets[0]=uint32_t(packet_.size());
    for(const auto &row:rows_)for(const auto &s:row)packet_.insert(packet_.end(),s.begin(),s.end());
    auto append=[&](const void *data,size_t bytes){
        uint32_t offset=uint32_t(packet_.size());size_t pos=packet_.size();
        packet_.resize(pos+(bytes+3)/4);std::memcpy(packet_.data()+pos,data,bytes);return offset;
    };
    u.offsets[1]=append(scene.memory.palram,0x4000);
    u.offsets[2]=append(scene.memory.colorxlat,0xc000);
    u.offsets[3]=append(scene.memory.lumaram,0x20000);
    u.layers[0]=append(scene.memory.tex0,0x200000);
    u.layers[1]=append(scene.memory.tex1,0x200000);
    u.layers[2]=append(scene.back.data(),scene.back.size()*4);
    u.layers[3]=append(scene.front.data(),scene.front.size()*4);
    const uint32_t bytes=uint32_t(packet_.size()*4);
    if(bytes>capacity_) {
        // Released resources remain alive for submitted command buffers.
        if(buffer_)SDL_ReleaseGPUBuffer(device_,buffer_);
        if(upload_)SDL_ReleaseGPUTransferBuffer(device_,upload_);
        buffer_=nullptr;upload_=nullptr;
        capacity_=(bytes+0xffff)&~0xffffu;
        SDL_GPUBufferCreateInfo bi{};bi.usage=SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;bi.size=capacity_;
        buffer_=SDL_CreateGPUBuffer(device_,&bi);
        SDL_GPUTransferBufferCreateInfo ti{};ti.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;ti.size=capacity_;
        upload_=SDL_CreateGPUTransferBuffer(device_,&ti);
        if(!buffer_||!upload_){capacity_=0;return false;}
    }
    void *mapped=SDL_MapGPUTransferBuffer(device_,upload_,true);
    if(!mapped)return false;
    std::memcpy(mapped,packet_.data(),bytes);SDL_UnmapGPUTransferBuffer(device_,upload_);
    auto *copy=SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src{};src.transfer_buffer=upload_;
    SDL_GPUBufferRegion dst{};dst.buffer=buffer_;dst.size=bytes;
    SDL_UploadToGPUBuffer(copy,&src,&dst,true);SDL_EndGPUCopyPass(copy);
    if(!replacement_uploaded_) {
        void *mapped=SDL_MapGPUTransferBuffer(device_,replacement_upload_,false);
        if(!mapped)return false;
        const uint32_t size=uint32_t(replacement_pixels_.size()*4);
        std::memcpy(mapped,replacement_pixels_.data(),size);SDL_UnmapGPUTransferBuffer(device_,replacement_upload_);
        auto *copy=SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation src{};src.transfer_buffer=replacement_upload_;
        SDL_GPUBufferRegion dst{};dst.buffer=replacement_buffer_;dst.size=size;
        SDL_UploadToGPUBuffer(copy,&src,&dst,false);SDL_EndGPUCopyPass(copy);
        replacement_uploaded_=true;
    }
    SDL_PushGPUComputeUniformData(cmd,0,&u,sizeof u);
    SDL_GPUStorageTextureReadWriteBinding out{};out.texture=target;out.cycle=true;
    auto *pass=SDL_BeginGPUComputePass(cmd,&out,1,nullptr,0);
    SDL_BindGPUComputePipeline(pass,pipeline_);
    SDL_GPUBuffer *buffers[]={buffer_,replacement_buffer_};
    SDL_BindGPUComputeStorageBuffers(pass,0,buffers,2);
    SDL_DispatchGPUCompute(pass,(width+7)/8,(height+7)/8,1);
    SDL_EndGPUComputePass(pass);
    return true;
}
}
