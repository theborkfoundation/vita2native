#define GL_SILENCE_DEPRECATION
#include <SDL.h>
#include "nanovg.h"
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#define NANOVG_GL3
#else
#include <GLES2/gl2.h>
#define NANOVG_GLES2
#endif
#include "nanovg_gl.h"
#include "inter_font.h"
#include "inter_bold.h"
#include "cover_0.h"
#include "cover_1.h"
#include "cover_2.h"
#include "cover_3.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <tuple>
#include <vector>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
static NVGcontext *vg;
static int artwork[4] = {};
struct Item { fs::path path; bool dir; };
struct App {
    bool running=true, picker=false; int screen=0, focus=0, selected=0, pick_index=0, scroll=0, pick_kind=0;
    int home_focus=0, previous_home_focus=0, all_focus=0, previous=0, direction=0, queued=0;
    Uint32 motion_start=0, focus_start=0, screen_start=SDL_GetTicks()+1600;
    static constexpr Uint32 motion_duration=300, entrance_duration=600;
    bool moving=false;
    void change_screen(int next) { screen=next; focus=0; message.clear(); screen_start=SDL_GetTicks(); }
    void set_home_focus(int next) { if(home_focus!=next){previous_home_focus=home_focus;home_focus=next;focus_start=SDL_GetTicks();} }
    void move_game(int step) {
        if(moving){ queued=std::clamp(queued+step,-8,8); return; }
        previous=selected; selected=(selected+step+4)%4; direction=step;
        motion_start=SDL_GetTicks(); moving=true; message.clear();
    }
    void update_motion() {
        if(moving && SDL_GetTicks()-motion_start>=motion_duration){
            moving=false;
            if(queued){int step=queued>0?1:-1;queued-=step;move_game(step);}
        }
    }
    bool animating() const {
        Uint32 now=SDL_GetTicks();
        return moving || (now-start>=1600 && now-screen_start<entrance_duration) || (focus_start && now-focus_start<220);
    }
    fs::path pkg,license,dir=fs::current_path(),stage; std::vector<Item> items; std::string message; pid_t child=-1; Uint32 start=SDL_GetTicks();
    SDL_GameController *controller=nullptr; SDL_Joystick *joystick=nullptr; int axis_x=0,axis_y=0; Uint8 hat=SDL_HAT_CENTERED;
    ~App(){ if(controller) SDL_GameControllerClose(controller); if(joystick) SDL_JoystickClose(joystick); }
    void open_pad(int idx){ if(controller||joystick) return; if(SDL_IsGameController(idx)) controller=SDL_GameControllerOpen(idx); else joystick=SDL_JoystickOpen(idx); }
    void scan(){ items.clear(); std::error_code ec; if(dir.has_parent_path()&&dir.parent_path()!=dir) items.push_back({dir.parent_path(),true});
        std::vector<Item> found; fs::directory_iterator it(dir,fs::directory_options::skip_permission_denied,ec),end;
        while(!ec&&it!=end){ auto p=it->path(); std::error_code e; bool d=it->is_directory(e); bool file=it->is_regular_file(e); std::string ext=p.extension().string(); std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return (char)std::tolower(c);}); if(d||(file&&(pick_kind==0?ext==".pkg":p.filename()=="work.bin"))) found.push_back({p,d}); it.increment(ec); }
        std::sort(found.begin(),found.end(),[](const Item&a,const Item&b){return a.dir!=b.dir?a.dir>b.dir:a.path.filename().string()<b.path.filename().string();}); items.insert(items.end(),found.begin(),found.end()); pick_index=scroll=0; if(ec) message="Could not read this directory.";
    }
    void browse(int kind){ pick_kind=kind; picker=true; message.clear(); screen_start=SDL_GetTicks(); scan(); }
    void start_import(){ if(child>0) return; if(pkg.empty()||license.empty()){message="Select a package and work.bin first."; return;} std::string temp=(fs::current_path()/".v2n-import-XXXXXX").string(); std::vector<char> buf(temp.begin(),temp.end());buf.push_back(0); char *p=mkdtemp(buf.data()); if(!p){message="Could not create import directory.";return;} stage=p;
        std::string executable; const char *env=getenv("V2N_EXTRACT"); if(env&&*env) executable=env; else {char *base=SDL_GetBasePath(); if(base){fs::path sibling=fs::path(base)/"v2n-extract"; SDL_free(base); if(access(sibling.c_str(),X_OK)==0) executable=sibling.string();} if(executable.empty()) executable="v2n-extract";}
        child=fork(); if(child==0){int fd=open((stage/"import.log").c_str(),O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd>=0){dup2(fd,STDOUT_FILENO);dup2(fd,STDERR_FILENO);close(fd);} execlp(executable.c_str(),executable.c_str(),"--pkg",pkg.c_str(),"--work-bin",license.c_str(),"--out",stage.c_str(),(char*)nullptr); _exit(127);}
        if(child<0){child=-1;std::error_code ec;fs::remove_all(stage,ec);message="Could not start extractor.";}else message="Importing... Please wait.";
    }
    void poll(){ if(child<=0)return; int status=0; pid_t result=waitpid(child,&status,WNOHANG); if(result==0)return; child=-1; std::error_code ec;
        if(result<0||!WIFEXITED(status)||WEXITSTATUS(status)!=0){message=(result>0&&WIFEXITED(status)&&WEXITSTATUS(status)==127)?"Extractor not found. Set V2N_EXTRACT.":"Import failed. Log: "+stage.filename().string()+"/import.log";return;}
        std::vector<fs::path> outputs; fs::directory_iterator it(stage,ec),end; while(!ec&&it!=end){if(it->is_directory(ec)&&it->path().filename().string().rfind(".v2n-temp-",0)!=0)outputs.push_back(it->path());it.increment(ec);} if(ec||outputs.size()!=1){message="Import output invalid. See staging folder.";return;}
        fs::path dest=fs::current_path()/outputs[0].filename(); if(fs::exists(dest,ec)||ec){message="Title exists. Saved in "+stage.filename().string();return;} fs::rename(outputs[0],dest,ec);if(ec){message="Could not move title. See staging folder.";return;}fs::remove_all(stage,ec);message="Import complete: "+dest.filename().string();pkg.clear();license.clear();
    }
    void action(int a){ // up, down, left, right, select, back
        if(child>0){if(a==5)message="Import is running; please wait.";return;}
        if(picker){
            if(a==5){picker=false;screen_start=SDL_GetTicks();return;}
            if(a==0&&!items.empty())pick_index=std::max(0,pick_index-1);
            if(a==1&&!items.empty())pick_index=std::min((int)items.size()-1,pick_index+1);
            if(a==4&&!items.empty()){auto item=items[pick_index];if(item.dir){dir=item.path;scan();}else{(pick_kind==0?pkg:license)=item.path;picker=false;screen_start=SDL_GetTicks();message.clear();}}
            if(pick_index<scroll)scroll=pick_index;if(pick_index>=scroll+8)scroll=pick_index-7;return;
        }
        if(a==5){
            if(screen!=0){int from=screen;change_screen(0);home_focus=from==2?1:2;previous_home_focus=home_focus;focus_start=0;}
            else running=false;
            return;
        }
        if(screen==0){
            if(a==0&&home_focus!=0)set_home_focus(0);
            else if(a==1&&home_focus==0)set_home_focus(1);
            else if(home_focus==0&&(a==2||a==3))move_game(a==2?-1:1);
            else if(home_focus!=0&&a==2)set_home_focus(1);
            else if(home_focus!=0&&a==3)set_home_focus(2);
            else if(a==4){
                if(home_focus==1){moving=false;queued=0;all_focus=selected;change_screen(2);}
                else if(home_focus==2){moving=false;queued=0;change_screen(1);}
                else message="Games are sample entries for now.";
            }
        }else if(screen==2){
            if(a==2&&all_focus%2==1)all_focus--;
            else if(a==3&&all_focus%2==0)all_focus++;
            else if(a==0&&all_focus>=2)all_focus-=2;
            else if(a==1&&all_focus<2)all_focus+=2;
            else if(a==4){selected=all_focus;message="Games are sample entries for now.";}
        }else{
            if(a==0)focus=std::max(0,focus-1);if(a==1)focus=std::min(2,focus+1);
            if(a==4){if(focus<2)browse(focus);else start_import();}
        }
    }
};
static NVGcolor rgba(int r,int g,int b,int a=255){return nvgRGBA(r,g,b,a);}
static void rect(float x,float y,float w,float h,NVGcolor c,float radius=0){nvgBeginPath(vg);nvgRoundedRect(vg,x,y,w,h,radius);nvgFillColor(vg,c);nvgFill(vg);}
static void outline(float x,float y,float w,float h,float radius,NVGcolor c,float width=1){nvgBeginPath(vg);nvgRoundedRect(vg,x,y,w,h,radius);nvgStrokeColor(vg,c);nvgStrokeWidth(vg,width);nvgStroke(vg);}
static void gradient(float x,float y,float w,float h,NVGcolor a,NVGcolor b){nvgBeginPath(vg);nvgRect(vg,x,y,w,h);nvgFillPaint(vg,nvgLinearGradient(vg,x,y,x,y+h,a,b));nvgFill(vg);}
static void label(float x,float y,const std::string &s,float size,NVGcolor c,bool bold=false,float maxw=590){nvgSave(vg);nvgIntersectScissor(vg,x,y-3,maxw,size+8);nvgFontFace(vg,bold?"inter-bold":"inter");nvgFontSize(vg,size);nvgTextAlign(vg,NVG_ALIGN_LEFT|NVG_ALIGN_TOP);nvgFillColor(vg,c);nvgText(vg,x,y,s.c_str(),nullptr);nvgRestore(vg);}
static void centered(float x,float y,const char *s,float size,NVGcolor c,bool bold=false){nvgFontFace(vg,bold?"inter-bold":"inter");nvgFontSize(vg,size);nvgTextAlign(vg,NVG_ALIGN_CENTER|NVG_ALIGN_MIDDLE);nvgFillColor(vg,c);nvgText(vg,x,y,s,nullptr);}
static void brand(float x,float y,float size){outline(x,y,size,size,5,rgba(238,246,250,220),1.2);nvgBeginPath(vg);nvgMoveTo(vg,x+size*.20f,y+size*.27f);nvgLineTo(vg,x+size*.38f,y+size*.73f);nvgLineTo(vg,x+size*.50f,y+size*.45f);nvgLineTo(vg,x+size*.62f,y+size*.73f);nvgLineTo(vg,x+size*.80f,y+size*.27f);nvgStrokeWidth(vg,size*.07f);nvgStrokeColor(vg,rgba(255,255,255));nvgStroke(vg);}
static const char *game_names[]={"Horizon Drift","Moonlit Valley","Neon Circuit","Little Voyager"};
static void image_cover(int image,float x,float y,float w,float h,float radius,float alpha=1){
    if(!image){rect(x,y,w,h,rgba(51,68,76),radius);return;}
    int iw=0,ih=0;nvgImageSize(vg,image,&iw,&ih);if(iw<=0||ih<=0)return;
    float scale=std::max(w/iw,h/ih),dw=iw*scale,dh=ih*scale;
    nvgBeginPath(vg);nvgRoundedRect(vg,x,y,w,h,radius);
    nvgFillPaint(vg,nvgImagePattern(vg,x+(w-dw)/2,y+(h-dh)/2,dw,dh,0,image,alpha));nvgFill(vg);
}
static float ease_out(float t){t=std::clamp(t,0.0f,1.0f);return 1.0f-(1.0f-t)*(1.0f-t)*(1.0f-t);}
static float entrance(const App &a,int delay=0){return ease_out(((float)(SDL_GetTicks()-a.screen_start)-delay)/360.0f);}
// Inter's capital letters sit above the midpoint of its line box; this offset
// centers their visible height on the button circles, not the font's line box.
static constexpr float footer_center_y=459.0f;
static constexpr float footer_label_y=footer_center_y-4.25f;
static float footer_text_width(const char *text){
    nvgFontFace(vg,"inter-bold");nvgFontSize(vg,10);nvgTextAlign(vg,NVG_ALIGN_LEFT|NVG_ALIGN_TOP);
    return nvgTextBounds(vg,0,0,text,nullptr,nullptr);
}
static void footer_key(float cx,char key,const char *meaning){
    constexpr float cy=footer_center_y;
    nvgBeginPath(vg);nvgCircle(vg,cx,cy,11);nvgFillColor(vg,rgba(214,232,237));nvgFill(vg);
    // Draw both glyphs on the same centered, nine-pixel-high grid.
    nvgBeginPath(vg);
    if(key=='A'){
        nvgMoveTo(vg,cx-3.8f,cy+4.5f);nvgLineTo(vg,cx,cy-4.5f);nvgLineTo(vg,cx+3.8f,cy+4.5f);
        nvgMoveTo(vg,cx-2.3f,cy+1.1f);nvgLineTo(vg,cx+2.3f,cy+1.1f);
    }else{
        nvgMoveTo(vg,cx-3.2f,cy-4.5f);nvgLineTo(vg,cx-3.2f,cy+4.5f);nvgLineTo(vg,cx+0.5f,cy+4.5f);
        nvgBezierTo(vg,cx+4.5f,cy+4.5f,cx+4.5f,cy,cx+0.5f,cy);
        nvgLineTo(vg,cx-3.2f,cy);nvgMoveTo(vg,cx-3.2f,cy-4.5f);nvgLineTo(vg,cx+0.5f,cy-4.5f);
        nvgBezierTo(vg,cx+4.1f,cy-4.5f,cx+4.1f,cy,cx+0.5f,cy);
    }
    nvgStrokeColor(vg,rgba(18,35,44));nvgStrokeWidth(vg,1.5f);nvgLineCap(vg,NVG_ROUND);nvgLineJoin(vg,NVG_ROUND);nvgStroke(vg);
    label(cx+19,footer_label_y,meaning,10,rgba(191,210,217),true);
}
static void footer_dpad(float cx,float cy){
    nvgBeginPath(vg);nvgCircle(vg,cx,cy,11);nvgFillColor(vg,rgba(214,232,237));nvgFill(vg);
    // Controller D-pad silhouette, with separate direction markers on each arm.
    nvgBeginPath(vg);nvgMoveTo(vg,cx-2.5f,cy-8);nvgLineTo(vg,cx+2.5f,cy-8);
    nvgLineTo(vg,cx+2.5f,cy-2.5f);nvgLineTo(vg,cx+8,cy-2.5f);nvgLineTo(vg,cx+8,cy+2.5f);
    nvgLineTo(vg,cx+2.5f,cy+2.5f);nvgLineTo(vg,cx+2.5f,cy+8);nvgLineTo(vg,cx-2.5f,cy+8);
    nvgLineTo(vg,cx-2.5f,cy+2.5f);nvgLineTo(vg,cx-8,cy+2.5f);nvgLineTo(vg,cx-8,cy-2.5f);
    nvgLineTo(vg,cx-2.5f,cy-2.5f);nvgClosePath(vg);nvgFillColor(vg,rgba(18,35,44));nvgFill(vg);
    nvgBeginPath(vg);nvgCircle(vg,cx,cy,1.2f);nvgFillColor(vg,rgba(214,232,237));nvgFill(vg);
}
static void header(const App &a){
    brand(25,17,26);label(60,19,"V2N",16,rgba(244,248,250),true);label(60,37,"LAUNCHER",8,rgba(157,177,188),true);
    if(a.screen!=0){
        const char *name=a.picker?"CHOOSE FILE":a.screen==2?"ALL GAMES":"IMPORT GAME";
        nvgFontFace(vg,"inter-bold");nvgFontSize(vg,11);nvgTextAlign(vg,NVG_ALIGN_LEFT|NVG_ALIGN_TOP);
        float width=nvgTextBounds(vg,0,0,name,nullptr,nullptr)+30;
        rect(615-width,18,width,27,rgba(135,174,185,35),13.5f);outline(615-width,18,width,27,13.5f,rgba(162,197,205,75));
        centered(615-width/2,31.5f,name,11,rgba(229,243,245),true);
    }
}
static void footer(const App &a){
    rect(0,438,640,42,rgba(12,26,35,250));rect(0,438,640,1,rgba(159,190,201,40));
    footer_dpad(35,footer_center_y);label(54,footer_label_y,"NAVIGATE",10,rgba(191,210,217),true);
    const char *back=a.picker||a.screen!=0?"BACK":"EXIT";
    const float back_cx=615-footer_text_width(back)-19;
    const float select_cx=back_cx-11-22-footer_text_width("SELECT")-19;
    footer_key(select_cx,'A',"SELECT");footer_key(back_cx,'B',back);
}
static void splash(Uint32 elapsed){
    float appear=ease_out(elapsed/600.0f);
    rect(0,0,640,480,rgba(10,19,27));gradient(0,0,640,480,rgba(43,64,73),rgba(7,13,20));
    nvgSave(vg);nvgGlobalAlpha(vg,appear);nvgTranslate(vg,0,12*(1-appear));brand(284,170,72);nvgRestore(vg);
    rect(240,282,160,3,rgba(255,255,255,35),1.5);
    rect(240,282,160*ease_out(elapsed/1600.f),3,rgba(229,240,242),1.5);
}
static void cover_card(int id,float x,float active,bool focused,float entry){
    constexpr float width=310, y=107, h=154;
    nvgSave(vg);nvgGlobalAlpha(vg,entry);nvgTranslate(vg,0,12*(1-entry));
    image_cover(artwork[id],x,y,width,h,5);nvgSave(vg);nvgIntersectScissor(vg,x,y,width,h);gradient(x,y+h-75,width,75,rgba(8,15,21,0),rgba(8,15,21,220));nvgRestore(vg);
    label(x+16,y+h-32,game_names[id],19,rgba(255,255,255),true,width-32);
    NVGcolor edge=focused?rgba(248,252,255,(int)(55+200*active)):rgba(255,255,255,(int)(45+55*active));outline(x,y,width,h,5,edge,1+active);nvgRestore(vg);
}
static void draw_games(const App &a){
    float heading=entrance(a);nvgSave(vg);nvgGlobalAlpha(vg,heading);nvgTranslate(vg,0,8*(1-heading));label(25,72,"Recent games",20,rgba(248,251,253),true);nvgRestore(vg);
    nvgSave(vg);nvgScissor(vg,0,103,640,165);
    float t=a.moving?ease_out((SDL_GetTicks()-a.motion_start)/(float)App::motion_duration):1.0f;
    int first;float first_weight,second_weight,x;
    constexpr float pitch=320;
    if(!a.moving){first=a.selected;first_weight=1;second_weight=0;x=25;}
    else if(a.direction>0){first=a.previous;first_weight=1-t;second_weight=t;x=25-pitch*t;}
    else{first=a.selected;first_weight=t;second_weight=1-t;x=25-pitch*(1-t);}
    for(int i=0;i<4;i++){
        float active=i==0?first_weight:i==1?second_weight:0;
        cover_card((first+i)%4,x,active,a.home_focus==0,entrance(a,65+i*50));x+=pitch;
    }
    nvgRestore(vg);
    float fp=a.focus_start?ease_out((SDL_GetTicks()-a.focus_start)/220.0f):1.0f;
    for(int i=0;i<2;i++){
        float show=entrance(a,240+i*80);nvgSave(vg);nvgGlobalAlpha(vg,show);nvgTranslate(vg,0,12*(1-show));
        int target=i+1;float focused=(a.home_focus==target?fp:0)+(a.previous_home_focus==target?1-fp:0);
        float x=25+i*301;
        rect(x,321,289,92,rgba(32,52,64,225),7);
        outline(x,321,289,92,7,rgba(187+(int)(52*focused),211+(int)(37*focused),219+(int)(30*focused),60+(int)(195*focused)),1+focused);
        rect(x+15,347,40,40,rgba(204,230,234,24),7);
        if(i==0){for(int row=0;row<2;row++)for(int col=0;col<2;col++)rect(x+25+col*11,357+row*11,7,7,rgba(213,232,235),1);}
        else{nvgBeginPath(vg);nvgMoveTo(vg,x+35,357);nvgLineTo(vg,x+35,377);nvgMoveTo(vg,x+25,367);nvgLineTo(vg,x+45,367);nvgStrokeColor(vg,rgba(213,232,235));nvgStrokeWidth(vg,2);nvgStroke(vg);}
        label(x+68,343,i==0?"View all games":"Import game",15,rgba(245,250,251),true,190);
        label(x+68,369,i==0?"Browse your library":"Add a package and license",10,rgba(164,188,196),false,203);
        label(x+258,346,"→",16,rgba(195,218,223),true,24);nvgRestore(vg);
    }
    if(!a.message.empty()){rect(25,282,430,23,rgba(16,27,34,245),4);label(33,288,a.message,10,rgba(232,239,241),false,410);}
}
static void draw_all_games(const App &a){
    float heading=entrance(a);nvgSave(vg);nvgGlobalAlpha(vg,heading);nvgTranslate(vg,0,8*(1-heading));
    label(25,72,"All games",20,rgba(248,251,253),true);label(525,79,"4 GAMES",10,rgba(174,192,200),true);nvgRestore(vg);
    for(int i=0;i<4;i++){
        float show=entrance(a,60+i*60);float x=25+(i%2)*301,y=111+(i/2)*150;
        nvgSave(vg);nvgGlobalAlpha(vg,show);nvgTranslate(vg,0,10*(1-show));
        image_cover(artwork[i],x,y,289,136,5);
        nvgSave(vg);nvgIntersectScissor(vg,x,y,289,136);gradient(x,y+65,289,71,rgba(8,15,21,0),rgba(8,15,21,225));nvgRestore(vg);
        label(x+14,y+104,game_names[i],16,rgba(255,255,255),true,260);
        outline(x,y,289,136,5,a.all_focus==i?rgba(248,252,255):rgba(255,255,255,60),a.all_focus==i?2:1);
        nvgRestore(vg);
    }
    if(!a.message.empty()){rect(25,411,480,21,rgba(16,27,34,245),4);label(33,416,a.message,10,rgba(232,239,241),false,460);}
}
static void draw_import(const App &a){
    float heading=entrance(a);nvgSave(vg);nvgGlobalAlpha(vg,heading);nvgTranslate(vg,0,10*(1-heading));
    label(25,72,"Import a game",20,rgba(248,251,253),true);label(25,99,"Select your package and its license to add a game.",12,rgba(174,193,201));nvgRestore(vg);
    for(int i=0;i<2;i++){float show=entrance(a,80+i*90);nvgSave(vg);nvgGlobalAlpha(vg,show);nvgTranslate(vg,0,12*(1-show));float y=135+i*76;rect(25,y,590,64,rgba(24,40,51,225),5);outline(25,y,590,64,5,a.focus==i?rgba(241,248,250):rgba(119,150,162,90),a.focus==i?2:1);rect(39,y+14,36,36,rgba(255,255,255,20),4);centered(57,y+32,i==0?"P":"L",15,rgba(226,238,240),true);label(89,y+11,i==0?"Game package":"License file",13,rgba(244,249,251),true);std::string name=(i==0?a.pkg:a.license).filename().string();label(89,y+34,name.empty()?(i==0?"Choose a .pkg file":"Choose a work.bin file"):name,11,rgba(173,194,202),false,350);rect(534,y+17,64,30,rgba(255,255,255,24),4);centered(566,y+32,"BROWSE",10,rgba(240,247,249),true);nvgRestore(vg);}
    float details=entrance(a,260);nvgSave(vg);nvgGlobalAlpha(vg,details);nvgTranslate(vg,0,10*(1-details));
    label(26,297,"IMPORT LOCATION",10,rgba(155,179,188),true);label(26,315,fs::current_path().string(),11,rgba(224,235,237),false,395);
    rect(453,304,162,40,a.focus==2?rgba(225,237,239):rgba(92,115,124),5);centered(534,324,a.child>0?"IMPORTING...":"IMPORT GAME",11,a.focus==2?rgba(14,28,35):rgba(225,236,239),true);
    rect(25,365,590,47,rgba(255,255,255,12),5);label(39,373,a.message.empty()?"Your files will be prepared for vita2native.":a.message,11,rgba(199,218,224),false,560);if(a.message.empty())label(39,391,"Your title is saved in the current directory.",10,rgba(143,168,178));nvgRestore(vg);
}
static void draw_picker(const App &a){
    float heading=entrance(a);nvgSave(vg);nvgGlobalAlpha(vg,heading);nvgTranslate(vg,0,8*(1-heading));label(25,72,a.pick_kind==0?"Choose a package":"Choose work.bin",20,rgba(248,251,253),true);label(25,100,a.dir.string(),11,rgba(174,193,201),false,590);nvgRestore(vg);
    if(a.items.empty())label(36,156,"No matching files or folders.",12,rgba(168,189,197));
    for(int row=0;row<8&&a.scroll+row<(int)a.items.size();row++){int idx=a.scroll+row;float show=entrance(a,50+row*35);nvgSave(vg);nvgGlobalAlpha(vg,show);nvgTranslate(vg,0,8*(1-show));float y=127+row*37;const auto &it=a.items[idx];if(idx==a.pick_index){rect(25,y,590,33,rgba(255,255,255,24),4);outline(25,y,590,33,4,rgba(239,247,250),1.5);}label(38,y+9,it.dir?"DIR":"FILE",10,rgba(159,188,197),true);std::string name=it.path==a.dir.parent_path()?"..":it.path.filename().string();label(90,y+8,name,12,rgba(238,247,249),false,505);nvgRestore(vg);}
}
static void draw(App &a){
    Uint32 elapsed=SDL_GetTicks()-a.start;if(elapsed<1600){splash(elapsed);return;}
    if(a.moving){float t=ease_out((SDL_GetTicks()-a.motion_start)/(float)App::motion_duration);image_cover(artwork[a.previous],0,0,640,440,0,.48f);image_cover(artwork[a.selected],0,0,640,440,0,.48f*t);}
    else image_cover(artwork[a.selected],0,0,640,440,0,.48f);
    rect(0,0,640,440,rgba(12,25,35,190));gradient(0,0,640,440,rgba(30,43,52,75),rgba(10,22,30,240));header(a);
    if(a.picker)draw_picker(a);else if(a.screen==0)draw_games(a);else if(a.screen==2)draw_all_games(a);else draw_import(a);footer(a);
}
int main(int argc,char **argv){
    (void)argc;(void)argv; if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER|SDL_INIT_JOYSTICK|SDL_INIT_TIMER)<0){fprintf(stderr,"SDL: %s\n",SDL_GetError());return 1;}
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,2);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE,8);
    SDL_Window *window=SDL_CreateWindow("V2N Launcher",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,640,480,SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN|SDL_WINDOW_ALLOW_HIGHDPI);
    if(!window){fprintf(stderr,"SDL window: %s\n",SDL_GetError());SDL_Quit();return 1;}
    SDL_GLContext context=SDL_GL_CreateContext(window);if(!context){fprintf(stderr,"SDL GL: %s\n",SDL_GetError());SDL_DestroyWindow(window);SDL_Quit();return 1;}
    SDL_GL_SetSwapInterval(1);
#ifdef __APPLE__
    vg=nvgCreateGL3(NVG_ANTIALIAS|NVG_STENCIL_STROKES);
#else
    vg=nvgCreateGLES2(NVG_ANTIALIAS|NVG_STENCIL_STROKES);
#endif
    if(!vg||nvgCreateFontMem(vg,"inter",inter_font,sizeof(inter_font),0)<0||nvgCreateFontMem(vg,"inter-bold",inter_bold,sizeof(inter_bold),0)<0){fprintf(stderr,"Failed to initialize NanoVG or font\n");return 1;}
    unsigned char *covers[]={cover_0,cover_1,cover_2,cover_3};int sizes[]={sizeof(cover_0),sizeof(cover_1),sizeof(cover_2),sizeof(cover_3)};
    for(int i=0;i<4;i++){artwork[i]=nvgCreateImageMem(vg,0,covers[i],sizes[i]);if(!artwork[i])fprintf(stderr,"Could not load cover %d\n",i);}
    {App app; const char *capture=getenv("V2N_LAUNCHER_SCREENSHOT");
    if(capture){app.start=SDL_GetTicks()-2000;const char *screen=getenv("V2N_LAUNCHER_SCREEN");if(screen&&strcmp(screen,"import")==0)app.screen=1;if(screen&&strcmp(screen,"all")==0)app.screen=2;if(screen&&strcmp(screen,"home-focus")==0){app.home_focus=1;app.previous_home_focus=1;}if(screen&&strcmp(screen,"picker")==0){app.screen=1;app.browse(0);}app.screen_start=SDL_GetTicks()-1000;
        if(screen&&strcmp(screen,"motion-right")==0){app.move_game(1);app.motion_start=SDL_GetTicks()-100;}
        if(screen&&strcmp(screen,"motion-left")==0){app.move_game(-1);app.motion_start=SDL_GetTicks()-100;}
    }
    for(int i=0;i<SDL_NumJoysticks();i++)app.open_pad(i);
    bool dirty=true, was_splash=true, was_animating=false;
    while(app.running){
    auto before=std::make_tuple(app.screen,app.home_focus,app.all_focus,app.focus,app.selected,app.picker,app.pick_index,app.scroll,app.dir,app.pkg,app.license,app.message,app.child);
    SDL_Event e;while(SDL_PollEvent(&e)){
        if(e.type==SDL_WINDOWEVENT&&(e.window.event==SDL_WINDOWEVENT_EXPOSED||e.window.event==SDL_WINDOWEVENT_SIZE_CHANGED||e.window.event==SDL_WINDOWEVENT_SHOWN))dirty=true;if(e.type==SDL_QUIT){if(app.child<=0)app.running=false;}
        else if(e.type==SDL_CONTROLLERDEVICEADDED||e.type==SDL_JOYDEVICEADDED)app.open_pad(e.type==SDL_CONTROLLERDEVICEADDED?e.cdevice.which:e.jdevice.which);
        else if(e.type==SDL_CONTROLLERDEVICEREMOVED&&app.controller&&!SDL_GameControllerGetAttached(app.controller)){SDL_GameControllerClose(app.controller);app.controller=nullptr;}
        else if(e.type==SDL_JOYDEVICEREMOVED&&app.joystick&&!SDL_JoystickGetAttached(app.joystick)){SDL_JoystickClose(app.joystick);app.joystick=nullptr;}
        else if(e.type==SDL_KEYDOWN){auto k=e.key.keysym.sym;int action=-1;if(k==SDLK_UP)action=0;else if(k==SDLK_DOWN)action=1;else if(k==SDLK_LEFT)action=2;else if(k==SDLK_RIGHT)action=3;else if(k==SDLK_RETURN||k==SDLK_SPACE||k==SDLK_KP_ENTER)action=4;else if(k==SDLK_ESCAPE||k==SDLK_BACKSPACE)action=5;if(action>=0)app.action(action);}
        else if(e.type==SDL_CONTROLLERBUTTONDOWN){int action=-1;switch(e.cbutton.button){case SDL_CONTROLLER_BUTTON_DPAD_UP:action=0;break;case SDL_CONTROLLER_BUTTON_DPAD_DOWN:action=1;break;case SDL_CONTROLLER_BUTTON_DPAD_LEFT:action=2;break;case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:action=3;break;case SDL_CONTROLLER_BUTTON_A:action=4;break;case SDL_CONTROLLER_BUTTON_B:action=5;break;}if(action>=0)app.action(action);}
        else if(e.type==SDL_JOYBUTTONDOWN&&app.joystick){int action=-1;switch(e.jbutton.button){case 0:action=4;break;case 1:action=5;break;}if(action>=0)app.action(action);}
        else if(e.type==SDL_JOYHATMOTION&&app.joystick){Uint8 h=e.jhat.value;if((h&SDL_HAT_UP)&&!(app.hat&SDL_HAT_UP))app.action(0);if((h&SDL_HAT_DOWN)&&!(app.hat&SDL_HAT_DOWN))app.action(1);if((h&SDL_HAT_LEFT)&&!(app.hat&SDL_HAT_LEFT))app.action(2);if((h&SDL_HAT_RIGHT)&&!(app.hat&SDL_HAT_RIGHT))app.action(3);app.hat=h;}
        else if((e.type==SDL_CONTROLLERAXISMOTION&&e.caxis.axis<=1)||(e.type==SDL_JOYAXISMOTION&&app.joystick&&e.jaxis.axis<=1)){int axis=e.type==SDL_CONTROLLERAXISMOTION?e.caxis.axis:e.jaxis.axis;int value=e.type==SDL_CONTROLLERAXISMOTION?e.caxis.value:e.jaxis.value;int &prev=axis==0?app.axis_x:app.axis_y;int next=value>16000?1:value< -16000?-1:0;if(next&&next!=prev)app.action(axis==0?(next<0?2:3):(next<0?0:1));prev=next;}
    }app.poll();app.update_motion();
    if(before!=std::make_tuple(app.screen,app.home_focus,app.all_focus,app.focus,app.selected,app.picker,app.pick_index,app.scroll,app.dir,app.pkg,app.license,app.message,app.child))dirty=true;
    bool splash_active=SDL_GetTicks()-app.start<1600;bool animating=app.animating();if(splash_active||was_splash!=splash_active||animating||was_animating)dirty=true;was_splash=splash_active;was_animating=animating;
    if(!dirty){SDL_Delay(16);continue;}dirty=false;
    int width,height;SDL_GL_GetDrawableSize(window,&width,&height);glViewport(0,0,width,height);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);nvgBeginFrame(vg,640,480,(float)width/640);draw(app);nvgEndFrame(vg);
    if(capture){std::vector<unsigned char> pixels((size_t)width*height*4);glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());FILE *f=fopen(capture,"wb");if(f){fprintf(f,"P6\n%d %d\n255\n",width,height);for(int y=height-1;y>=0;y--)for(int x=0;x<width;x++)fwrite(&pixels[((size_t)y*width+x)*4],1,3,f);fclose(f);}else fprintf(stderr,"Could not save screenshot\n");app.running=false;}
    SDL_GL_SwapWindow(window);SDL_Delay(16);}}
#ifdef __APPLE__
    nvgDeleteGL3(vg);
#else
    nvgDeleteGLES2(vg);
#endif
    SDL_GL_DeleteContext(context);SDL_DestroyWindow(window);SDL_Quit();return 0;
}
