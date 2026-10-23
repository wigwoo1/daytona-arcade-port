#include "app/gpu_geometry.h"
#include "runtime/game_loop.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

// Private-ROM validation tool. Never packages or embeds ROM data.
int main(int argc,char **argv) {
    if(argc<5){std::fprintf(stderr,"check_gpu_geometry IMAGES FRAMES HEIGHT OUTPUT_PREFIX [WIDE_0_OR_1] [DEPTH_0_OR_1] [QUALITY] [AUDIT_OR_MINUS] [attract|scripted] [KEY_HEX] [COURSE_0_TO_2] [SOURCE_HASH_HEX]\n");return 2;}
    SDL_Init(SDL_INIT_VIDEO);
    auto *dev=SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV,true,"vulkan");
    if(!dev){std::fprintf(stderr,"%s\n",SDL_GetError());return 2;}
    bool wide=argc<6||std::atoi(argv[5])!=0,depth=argc<7||std::atoi(argv[6])!=0;
    int quality=argc>=8?std::atoi(argv[7]):100;
    const bool attract=argc>=10&&std::string(argv[9])=="attract";
    const bool audit=argc>=9&&std::string(argv[8])!="-";
    const uint64_t find_key=argc>=11?std::strtoull(argv[10],nullptr,16):0;
    const uint64_t find_source=argc>=13?std::strtoull(argv[12],nullptr,16):0;
    bool found_key=false;
    const bool visibility_scan=argc>=16 && std::string(argv[15])=="visibility-scan";
    const bool sequence_capture=argc>=16 && std::string(argv[15])=="sequence-capture";
    const int scan_start=argc>=17?std::max(1,std::atoi(argv[16])):1;
    const int scan_step=argc>=18?std::max(1,std::atoi(argv[17])):30;
    const size_t scan_min_pixels=argc>=19?size_t(std::max(1,std::atoi(argv[18]))):8;
    if((visibility_scan || sequence_capture) && quality!=300){std::fprintf(stderr,"Screen sampling requires private 300%% pack\n");return 2;}

    std::vector<uint8_t> scan_shader;
    if((visibility_scan || sequence_capture) && argc>=20) {
        std::ifstream shader(argv[19],std::ios::binary);
        if(!shader){std::fprintf(stderr,"Private scan shader file missing\n");return 2;}
        scan_shader.assign(std::istreambuf_iterator<char>(shader),std::istreambuf_iterator<char>());
        if(scan_shader.empty()){std::fprintf(stderr,"Private scan shader file empty\n");return 2;}
    }

    const int course_index=argc>=12?std::clamp(std::atoi(argv[11]),0,2):0;
    const bool course_script=argc>=12;
    int frames=std::atoi(argv[2]),height=std::atoi(argv[3]),width=int(std::lround(height*(wide?16.0/9.0:496.0/384.0)));
    auto game=std::make_unique<rt::GameLoop>(argv[1]);
    auto &video=game->board().video();
    const bool vertical_capture=argc>=31 && (std::string(argv[30])=="vertical-road-center" || std::string(argv[30])=="vertical-capture-only");
    const bool preclip_driver=argc>=31 && (std::string(argv[30])=="preclip-road-center" || std::string(argv[30])=="vertical-road-center");
    const bool preclip_capture=preclip_driver || (argc>=31 && std::string(argv[30])=="preclip-capture-only");
    game->board().geometry().record_unclipped=preclip_capture || vertical_capture;
    game->board().geometry().record_vertical_clip=vertical_capture;
    // Private trace of remaining462 and sequence-checked452 candidates.
    // Recording submissions does not alter native game or presentation output.
    const bool remaining_model_scan=argc>=29 && std::string(argv[28])=="remaining-model-scan";
    std::ofstream remaining_model_trace;
    if(remaining_model_scan) {
        game->board().geometry().record_objects=true;
        remaining_model_trace.open(std::string(argv[4])+"-remaining-models.tsv");
        if(!remaining_model_trace)return 2;
        remaining_model_trace<<"frame\tkind\tmodel\tpoints\theaders\tcount\n";
    }
    const bool visual_driver=argc>=24 && std::atoi(argv[23]);
    // The driver requires composed native geometry, not just presentation layers.
    video.enable_presentation(true,!visual_driver);video.set_tile_cache(true);
    video.set_widescreen(wide);video.set_depth_buffer(depth);video.set_texture_quality(quality);video.set_render_threads(16);
    game->board().track_extension=2;
    game->board().model_lod=argc>=14?unsigned(std::clamp(std::atoi(argv[13]),0,2)):0;
    std::set<std::array<uint16_t,4>> materials;
    std::set<std::array<uint16_t,4>> epoch_materials;
    std::set<std::pair<std::array<uint16_t,4>,uint64_t>> material_states;
    std::vector<std::string> state_records;
    uint64_t audit_revision=UINT64_MAX;
    // Private memory provenance probe; these regions need not be used by any polygon.
    const bool memory_probe=audit && argc>=21 && std::string(argv[20])=="memory-region-probe";
    std::set<std::pair<unsigned,uint64_t>> memory_probe_seen;
    std::vector<std::string> memory_probe_records;
    std::array<uint64_t,4> memory_probe_current{{UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX}};
    std::vector<std::string> memory_usage_records;
    std::set<std::pair<unsigned,std::array<uint16_t,4>>> memory_usage_seen;
    const std::array<uint64_t,4> original_probe_hash{{0x8c1dd2c3e1148970ULL,0xa81e84ff084570a3ULL,0x088470cc41ca4308ULL,0x2a5ea6749ce951c3ULL}};
    unsigned memory_usage_checked_frames=0;
    auto capture_memory_regions=[&](const rt::VideoMem &memory,unsigned frame) {
        const std::array<std::array<int,4>,4> areas{{{128,896,64,128},{640,896,64,32},{672,960,32,64},{512,896,64,128}}};
        for(unsigned id=0;id<areas.size();++id) {
            const auto [bx,by,w,h]=areas[id];
            std::vector<uint8_t> indices(size_t(w)*h);uint64_t hash=14695981039346656037ULL;
            for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
                int xx=bx+x,yy=by+y;if(xx>=1024){xx-=1024;yy^=1024;}
                unsigned address=(yy/2)*512+xx/2,tex=memory.tex0[(address>>1)&0x7ffff];
                if(address&1)tex>>=16;if(!(y&1))tex>>=8;if(!(x&1))tex>>=4;tex&=15;
                indices[size_t(y)*w+x]=uint8_t(tex);hash=(hash^tex)*1099511628211ULL;
            }
            if(memory_probe_current[id]==hash)continue;
            memory_probe_current[id]=hash;
            memory_probe_seen.emplace(id,hash);
            char name[80];std::snprintf(name,sizeof name,"region%u-%016llx.indices",id,static_cast<unsigned long long>(hash));
            const auto path=std::filesystem::path(argv[8])/"unbound-memory"/name;
            std::filesystem::create_directories(path.parent_path());std::ofstream file(path,std::ios::binary);
            file.write(reinterpret_cast<const char *>(indices.data()),indices.size());
            std::ostringstream record;record<<"{\"region\":"<<id<<",\"first_frame\":"<<frame<<",\"bounds\":["<<bx<<','<<by<<','<<w<<','<<h<<"],\"file\":\""<<name<<"\",\"polygon_usage_proven\":false}";
            memory_probe_records.push_back(record.str());
        }
    };

    auto export_material=[&](const rt::GeoPoly &poly,const rt::VideoMem &memory) {
        const auto *header=poly.texheader;
        const int mode=(header[0]>>13)&3;
        if(mode<2)return;
        const int w=32<<(header[0]&7),h=32<<((header[0]>>3)&7);
        const int bx=32*(header[2]&63),by=32*((header[2]>>6)&31);
        const auto *sheet=(header[2]&4096)?memory.tex1:memory.tex0;
        auto le16=[](const uint8_t *data,unsigned n){return unsigned(data[n*2])|(unsigned(data[n*2+1])<<8);};
        const unsigned pal=le16(memory.palram,4096+((header[3]>>6)&1023));
        const unsigned lumabase=(header[1]&255)<<7;
        std::vector<uint32_t> pixels(size_t(w)*h);
        std::vector<uint8_t> indices(size_t(w)*h);
        uint64_t source_hash=14695981039346656037ULL;
        for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
            int xx=bx+x,yy=by+y;if(xx>=1024){xx-=1024;yy^=1024;}
            const unsigned address=(yy/2)*512+xx/2;
            unsigned tex=sheet[(address>>1)&0x7ffff];
            if(address&1)tex>>=16;if(!(y&1))tex>>=8;if(!(x&1))tex>>=4;
            tex&=15;
            indices[size_t(y)*w+x]=uint8_t(tex);
            source_hash=(source_hash^tex)*1099511628211ULL;
            const unsigned luma=std::min(63u,unsigned(memory.lumaram[(lumabase+(tex*8))*4])*255/256);
            auto channel=[&](unsigned base,unsigned c){unsigned v=le16(memory.colorxlat,base+(c<<8)+luma)&255;return v<=64?0:(v-64)*255/191;};
            pixels[size_t(y)*w+x]=(mode==3&&tex==15?0:0xff000000u)|(channel(0,pal&31)<<16)|(channel(8192,(pal>>5)&31)<<8)|channel(16384,(pal>>10)&31);
        }
        const std::array<uint16_t,4> material{header[0],header[1],header[2],header[3]};
        if(!material_states.emplace(material,source_hash).second)return;
        char name[80];std::snprintf(name,sizeof name,"%04x-%04x-%04x-%04x-%016llx.bmp",header[0],header[1],header[2],header[3],static_cast<unsigned long long>(source_hash));
        const auto path=std::filesystem::path(argv[8])/"rgb-originals"/name;
        std::filesystem::create_directories(path.parent_path());
        auto raw_path=path;raw_path.replace_extension(".bgra");
        std::ofstream raw(raw_path,std::ios::binary);
        raw.write(reinterpret_cast<const char *>(pixels.data()),pixels.size()*4);
        auto *surface=SDL_CreateSurfaceFrom(w,h,SDL_PIXELFORMAT_ARGB8888,pixels.data(),w*4);
        if(surface){SDL_SaveBMP(surface,path.string().c_str());SDL_DestroySurface(surface);}
        auto native_path=std::filesystem::path(argv[8])/"native-indices"/name;
        native_path.replace_extension(".indices");
        std::filesystem::create_directories(native_path.parent_path());
        std::ofstream native(native_path,std::ios::binary);
        native.write(reinterpret_cast<const char *>(indices.data()),indices.size());
        state_records.push_back("{\"header\":["+std::to_string(header[0])+","+std::to_string(header[1])+","+std::to_string(header[2])+","+std::to_string(header[3])+"],\"source_hash\":"+std::to_string(source_hash)+",\"width\":"+std::to_string(w)+",\"height\":"+std::to_string(h)+",\"first_frame\":"+std::to_string(game->frames())+",\"file\":\""+native_path.filename().string()+"\"}");
    };
    std::ofstream location_scan;
    const bool sign_location_scan=argc>=16 && std::string(argv[15])=="sign-location-scan";
    const bool fade_location_scan=argc>=16 && (std::string(argv[15])=="target-fade-scan" || sign_location_scan);
    if(argc>=16 && (std::string(argv[15])=="target-location-scan" || fade_location_scan)) {
        location_scan.open(std::string(argv[4])+"-locations.tsv");
        if(fade_location_scan)location_scan.precision(9);
        location_scan << "frame\ttexture\tdepth_min\txmin\tymin\txmax\tymax";
        if(fade_location_scan)location_scan << "\ttexlod";
        location_scan << '\n';
    }
    std::vector<std::array<uint16_t,3>> wrongway_points;
    if(argc>=28 && std::string(argv[27])!="-") {
        std::ifstream signature(argv[27],std::ios::binary);uint32_t count=0;
        signature.read(reinterpret_cast<char *>(&count),4);
        if(!signature || count<300 || count>5000)return 2;
        for(uint32_t i=0;i<count;i++) {
            uint16_t x=0,y=0;uint8_t tone=0;
            signature.read(reinterpret_cast<char *>(&x),2);signature.read(reinterpret_cast<char *>(&y),2);signature.read(reinterpret_cast<char *>(&tone),1);
            if(!signature || x>=496 || y>=384 || (tone!=0 && tone!=255))return 2;
            wrongway_points.push_back({x,y,tone});
        }
    }
    int wrongway_stable=0,wrongway_absent=0,wrongway_turn=0,wrongway_cooldown=0;
    double wrongway_direction=1;
    std::ofstream road_driver_trace;
    const bool trace_road_driver=argc>=16 && std::string(argv[15])=="road-driver-trace";
    const int trace_first=argc>=17 && std::atoi(argv[16])>0?std::atoi(argv[16]):7200;
    const int trace_last=argc>=18 && std::atoi(argv[17])>trace_first?std::atoi(argv[17]):7800;
    if(trace_road_driver && trace_last<trace_first)return 2;
    if(trace_road_driver) {
        road_driver_trace.open(std::string(argv[4])+"-driver-road.tsv");
        if(!road_driver_trace)return 2;
        road_driver_trace << "frame\trow\tfound\tanchor\ttarget\tdistance\tintervals\n";
    }
    double driver_steer=0,driver_error=0,previous_road_anchor=248,driver_integral=0;
    const bool trace_driver_controls=argc>=30 && std::string(argv[29])=="driver-control-trace";
    std::ofstream driver_control_trace;
    if(trace_driver_controls) {
        driver_control_trace.open(std::string(argv[4])+"-driver-controls.tsv");
        if(!driver_control_trace)return 2;
        driver_control_trace<<"frame\ttarget\trows\tdisplaced_rows\terror\tcorrection\tsteer\tadc\n";
    }
    unsigned stripe_guard_pass=0,stripe_guard_fail=0,stripe_row_polygons=0;
    uint64_t bridge_guard_revision=UINT64_MAX;bool bridge_source_valid=false;
    unsigned bridge_guard_pass=0,bridge_guard_fail=0,bridge_row_polygons=0;
    const bool easy_validation=argc>=26 && std::atoi(argv[25]);
    for(int f=0;f<frames;f++) {
        rt::Inputs input;
        if(easy_validation && f<3000) {
            auto pulse=[&](int at,uint8_t bit,bool second=false) {if(f>=at && f<at+10) {if(second)input.in1&=~bit;else input.in0&=~bit;}};
            pulse(1200,0x04); // Native service menu, not a memory patch.
            pulse(1400,0x20);pulse(1440,0x20);pulse(1480,0x10);
            if(std::atoi(argv[25])==2 || std::atoi(argv[25])==3) {
                pulse(1520,0x01,true);pulse(1560,0x01,true);pulse(1600,0x01,true);
                pulse(1640,0x40); // Country JPN -> USA, verified in native menu.
                pulse(1680,0x20);pulse(1720,0x20);
                if(std::atoi(argv[25])==3) {
                    pulse(1760,0x40); // Also select native Easy difficulty.
                    pulse(1800,0x20);pulse(1840,0x10);
                    pulse(1900,0x01,true);pulse(1940,0x01,true);pulse(1980,0x10);
                } else {
                    pulse(1760,0x20);pulse(1800,0x10);
                    pulse(1860,0x01,true);pulse(1900,0x01,true);pulse(1940,0x10);
                }
            } else {
                pulse(1540,0x01,true);pulse(1580,0x40); // Difficulty -> Easy.
                if(std::atoi(argv[25])==4)pulse(1620,0x40); // Private native next-difficulty inspection.
                pulse(1640,0x20);pulse(1680,0x10); // Exit Game System.
                pulse(1740,0x01,true);pulse(1780,0x01,true);pulse(1820,0x10);
            }
        }
        const int actual_frame=f;
        if(!attract && (!easy_validation || f>=3000)) {
        const int f=actual_frame-(easy_validation?3000:0);
        const bool ordinary_race=argc>=25 && std::atoi(argv[24]);
        for(int n:{1200,1240,1280})if(f>=n&&f<=n+9)input.in0&=~1;
        for(int n:{1400,1600,2000,2400})if((!ordinary_race || n==1400) && f>=n&&f<=n+9)input.in0&=~0x10;
        if(!ordinary_race) for(int n:{1800,2200})if(f>=n&&f<=n+9)input.in0&=~0x20;
        if(course_index>=1 && f>=1400 && f<1610)input.steer=course_index==1?0xb0:0xe0;
        // Private mode26 selects the native manual car using ordinary steering and gearbox input.
        if(argc>=24 && std::atoi(argv[23])==26) {
            if(f>=2300 && f<2520)input.steer=0xe0;
            if(f>=2600)input.in1=uint8_t((input.in1&~0x70)|(5u<<4)); // ordinary fourth gear
        }
        // Optional private replay duration; existing captures retain their inputs.
        const int accel_end=argc>=23?std::max(5999,std::atoi(argv[22])):5999;
        if(f>=(course_script?1600:1400)&&f<=accel_end)input.accel=0xe0;
        // Private visual driving experiment, using only ordinary rendered pixels.
        // Never changes the production game's inputs or replay defaults.
        // Geometry-driving validation uses full ordinary accelerator input.
        // Existing fixed-input replay captures retain their historical 0xe0 throttle.
        if(argc>=24 && std::atoi(argv[23])>=2 && f>=1600 && f<=accel_end)input.accel=0xff;
        // Private mode 6 leaves steering neutral through the rolling-start sequence.
        const int visual_driver_start=argc>=24 && std::atoi(argv[23])>=2 && std::atoi(argv[23])!=6 && std::atoi(argv[23])!=7 && std::atoi(argv[23])!=13 && std::atoi(argv[23])!=14 && std::atoi(argv[23])!=15 && std::atoi(argv[23])!=16 && std::atoi(argv[23])!=17 && std::atoi(argv[23])!=18 && std::atoi(argv[23])!=29 && std::atoi(argv[23])!=30 && std::atoi(argv[23])!=31 && std::atoi(argv[23])!=32 && std::atoi(argv[23])!=8 && std::atoi(argv[23])!=9 && std::atoi(argv[23])!=28 && std::atoi(argv[23])!=27 && std::atoi(argv[23])!=26 && std::atoi(argv[23])!=24 && std::atoi(argv[23])!=23 && std::atoi(argv[23])!=22 && std::atoi(argv[23])!=21 && std::atoi(argv[23])!=19 && std::atoi(argv[23])!=20 && std::atoi(argv[23])!=10 && std::atoi(argv[23])!=11 && std::atoi(argv[23])!=12?2600:3400;
        if(argc>=24 && std::atoi(argv[23]) && f>=visual_driver_start) {
            const auto &screen=game->screen();
            double sum=0,near_sum=0,curvature_far_sum=0; int count=0,near_count=0,near_displaced=0,curvature_far_count=0; int displaced_road_rows=0; bool near_fallback=false;
            if(screen.size()==496*384) for(int y=210;y<240;y+=5) {
                int start=40,last=40,best_start=0,best_end=0,gap=0;
                for(int x=40;x<456;x++) {
                    const auto pixel=screen[size_t(y)*496+x];
                    const int r=(pixel>>16)&255,g=(pixel>>8)&255,b=pixel&255;
                    if(r>55 && r<215 && std::abs(r-g)<12 && std::abs(g-b)<12) {
                        last=x; gap=0;
                    } else if(++gap>6) {
                        if(last-start>best_end-best_start) {best_start=start;best_end=last;}
                        start=x+1;last=start;
                    }
                }
                if(last-start>best_end-best_start) {best_start=start;best_end=last;}
                if(best_end-best_start>35) {sum+=(best_start+best_end)*0.5;++count;}
            }
            if(std::atoi(argv[23])>=2) {
                sum=0;count=0;
                const auto scene=video.presentation_scene();
                const bool lane_driver=std::atoi(argv[23])==5 || std::atoi(argv[23])==7 || std::atoi(argv[23])==13 || std::atoi(argv[23])==14 || std::atoi(argv[23])==15 || std::atoi(argv[23])==16 || std::atoi(argv[23])==17 || (std::atoi(argv[23])==18 || (std::atoi(argv[23])==29 || std::atoi(argv[23])==30 || std::atoi(argv[23])==31 || std::atoi(argv[23])==32)) || std::atoi(argv[23])==9 || std::atoi(argv[23])==28 || std::atoi(argv[23])==27 || std::atoi(argv[23])==26 || std::atoi(argv[23])==24 || std::atoi(argv[23])==23 || std::atoi(argv[23])==22 || std::atoi(argv[23])==21 || std::atoi(argv[23])==19 || std::atoi(argv[23])==20 || std::atoi(argv[23])==10 || std::atoi(argv[23])==11 || std::atoi(argv[23])==12;
                bool stripe_source_valid=false;
                if((std::atoi(argv[23])==18 || (std::atoi(argv[23])==29 || std::atoi(argv[23])==30 || std::atoi(argv[23])==31 || std::atoi(argv[23])==32)) && actual_frame>=8100) {
                    uint64_t hash=14695981039346656037ULL;
                    for(int y=0;y<128;y++)for(int x=0;x<32;x++) {
                        const unsigned address=((512+y)/2)*512+x/2;
                        unsigned value=scene.memory.tex1[(address>>1)&0x7ffff];
                        if(address&1)value>>=16;if(!(y&1))value>>=8;if(!(x&1))value>>=4;
                        hash=(hash^(value&15))*1099511628211ULL;
                    }
                    stripe_source_valid=hash==0x39380e607f6f0c89ULL;
                    if(stripe_source_valid)++stripe_guard_pass;else ++stripe_guard_fail;
                }
                // Private mode29 admits verified bridge asphalt248 only with exact current source identity.
                if((std::atoi(argv[23])==29 || std::atoi(argv[23])==30 || std::atoi(argv[23])==31 || std::atoi(argv[23])==32) && actual_frame>=10000) {
                    if(bridge_guard_revision!=scene.memory.texture_revision) {
                        uint64_t hash=14695981039346656037ULL;
                        for(int y=0;y<256;y++)for(int x=0;x<256;x++) {
                            const unsigned address=((256+y)/2)*512+x/2;
                            unsigned value=scene.memory.tex1[(address>>1)&0x7ffff];
                            if(address&1)value>>=16;if(!(y&1))value>>=8;if(!(x&1))value>>=4;
                            hash=(hash^(value&15))*1099511628211ULL;
                        }
                        bridge_source_valid=hash==0xcb851bc16f15f927ULL;
                        bridge_guard_revision=scene.memory.texture_revision;
                    }
                    if(bridge_source_valid)++bridge_guard_pass;else ++bridge_guard_fail;
                }
                // Private mode23 uses nearest opaque road coverage rather than hidden projected road.
                std::vector<uint8_t> visible_road;
                if(std::atoi(argv[23])==23) {
                    visible_road.assign(496*384,0);
                    std::vector<float> invdepth(496*384,0);
                    std::vector<uint8_t> win(496*384,0);
                    rt::Raster coverage;coverage.set_target(496,384,false);coverage.set_depth_buffer(true);
                    coverage.set_span_sink([&](int renderer,int y,int x0,int x1,const float *start,const float *dx,const rt::Raster::Extra &o) {
                        if(renderer==3 || y<210 || y>285 || y%5)return;
                        const auto key=o.texture_key&0x00001fff0000003fULL;
                        const auto address=unsigned((key>>32)&0x1fff);
                        const bool road=renderer>=2 && (key&63)==27 && (address==3072 || address==7168);
                        for(int x=std::max(0,x0);x<std::min(496,x1);++x) {
                            if(o.checker && ((x^y)&1))continue;
                            const float inv=start[0]+float(x-x0)*dx[0];
                            if(!(inv>0) || !std::isfinite(inv))continue;
                            const size_t n=size_t(y)*496+x;
                            if(invdepth[n]>0 && (o.window<win[n] || (o.window==win[n] && inv<=invdepth[n]*1.001f)))continue;
                            invdepth[n]=inv;win[n]=o.window;visible_road[n]=uint8_t(road);
                        }
                    });
                    coverage.render(game->board().geometry().polys,scene.windows,scene.memory,scene.crtc_x,scene.crtc_y,scene.render_x,scene.render_y,0,495,0,383);
                }
                double lane_anchor=(std::atoi(argv[23])==16 || std::atoi(argv[23])==17 || (std::atoi(argv[23])==18 || (std::atoi(argv[23])==29 || std::atoi(argv[23])==30 || std::atoi(argv[23])==31 || std::atoi(argv[23])==32))) && actual_frame>=7700?previous_road_anchor:248;
                const int road_row_start=std::atoi(argv[23])==3?270:210;
                for(int sample=0;sample<(lane_driver?16:6);++sample) {
                    const int row=lane_driver?285-sample*5:road_row_start+sample*5;
                    std::vector<std::pair<double,double>> intervals;
                    for(const auto &poly:preclip_driver?game->board().geometry().unclipped_polys:game->board().geometry().polys) {
                        if(std::atoi(argv[23])==23)break;
                        if(poly.window>scene.windows)continue;
                        const unsigned address=poly.texheader[2]&0x1fff;
                        const bool main_road=(poly.texheader[0]&0x3f)==27 && (address==3072 || address==7168);
                        const bool stripe_road=stripe_source_valid && (poly.texheader[0]&0x3f)==16 && address==7168;
                        const bool bridge_road=(std::atoi(argv[23])==29 || std::atoi(argv[23])==30 || std::atoi(argv[23])==31 || std::atoi(argv[23])==32) && actual_frame>=10000 && bridge_source_valid && (poly.texheader[0]&0x3f)==27 && address==6656;
                        if(!main_road && !stripe_road && !bridge_road)continue;
                        if(bridge_road)++bridge_row_polygons;
                        if(stripe_road)++stripe_row_polygons;
                        double left=496,right=0;
                        for(unsigned i=0;i<poly.num_vertices;i++) {
                            const auto &a=poly.v[i],&b=poly.v[(i+1)%poly.num_vertices];
                            if(a.p[0]<=0 || b.p[0]<=0)continue;
                            const double ax=scene.crtc_x+poly.center[0]+a.x/a.p[0],bx=scene.crtc_x+poly.center[0]+b.x/b.p[0];
                            const double ay=scene.crtc_y+384-poly.center[1]-a.y/a.p[0],by=scene.crtc_y+384-poly.center[1]-b.y/b.p[0];
                            if((ay<=row && by>row)||(by<=row && ay>row)) {
                                const double x=ax+(bx-ax)*(row-ay)/(by-ay);
                                left=std::min(left,x);right=std::max(right,x);
                            }
                        }
                        if(right>left && right>0 && left<496) {
                            const bool raw_center=preclip_driver || (argc>=31 && std::string(argv[30])=="raw-road-center" && std::atoi(argv[23])==9);
                            intervals.emplace_back(raw_center?left:std::max(0.0,left),raw_center?right:std::min(496.0,right));
                        }
                    }
                    if(!visible_road.empty()) {
                        int begin=-1;
                        for(int x=0;x<=496;++x) {
                            const bool road=x<496 && visible_road[size_t(row)*496+x];
                            if(road && begin<0)begin=x;
                            if(!road && begin>=0){intervals.emplace_back(begin,x);begin=-1;}
                        }
                    }
                    // Keep disconnected road sections separate so distant lanes do not pull steering off-track.
                    std::sort(intervals.begin(),intervals.end());
                    std::vector<std::pair<double,double>> merged;
                    for(const auto &interval:intervals) {
                        if(merged.empty() || interval.first>merged.back().second+3)merged.push_back(interval);
                        else merged.back().second=std::max(merged.back().second,interval.second);
                    }
                    double best_distance=1e9,target=248;
                    bool found=false;
                    for(const auto &interval:merged) {
                        // Private diagnostic: a road span clipped at both screen edges
                        // has no measurable center and otherwise biases steering to 248.
                        if(argc>=31 && std::string(argv[30])=="unclipped-road" && std::atoi(argv[23])==9
                            && interval.first<=0 && interval.second>=496)continue;
                        if(interval.second-interval.first<=((std::atoi(argv[23])==16 || std::atoi(argv[23])==17 || (std::atoi(argv[23])==18 || (std::atoi(argv[23])==29 || std::atoi(argv[23])==30 || std::atoi(argv[23])==31 || std::atoi(argv[23])==32))) && actual_frame>=7700?8:35))continue;
                        const double anchor=lane_driver?lane_anchor:248;
                        const double distance=std::max({interval.first-anchor,anchor-interval.second,0.0});
                        if(distance<best_distance) {best_distance=distance;target=interval.first+(interval.second-interval.first)*(std::atoi(argv[23])==27?0.35:0.5);found=true;}
                    }
                    if((std::atoi(argv[23])==17 || (std::atoi(argv[23])==18 || (std::atoi(argv[23])==29 || std::atoi(argv[23])==30 || std::atoi(argv[23])==31 || std::atoi(argv[23])==32))) && actual_frame>=7700 && best_distance>100)found=false;
                    if(trace_road_driver && actual_frame>=trace_first && actual_frame<=trace_last && actual_frame%10==0) {
                        road_driver_trace << actual_frame << '\t' << row << '\t' << found << '\t' << lane_anchor << '\t' << target << '\t' << best_distance << '\t';
                        for(const auto &interval:merged)road_driver_trace << interval.first << ':' << interval.second << ';';
                        road_driver_trace << '\n';
                    }
                    if(found) {
                        if(lane_driver)lane_anchor=target;
                        if(std::atoi(argv[23])==16 || std::atoi(argv[23])==17 || (std::atoi(argv[23])==18 || (std::atoi(argv[23])==29 || std::atoi(argv[23])==30 || std::atoi(argv[23])==31 || std::atoi(argv[23])==32)))previous_road_anchor=target;
                        if((std::atoi(argv[23])==11 || std::atoi(argv[23])==12 || std::atoi(argv[23])==22) && row>235) {near_sum+=target;++near_count;if(best_distance>35)++near_displaced;}
                        if(std::atoi(argv[23])==22 && row<=235) {curvature_far_sum+=target;++curvature_far_count;}
                        if(!lane_driver || row<=235 || (std::atoi(argv[23])==30 && actual_frame>=10000) || std::atoi(argv[23])==9 || std::atoi(argv[23])==28 || std::atoi(argv[23])==27 || std::atoi(argv[23])==26 || std::atoi(argv[23])==24 || std::atoi(argv[23])==23 || std::atoi(argv[23])==22 || std::atoi(argv[23])==21 || std::atoi(argv[23])==19 || std::atoi(argv[23])==20) {const int weight=std::atoi(argv[23])==20 && row<=235?3:1;sum+=target*weight;count+=weight;if(best_distance>35)++displaced_road_rows;}
                    }
                }
            }
            if((std::atoi(argv[23])==11 || std::atoi(argv[23])==12) && count<3 && near_count>2) {
                sum=near_sum;count=near_count;displaced_road_rows=near_displaced;near_fallback=true;
            }
            if(count>2) {
                const double previous_error=driver_error;
                const bool lane_driver=std::atoi(argv[23])==5 || std::atoi(argv[23])==7 || std::atoi(argv[23])==13 || std::atoi(argv[23])==14 || std::atoi(argv[23])==15 || std::atoi(argv[23])==16 || std::atoi(argv[23])==17 || (std::atoi(argv[23])==18 || (std::atoi(argv[23])==29 || std::atoi(argv[23])==30 || std::atoi(argv[23])==31 || std::atoi(argv[23])==32)) || std::atoi(argv[23])==9 || std::atoi(argv[23])==28 || std::atoi(argv[23])==27 || std::atoi(argv[23])==26 || std::atoi(argv[23])==24 || std::atoi(argv[23])==23 || std::atoi(argv[23])==22 || std::atoi(argv[23])==21 || std::atoi(argv[23])==19 || std::atoi(argv[23])==20 || std::atoi(argv[23])==10 || std::atoi(argv[23])==11 || std::atoi(argv[23])==12;
                const bool responsive_driver=std::atoi(argv[23])==8 || std::atoi(argv[23])==9 || std::atoi(argv[23])==28 || std::atoi(argv[23])==27 || std::atoi(argv[23])==26 || std::atoi(argv[23])==24 || std::atoi(argv[23])==23 || std::atoi(argv[23])==22 || std::atoi(argv[23])==21 || std::atoi(argv[23])==19 || std::atoi(argv[23])==20;
                const bool private_fast_response=argc>=31 && std::string(argv[30])=="fast-response" && std::atoi(argv[23])==9;
                driver_error += (private_fast_response?0.5:(responsive_driver?0.15:(lane_driver?0.15:0.04)))*(sum/count-248.0-driver_error);
                const bool geometry_driver=std::atoi(argv[23])>=2;
                const bool recovery=geometry_driver && displaced_road_rows>2;
                const double limit=recovery?1.0:(lane_driver?0.75:(geometry_driver?0.55:0.35));
                double correction=std::clamp(driver_error/(recovery?120.0:(responsive_driver?(std::atoi(argv[23])==28?(std::abs(driver_error)>40.0?180.0:350.0):(std::atoi(argv[23])==19?180.0:(std::atoi(argv[23])==24?350.0:250.0))):(lane_driver?180.0:(geometry_driver?200.0:450.0))))+(responsive_driver?0.04:(lane_driver?0.02:0.15))*(driver_error-previous_error),-limit,limit);
                if(argc>=31 && std::string(argv[30])=="native-p200" && std::atoi(argv[23])==9 && !recovery)
                    correction=std::clamp(correction+driver_error*(1.0/200.0-1.0/250.0),-limit,limit);
                // Private bounded leaky correction for persistent oval corner offset.
                // Unlike mode21, it decays continuously and adds at most eight ADC steps.
                if(argc>=31 && std::string(argv[30])=="leaky-correction" && std::atoi(argv[23])==9) {
                    driver_integral=std::clamp(driver_integral*0.995+driver_error/250.0*0.002,-0.08,0.08);
                    correction=std::clamp(correction+driver_integral,-limit,limit);
                }
                if(std::atoi(argv[23])==22 && near_count>2 && curvature_far_count>2) {
                    const double bend=curvature_far_sum/curvature_far_count-near_sum/near_count;
                    correction=std::clamp(correction+std::clamp(bend/200.0,-0.25,0.25),-limit,limit);
                }
                if(std::atoi(argv[23])==21) {
                    // Private bounded integral term tests persistent corner offset.
                    driver_integral=std::clamp(driver_integral+driver_error/250.0*0.01,-0.35,0.35);
                    correction=std::clamp(correction+driver_integral,-limit,limit);
                }
                const double slew=private_fast_response?0.08:(responsive_driver?0.04:(lane_driver?0.04:0.015));
                driver_steer += std::clamp(correction-driver_steer,-slew,slew);
                input.steer=uint8_t(std::lround(128+(std::atoi(argv[23])==12?127:96)*driver_steer));
                if(trace_driver_controls && actual_frame%10==0)
                    driver_control_trace<<actual_frame<<'\t'<<sum/count<<'\t'<<count<<'\t'<<displaced_road_rows<<'\t'<<driver_error<<'\t'<<correction<<'\t'<<driver_steer<<'\t'<<unsigned(input.steer)<<'\n';
                // Private mode14 tests immediate proportional response with the same selected road.
                if(std::atoi(argv[23])==14 || (std::atoi(argv[23])==15 && actual_frame>=7500)) {
                    driver_error=sum/count-248.0;
                    const double direct=std::clamp(driver_error/150.0,-1.0,1.0);
                    driver_steer+=std::clamp(direct-driver_steer,-0.10,0.10);
                    input.steer=uint8_t(std::lround(128+127*driver_steer));
                }
                // Optional private mode 4 slows for large look-ahead steering errors.
                // Ordinary game inputs only; other replay modes remain unchanged.
                if(argc>=31 && std::string(argv[30])=="corner-lift" && std::atoi(argv[23])==9 && f<=accel_end) {
                    input.accel=std::abs(driver_error)>45.0?0xb0:0xff;
                    input.brake=0x20; // released native pedal, including during a lift
                }
                if((std::atoi(argv[23])==4 || std::atoi(argv[23])==10 || std::atoi(argv[23])==11 || std::atoi(argv[23])==12) && f<=accel_end) {
                    const double bend=std::abs(driver_error);
                    input.accel=bend>70?0x90:(bend>40?0xc0:0xff);
                    input.brake=bend>100?0x50:0;
                    if(std::atoi(argv[23])==10 || std::atoi(argv[23])==11 || std::atoi(argv[23])==12) {
                        input.accel=bend>50?0xb0:0xff;
                        input.brake=bend>80?0x30:0;
                    }
                }
            }
            if((std::atoi(argv[23])==11 || std::atoi(argv[23])==12) && f<=accel_end) {
                if(near_fallback) {input.accel=0xa0;input.brake=0x30;}
                else if(count<3) {input.accel=0x70;input.brake=0x50;input.steer=uint8_t(std::lround(128+(std::atoi(argv[23])==12?127:96)*driver_steer));}
                if(f%600==0)std::printf("ROAD_FALLBACK frame=%d near=%d rows=%d\n",f,int(near_fallback),count);
            }
            // Private mode13 repeats mode7 steering but brakes before the observed harbour turn.
            // Only normal pedal inputs; no game state, timer, physics or production changes.
            if((std::atoi(argv[23])==13 || std::atoi(argv[23])==14 || std::atoi(argv[23])==15 || std::atoi(argv[23])==16 || std::atoi(argv[23])==17 || (std::atoi(argv[23])==18 || (std::atoi(argv[23])==29 || std::atoi(argv[23])==30 || std::atoi(argv[23])==31 || std::atoi(argv[23])==32))) && actual_frame>=7350 && actual_frame<7650 && f<=accel_end) {
                input.accel=actual_frame<7480?0:0x70;
                input.brake=actual_frame<7480?0x90:0;
            }
            // Private mode31 retains mode29 steering and source guards, and eases
            // ordinary pedals before the observed coastal exit loss. No state edits.
            if(std::atoi(argv[23])==31 && actual_frame>=13200 && actual_frame<13500 && f<=accel_end) {
                input.accel=actual_frame<13320?0:0xb0;
                input.brake=actual_frame<13320?0x70:0x20;
            }
            // Private mode32 retains driver29 steering and scales normal pedals
            // to the observed look-ahead bend after the coastal approach.
            if(std::atoi(argv[23])==32 && actual_frame>=13200 && f<=accel_end) {
                const double bend=std::abs(driver_error);
                input.accel=bend>90?0x90:(bend>50?0xb0:0xff);
                input.brake=bend>90?0x50:0x20;
            }
            if(!wrongway_points.empty() && screen.size()==496*384) {
                size_t matches=0;
                for(const auto &point:wrongway_points) {
                    const uint32_t pixel=screen[size_t(point[1])*496+point[0]];
                    const unsigned r=(pixel>>16)&255,g=(pixel>>8)&255,b=pixel&255;
                    if(point[2]?r>=248 && g>=248 && b>=248:r<=8 && g<=8 && b<=8)++matches;
                }
                const bool wrongway=matches*100>=wrongway_points.size()*96;
                wrongway_stable=wrongway?wrongway_stable+1:0;
                wrongway_absent=wrongway?0:wrongway_absent+1;
                if(wrongway_cooldown>0)--wrongway_cooldown;
                if(!wrongway_turn && !wrongway_cooldown && wrongway_stable>=8) {
                    wrongway_turn=300;wrongway_direction=count>2 && sum/count<248?-1:1;
                    std::printf("WRONGWAY_RECOVERY frame=%d start direction=%.0f matches=%zu\n",actual_frame,wrongway_direction,matches);
                }
                if(wrongway_turn) {
                    if(wrongway_turn<240 && wrongway_absent>=120) {
                        wrongway_turn=0;wrongway_cooldown=300;driver_steer=driver_error=0;
                        std::printf("WRONGWAY_RECOVERY frame=%d clear\n",actual_frame);
                    } else {
                        input.steer=uint8_t(std::lround(128+96*wrongway_direction));
                        input.accel=0x90;input.brake=wrongway_turn>270?0xc0:0;
                        if(--wrongway_turn==0) {wrongway_cooldown=300;driver_steer=driver_error=0;}
                    }
                }
            }
            if(trace_road_driver && actual_frame>=trace_first && actual_frame<=trace_last && actual_frame%10==0) {
                std::printf("CONTROL_DRIVER actual=%d target=%.4f filtered_error=%.4f steer_state=%.4f input=%u accel=%u brake=%u\n",actual_frame,count?sum/count:248.0,driver_error,driver_steer,unsigned(input.steer),unsigned(input.accel),unsigned(input.brake));
            }
            if(f%600==0 || (trace_road_driver && actual_frame>=trace_first && actual_frame<=trace_last && actual_frame%30==0)) {
                std::printf("VISUAL_DRIVER frame=%d road_rows=%d target_x=%.2f steer=%u\n",f,count,count?sum/count:248.0,unsigned(input.steer));
                if(screen.size()==496*384) {
                    auto surface=SDL_CreateSurfaceFrom(496,384,SDL_PIXELFORMAT_ARGB8888,const_cast<uint32_t *>(screen.data()),496*4);
                    if(surface) {SDL_SaveBMP(surface,(std::string(argv[4])+"-driver"+std::to_string(f)+".bmp").c_str());SDL_DestroySurface(surface);}
                }
            }
        }
        }
        // Private validation camera selector; default arguments preserve old replays.
        const int validation_camera=argc>=22?std::clamp(std::atoi(argv[21]),0,4):0;
        const int camera_switch_frame=argc>=27?std::max(3000,std::atoi(argv[26])):3000;
        const int active_camera=argc>=27 && f<camera_switch_frame?1:validation_camera;
        const int camera_pulse_origin=argc>=27 && f>=camera_switch_frame?camera_switch_frame:3000;
        if(active_camera && f>=camera_pulse_origin && (f-camera_pulse_origin)%600<10) {
            if(active_camera==4) input.in1&=~1u;
            else input.in0&=~(0x20u<<(active_camera-1));
        }
        // Private validation must be comparable to the production control ADC range.
        if(preclip_capture || vertical_capture || (argc>=31 && (std::string(argv[30])=="native-throttle" || std::string(argv[30])=="raw-road-center" || std::string(argv[30])=="native-p200")))
            input.accel=std::clamp<uint8_t>(input.accel,0x20,0xe0);
        game->run_frame(input);
        if(location_scan.is_open()) {
            const auto scene=video.presentation_scene();
            for(const auto &poly:scene.polys) {
                const int texture=sign_location_scan?((poly.texheader[0]&0x3f)==19 && (poly.texheader[2]&0x1fff)==3856?571:0):
                    (poly.texheader[0]==17624 && poly.texheader[2]==2606)?686:
                    (poly.texheader[0]==25794 && poly.texheader[2]==3868)?693:0;
                if(!texture)continue;
                double xmin=1e9,ymin=1e9,xmax=-1e9,ymax=-1e9,zmin=1e9;
                for(unsigned n=0;n<poly.num_vertices;n++) {
                    const auto &v=poly.v[n];if(v.p[0]<=0)continue;
                    const double x=scene.crtc_x+poly.center[0]+v.x/v.p[0];
                    const double y=scene.crtc_y+384-poly.center[1]-v.y/v.p[0];
                    xmin=std::min(xmin,x);xmax=std::max(xmax,x);ymin=std::min(ymin,y);ymax=std::max(ymax,y);zmin=std::min(zmin,double(v.p[0]));
                }
                if(xmax>0 && xmin<496 && ymax>0 && ymin<384) {
                    location_scan << f+1 << '\t' << texture << '\t' << zmin << '\t' << xmin << '\t' << ymin << '\t' << xmax << '\t' << ymax;
                    if(fade_location_scan)location_scan << '\t' << poly.texlod;
                    location_scan << '\n';
                }
            }
        }
        if(remaining_model_scan) {
            const auto &geometry=game->board().geometry();
            auto record=[&](const auto &objects,const char *kind) {
                for(const auto &object:objects) {
                    if(object.address!=8466996u && object.address!=8516948u &&
                       object.address!=8541704u && object.address!=8564881u &&
                       object.address!=8565110u && object.address!=8574567u &&
                       object.address!=8575026u && object.address!=8602238u &&
                       object.address!=8470478u && object.address!=8492384u &&
                       object.address!=8494245u)continue;
                    remaining_model_trace<<f+1<<'\t'<<kind<<'\t'<<object.address<<'\t'<<object.texture_points<<'\t'<<object.texture_headers<<'\t'<<object.count<<'\n';
                }
            };
            record(geometry.object_submissions,"native");
            record(geometry.extra_track_objects,"extension");
        }
        if (f == frames-1) std::printf("LOD mode=%u substitutions=%u native_faces=%zu presentation_faces=%zu\n", game->board().model_lod, game->board().geometry().overridden_objects, game->board().geometry().polys.size(), video.presentation_scene().polys.size());
        if(audit) {
            const auto scene=video.presentation_scene();
            if(scene.memory.texture_revision!=audit_revision || audit_revision==UINT64_MAX) {
                epoch_materials.clear();audit_revision=scene.memory.texture_revision;
                if(memory_probe)capture_memory_regions(scene.memory,unsigned(f+1));
            }
            if(memory_probe)++memory_usage_checked_frames;
            for(const auto &poly:scene.polys) {
                if(memory_probe && ((poly.texheader[0]>>13)&3)>=2 && !(poly.texheader[2]&4096)) {
                    const std::array<std::array<int,4>,4> areas{{{128,896,64,128},{640,896,64,32},{672,960,32,64},{512,896,64,128}}};
                    int sx=32*(poly.texheader[2]&63),sy=32*((poly.texheader[2]>>6)&31);
                    const int sw=32<<(poly.texheader[0]&7),sh=32<<((poly.texheader[0]>>3)&7);
                    if(sx>=1024){sx-=1024;sy^=1024;}
                    for(unsigned id=0;id<areas.size();++id) {
                        if(memory_probe_current[id]!=original_probe_hash[id])continue;
                        const auto [x,y,w,h]=areas[id];
                        // Conservative source rectangle overlap; no UV or screen visibility assertion.
                        bool overlap=sx<x+w && sx+std::min(sw,1024-sx)>x && sy<y+h && sy+sh>y;
                        if(sw>1024-sx)overlap|=0<x+w && sw-(1024-sx)>x && (sy^1024)<y+h && (sy^1024)+sh>y;
                        if(!overlap)continue;
                        const std::array<uint16_t,4> header{poly.texheader[0],poly.texheader[1],poly.texheader[2],poly.texheader[3]};
                        if(!memory_usage_seen.emplace(id,header).second)continue;
                        std::ostringstream record;record<<"{\"region\":"<<id<<",\"first_frame\":"<<(f+1)<<",\"header\":["<<header[0]<<','<<header[1]<<','<<header[2]<<','<<header[3]<<"],\"screen_visibility_proven\":false}";
                        memory_usage_records.push_back(record.str());
                    }
                }
                const std::array<uint16_t,4> material{poly.texheader[0],poly.texheader[1],poly.texheader[2],poly.texheader[3]};
                materials.insert(material);
                if(epoch_materials.insert(material).second)export_material(poly,scene.memory);
            }
        }
        if(game->sound()){game->sound()->take_fm();game->sound()->take_pcm();}
        if((visibility_scan || sequence_capture) && f+1>=scan_start && (f+1-scan_start)%scan_step==0) {
            // Private one-pack screen coverage search. Normal gameplay and renderer are unchanged.
            SDL_GPUTextureCreateInfo scan_info{};scan_info.type=SDL_GPU_TEXTURETYPE_2D;
            scan_info.format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            scan_info.usage=SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE|SDL_GPU_TEXTUREUSAGE_SAMPLER;
            scan_info.width=640;scan_info.height=360;scan_info.layer_count_or_depth=1;scan_info.num_levels=1;
            auto *scan_texture=SDL_CreateGPUTexture(dev,&scan_info);
            SDL_GPUTransferBufferCreateInfo scan_download_info{};scan_download_info.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;scan_download_info.size=640*360*4;
            auto *scan_download=SDL_CreateGPUTransferBuffer(dev,&scan_download_info);
            if(!scan_texture||!scan_download)return 2;
            size_t active=0,protected_pixels=0,spans=0;
            {
                app::GpuGeometry scan_renderer(dev,scan_shader);
                std::vector<uint8_t> baseline(640*360*4);
                for(int pass=0;pass<(sequence_capture?3:2);pass++) {
                auto *cmd=SDL_AcquireGPUCommandBuffer(dev);
                if(!scan_renderer.render(cmd,scan_texture,video,640,360,depth,pass==0?100:300,sequence_capture?pass==2:pass!=0,false))return 2;
                auto *copy=SDL_BeginGPUCopyPass(cmd);SDL_GPUTextureRegion source{};
                source.texture=scan_texture;source.w=640;source.h=360;source.d=1;
                SDL_GPUTextureTransferInfo dest{};dest.transfer_buffer=scan_download;
                SDL_DownloadFromGPUTexture(copy,&source,&dest);SDL_EndGPUCopyPass(copy);
                auto *fence=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
                if(!fence||!SDL_WaitForGPUFences(dev,true,&fence,1))return 2;
                SDL_ReleaseGPUFence(dev,fence);
                const auto *pixels=static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(dev,scan_download,false));
                if(!pixels)return 2;
                if(sequence_capture) {
                    const char *label=pass==0?"native":pass==1?"detail":"diagnostic";
                    std::ofstream capture(std::string(argv[4])+"-frame"+std::to_string(f+1)+"-"+label+".rgba",std::ios::binary);
                    capture.write(reinterpret_cast<const char *>(pixels),640*360*4);
                    if(!capture)return 2;
                }
                if(pass==0)std::copy(pixels,pixels+baseline.size(),baseline.begin());
                else for(size_t n=0;n<640*360;n++) {
                    const auto *c=pixels+n*4;
                    if(std::equal(c,c+4,baseline.data()+n*4))continue;
                    if(c[0]==0&&c[1]==255&&c[2]==0&&c[3]==255)++active;
                    if(c[0]==255&&c[1]==0&&c[2]==255&&c[3]==255)++protected_pixels;
                }
                SDL_UnmapGPUTransferBuffer(dev,scan_download);spans=scan_renderer.replacement_spans();
                }
            }
            SDL_ReleaseGPUTransferBuffer(dev,scan_download);SDL_ReleaseGPUTexture(dev,scan_texture);
            std::printf("VISIBILITY_SCAN frame=%d active=%zu protected=%zu spans=%zu\n",f+1,active,protected_pixels,spans);std::fflush(stdout);
            if(!sequence_capture && active>=scan_min_pixels && spans>0){found_key=true;frames=f+1;std::printf("Visible replacement found at frame=%d\n",frames);break;}
        }
        if(find_key && !visibility_scan) {
            for(const auto &poly:video.presentation_scene().polys) {
                uint64_t key=0;
                for(unsigned i=0;i<4;i++)key|=uint64_t(poly.texheader[i])<<(i*16);
                if((key&0x00001fff0000003fULL)!=find_key)continue;
                if(find_source) {
                    const auto scene=video.presentation_scene();
                    const auto *header=poly.texheader;
                    const auto *sheet=(header[2]&4096)?scene.memory.tex1:scene.memory.tex0;
                    const int w=32<<(header[0]&7),h=32<<((header[0]>>3)&7);
                    const int bx=32*(header[2]&63),by=32*((header[2]>>6)&31);
                    uint64_t hash=14695981039346656037ULL;
                    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
                        int xx=bx+x,yy=by+y;if(xx>=1024){xx-=1024;yy^=1024;}
                        const unsigned address=(yy/2)*512+xx/2;
                        unsigned tex=sheet[(address>>1)&0x7ffff];
                        if(address&1)tex>>=16;if(!(y&1))tex>>=8;if(!(x&1))tex>>=4;
                        hash=(hash^(tex&15))*1099511628211ULL;
                    }
                    if(hash!=find_source)continue;
                    std::printf("Requested source matched hash=%016llx\n",static_cast<unsigned long long>(hash));
                }
                found_key=true;
            }
            if(found_key){frames=f+1;std::printf("Requested texture found at frame=%d\n",frames);break;}
        }
    }
    if((find_key||visibility_scan)&&!found_key){std::fprintf(stderr,"Requested texture not found\n");SDL_DestroyGPUDevice(dev);SDL_Quit();return 3;}
    if(stripe_guard_pass || stripe_guard_fail)std::printf("STRIPE_SOURCE_GUARD matched_frames=%u declined_frames=%u accepted_row_polygons=%u\n",stripe_guard_pass,stripe_guard_fail,stripe_row_polygons);
    if(argc>=16 && (std::string(argv[15])=="geometry-report" || std::string(argv[15])=="road-visibility-report")) {
        const auto scene=video.presentation_scene();
        std::ofstream report(std::string(argv[4])+"-geometry.json");
        report << "{\"frame\":" << frames << ",\"crtc\":[" << scene.crtc_x << ',' << scene.crtc_y << "],\"polys\":[";
        bool first_poly=true;
        for(const auto &poly:scene.polys) {
            if(!first_poly)report << ',';first_poly=false;
            report << "{\"header\":[" << poly.texheader[0] << ',' << poly.texheader[1] << ',' << poly.texheader[2] << ',' << poly.texheader[3]
                   << "],\"center\":[" << poly.center[0] << ',' << poly.center[1] << "],\"luma\":" << unsigned(poly.luma)
                   << ",\"texlod\":" << poly.texlod << ",\"vertices\":[";
            for(int n=0;n<poly.num_vertices;n++) {
                if(n)report << ',';
                const auto &v=poly.v[n];report << '[' << v.x << ',' << v.y << ',' << v.p[0] << ',' << v.p[1] << ',' << v.p[2] << ']';
            }
            report << "]}";
        }
        report << "]}";
    }
    if(argc>=16 && std::string(argv[15])=="road-visibility-report") {
        const auto scene=video.presentation_scene();
        std::vector<float> inverse(496*384,0);
        std::vector<uint8_t> windows(496*384,0),road(496*384,0);
        std::vector<uint64_t> nearest_key(496*384,0);
        std::vector<int> nearest_renderer(496*384,-1);
        rt::Raster coverage;coverage.set_target(496,384,false);coverage.set_depth_buffer(true);
        coverage.set_span_sink([&](int renderer,int y,int x0,int x1,const float *start,const float *dx,const rt::Raster::Extra &o) {
            // Opaque-surface estimate only: cutouts and tile layers are excluded.
            if(renderer==3 || y<210 || y>350)return;
            const auto key=o.texture_key&0x00001fff0000003fULL;
            const auto address=unsigned((key>>32)&0x1fff);
            const bool asphalt=renderer>=2 && (key&63)==27 && (address==3072 || address==7168);
            for(int x=std::max(0,x0);x<std::min(496,x1);++x) {
                if(o.checker && ((x^y)&1))continue;
                const float inv=start[0]+float(x-x0)*dx[0];
                if(!(inv>0) || !std::isfinite(inv))continue;
                const size_t n=size_t(y)*496+x;
                if(inverse[n]>0 && (o.window<windows[n] || (o.window==windows[n] && inv<=inverse[n]*1.001f)))continue;
                inverse[n]=inv;windows[n]=o.window;road[n]=uint8_t(asphalt);nearest_key[n]=o.texture_key;nearest_renderer[n]=renderer;
            }
        });
        coverage.render(game->board().geometry().polys,scene.windows,scene.memory,scene.crtc_x,scene.crtc_y,scene.render_x,scene.render_y,0,495,0,383);
        std::ofstream materials(std::string(argv[4])+"-nearest-materials.tsv");
        materials << "row\tleft\tright\tkey\trenderer\n";
        for(int y=210;y<=350;y+=10) {
            int begin=0;
            for(int x=1;x<=496;++x) {
                const size_t n=size_t(y)*496+begin;
                if(x<496 && nearest_key[size_t(y)*496+x]==nearest_key[n] && nearest_renderer[size_t(y)*496+x]==nearest_renderer[n])continue;
                materials << y << '\t' << begin << '\t' << x << '\t' << nearest_key[n] << '\t' << nearest_renderer[n] << '\n';begin=x;
            }
        }
        std::ofstream report(std::string(argv[4])+"-opaque-road.tsv");
        report<<"row\tleft\tright\n";
        for(int y=210;y<=285;y+=5) {
            int begin=-1;
            for(int x=0;x<=496;++x) {
                const bool on=x<496 && road[size_t(y)*496+x];
                if(on && begin<0)begin=x;
                if(!on && begin>=0){report<<y<<'\t'<<begin<<'\t'<<x<<'\n';begin=-1;}
            }
        }
    }
    if(argc>=24 && (std::atoi(argv[23])==29 || std::atoi(argv[23])==30 || std::atoi(argv[23])==31 || std::atoi(argv[23])==32))std::printf("BRIDGE_SOURCE_GUARD matched_frames=%u declined_frames=%u accepted_row_polygons=%u\n",bridge_guard_pass,bridge_guard_fail,bridge_row_polygons);
    if(audit) {
        // Private asset audit only. Captured RAM is never source-distributed.
        const std::filesystem::path directory(argv[8]);
        std::filesystem::create_directories(directory);
        std::ofstream states(directory/"material-states.json");
        states<<"{\"frames\":"<<frames<<",\"course_index\":"<<course_index<<",\"states\":[";
        for(size_t i=0;i<state_records.size();i++){if(i)states<<",";states<<state_records[i];}
        states<<"]}";
        if(memory_probe) {
            std::ofstream probe(directory/"unbound-memory-states.json");
            probe<<"{\"scope\":\"Chronological texture memory transitions; first_frame starts this interval, repeated files are recurring content; no polygon usage or screen visibility claim\",\"states\":[";
            for(size_t i=0;i<memory_probe_records.size();++i){if(i)probe<<',';probe<<memory_probe_records[i];}
            probe<<"],\"usage_checked_frames\":"<<memory_usage_checked_frames<<",\"source_rectangle_usage_candidates\":[";
            for(size_t i=0;i<memory_usage_records.size();++i){if(i)probe<<',';probe<<memory_usage_records[i];}
            probe<<"]}";
        }

        const auto scene=video.presentation_scene();
        for(int sheet=0;sheet<2;sheet++) {
            std::ofstream file(directory/(sheet?"sheet1.bin":"sheet0.bin"),std::ios::binary);
            file.write(reinterpret_cast<const char *>(sheet?scene.memory.tex1:scene.memory.tex0),0x200000);
        }
        for(const auto &[name,data,size]:std::array<std::tuple<const char *,const uint8_t *,size_t>,3>{
                std::tuple{"palette.bin",scene.memory.palram,size_t(0x4000)},
                std::tuple{"color.bin",scene.memory.colorxlat,size_t(0xc000)},
                std::tuple{"luma.bin",scene.memory.lumaram,size_t(0x20000)}}) {
            std::ofstream file(directory/name,std::ios::binary);
            file.write(reinterpret_cast<const char *>(data),size);
        }
        std::ofstream file(directory/"materials.json");
        file << "{\"frames\":" << frames << ",\"headers\":[";
        bool first=true;
        for(const auto &header:materials) {
            if(!first)file << ','; first=false;
            file << '[' << header[0] << ',' << header[1] << ',' << header[2] << ',' << header[3] << ']';
        }
        file << "]}";
        std::ofstream current(directory/"current-materials.json");
        current << '[';first=true;
        for(const auto &poly:scene.polys){if(!first)current << ',';first=false;
            current << '[' << poly.texheader[0] << ',' << poly.texheader[1] << ',' << poly.texheader[2] << ',' << poly.texheader[3] << ',' << unsigned(poly.luma) << ',' << poly.texlod << ']';}
        current << ']';
    }
    // Controlled presentation fixture only: keep a source-matched live material,
    // palette and lighting, and display its full UV range on an isolated quad.
    // This is not a gameplay capture or evidence of full-scene visibility.
    std::vector<uint8_t> fixture_tiles,fixture_chars;
    std::unique_ptr<rt::Video> fixture_video;
    rt::Video *render_video=&video;
    if(argc>=16 && (std::string(argv[15])=="material-fixture" || std::string(argv[15])=="memory-fixture")) {
        const bool memory_fixture=std::string(argv[15])=="memory-fixture";
        const auto scene=video.presentation_scene();
        rt::GeoPoly p{};
        if(memory_fixture) {
            unsigned h0,h1,h2,h3;
            if(argc<18 || std::sscanf(argv[16],"%x,%x,%x,%x",&h0,&h1,&h2,&h3)!=4 || h0>65535 || h1>65535 || h2>65535 || h3>65535 || ((h0>>13)&3)<2) {
                std::fprintf(stderr,"Memory fixture requires native header and exact source hash\n");return 2;
            }
            p.texheader[0]=uint16_t(h0);p.texheader[1]=uint16_t(h1);p.texheader[2]=uint16_t(h2);p.texheader[3]=uint16_t(h3);p.luma=255;
            const int w=32<<(h0&7),h=32<<((h0>>3)&7),bx=32*(h2&63),by=32*((h2>>6)&31);
            const auto *sheet=(h2&4096)?scene.memory.tex1:scene.memory.tex0;
            uint64_t hash=14695981039346656037ULL;
            for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
                int xx=bx+x,yy=by+y;if(xx>=1024){xx-=1024;yy^=1024;}
                const unsigned address=(yy/2)*512+xx/2;unsigned tex=sheet[(address>>1)&0x7ffff];
                if(address&1)tex>>=16;if(!(y&1))tex>>=8;if(!(x&1))tex>>=4;hash=(hash^(tex&15))*1099511628211ULL;
            }
            if(hash!=std::strtoull(argv[17],nullptr,16)){std::fprintf(stderr,"Memory fixture native content mismatch\n");return 2;}
            std::printf("MEMORY SOURCE FIXTURE; header not required to be used by game geometry; no gameplay visibility claim; hash=%016llx\n",static_cast<unsigned long long>(hash));
        } else {
            if(!find_key || !find_source || !found_key) {
                std::fprintf(stderr,"Material fixture requires a matched key and source hash\n");return 2;
            }
            auto selected=std::find_if(scene.polys.begin(),scene.polys.end(),[&](const rt::GeoPoly &poly) {
                uint64_t k=0;for(unsigned i=0;i<4;i++)k|=uint64_t(poly.texheader[i])<<(16*i);
                return (k&0x00001fff0000003fULL)==find_key;
            });
            if(selected==scene.polys.end())return 2;
            p=*selected;
        }
        p.num_vertices=4;p.window=0;p.reverse=0;p.z=1;p.texlod=0;
        p.viewport[0]=-scene.crtc_x;p.viewport[1]=scene.crtc_y+1;
        p.viewport[2]=495-scene.crtc_x;p.viewport[3]=384+scene.crtc_y;
        const float tw=float(32u<<(p.texheader[0]&7)),th=float(32u<<((p.texheader[0]>>3)&7));
        // Tool-only mirror stress: shift into the adjacent mirrored UV tile.
        const int mirror_arg=memory_fixture?18:16;
        const int fixture_mirror=argc>mirror_arg?std::clamp(std::atoi(argv[mirror_arg]),0,3):0;
        if(fixture_mirror&1)p.texheader[0]|=0x100;
        if(fixture_mirror&2)p.texheader[0]|=0x200;
        const float fixture_h=std::min(304.0f,400.0f*th/tw),fixture_w=fixture_h*tw/th;
        const float left=(496-fixture_w)/2,top=40;
        const float sx[4]={left,left+fixture_w,left+fixture_w,left};
        const float sy[4]={top,top,top+fixture_h,top+fixture_h};
        const float u[4]={0,tw,tw,0},v[4]={0,0,th,th};
        for(int i=0;i<4;i++) {
            p.v[i].x=sx[i]-scene.crtc_x-p.center[0];
            p.v[i].y=384-p.center[1]+scene.crtc_y-sy[i];
            p.v[i].p[0]=1;p.v[i].p[1]=(u[i]+((fixture_mirror&1)?tw:0))*8;p.v[i].p[2]=(v[i]+((fixture_mirror&2)?th:0))*8;
        }
        const rt::VideoMem memory=scene.memory;
        fixture_tiles.resize(0x10000);fixture_chars.resize(0x80000);
        fixture_video=std::make_unique<rt::Video>(fixture_tiles.data(),fixture_chars.data());
        fixture_video->enable_presentation(true,true);fixture_video->set_widescreen(wide);
        fixture_video->set_texture_quality(quality);fixture_video->set_depth_buffer(depth);
        fixture_video->xhout_w(uint16_t(scene.crtc_x-84));fixture_video->xvout_w(uint16_t(scene.crtc_y-130));
        fixture_video->screen_update(std::vector<rt::GeoPoly>{p},0,memory);
        render_video=fixture_video.get();
        std::printf("CONTROLLED MATERIAL FIXTURE; not gameplay visibility proof. UV=%gx%g screen=%g,%g,%g,%g header=%04x,%04x,%04x,%04x luma=%u\n",tw,th,left,top,fixture_w,fixture_h,p.texheader[0],p.texheader[1],p.texheader[2],p.texheader[3],unsigned(p.luma));
    }
    SDL_GPUTextureCreateInfo ti{};ti.type=SDL_GPU_TEXTURETYPE_2D;ti.format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER|SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    ti.width=width;ti.height=height;ti.layer_count_or_depth=1;ti.num_levels=1;
    auto *texture=SDL_CreateGPUTexture(dev,&ti);
    SDL_GPUTransferBufferCreateInfo di{};di.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;di.size=width*height*4;
    auto *download=SDL_CreateGPUTransferBuffer(dev,&di);
    if(!texture||!download){std::fprintf(stderr,"%s\n",SDL_GetError());return 2;}
    bool success=false;
    {
      std::vector<uint8_t> private_shader;
      const char *private_shader_path = nullptr;
      if(argc>=17 && std::string(argv[15])=="experimental-shader") private_shader_path=argv[16];
      if(argc>=18 && std::string(argv[15])=="material-fixture") private_shader_path=argv[17];
      if(argc>=20 && std::string(argv[15])=="memory-fixture") private_shader_path=argv[19];
      if(private_shader_path) {
          std::ifstream shader(private_shader_path,std::ios::binary);
          if(!shader){std::fprintf(stderr,"Private shader file missing\n");return 2;}
          private_shader.assign(std::istreambuf_iterator<char>(shader),std::istreambuf_iterator<char>());
          if(private_shader.empty()){std::fprintf(stderr,"Private shader file empty\n");return 2;}
      }
      app::GpuGeometry renderer(dev,private_shader);
      if(!renderer.available())return 2;
      std::vector<double> times;
      for(int i=0;i<12;i++) {
        auto begin=std::chrono::steady_clock::now();
        auto *cmd=SDL_AcquireGPUCommandBuffer(dev);
        // Private diagnostic bits: 1=mask colors, 2=native-tone/detail experiment.
        const int repaint_test=argc>=15?std::atoi(argv[14]):0;
        if(!renderer.render(cmd,texture,*render_video,width,height,depth,quality,(repaint_test&1)!=0,(repaint_test&2)!=0))return 2;
        if(i==11){
          auto *copy=SDL_BeginGPUCopyPass(cmd);SDL_GPUTextureRegion source{};
          source.texture=texture;source.w=width;source.h=height;source.d=1;
          SDL_GPUTextureTransferInfo dest{};dest.transfer_buffer=download;
          SDL_DownloadFromGPUTexture(copy,&source,&dest);SDL_EndGPUCopyPass(copy);
        }
        auto *fence=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if(!fence||!SDL_WaitForGPUFences(dev,true,&fence,1)){std::fprintf(stderr,"%s\n",SDL_GetError());return 2;}
        SDL_ReleaseGPUFence(dev,fence);
        if(i>1)times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
      }
      std::sort(times.begin(),times.end());
      auto begin=std::chrono::steady_clock::now();
      const auto &cpu=render_video->present(width,height,1,false);
      double cpu_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
      const auto *gpu=static_cast<const uint32_t *>(SDL_MapGPUTransferBuffer(dev,download,false));
      if(!gpu)return 2;
      size_t different=0,large=0;uint64_t total=0;
      std::ofstream out(std::string(argv[4])+"-gpu.rgba",std::ios::binary),ref(std::string(argv[4])+"-cpu.bgra",std::ios::binary);
      out.write(reinterpret_cast<const char *>(gpu),width*height*4);ref.write(reinterpret_cast<const char *>(cpu.data()),width*height*4);
      for(size_t i=0;i<cpu.size();i++) {
        uint32_t g=gpu[i];g=(g&0xff00ff00)|((g&255)<<16)|((g>>16)&255);
        if(g!=cpu[i])different++;
        int maximum=0;
        for(int shift:{0,8,16}){int error=std::abs(int((g>>shift)&255)-int((cpu[i]>>shift)&255));total+=error;maximum=std::max(maximum,error);}
        if(maximum>16)large++;
      }
      std::printf("frames=%d size=%dx%d pixels=%zu different=%zu large=%zu mean_error=%.6f GPU_setup_upload_execute_median_ms=%.3f CPU_render_ms=%.3f\n",
          frames,width,height,cpu.size(),different,large,double(total)/(cpu.size()*3),times[times.size()/2],cpu_ms);
      SDL_UnmapGPUTransferBuffer(dev,download);
      std::printf("AI replacement spans=%zu\n",renderer.replacement_spans());
      std::printf("Replacements loaded=%zu quality=%d\n",renderer.replacement_count(),quality);
      success=quality==300 ? renderer.replacement_spans()>0 && large>0 : large<cpu.size()/100;
    }
    SDL_ReleaseGPUTransferBuffer(dev,download);SDL_ReleaseGPUTexture(dev,texture);
    SDL_DestroyGPUDevice(dev);SDL_Quit();return success?0:1;
}





