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
    bool running=true, picker=false; int tab=0, focus=0, selected=0, pick_index=0, scroll=0, pick_kind=0;
    fs::path pkg,license,dir=fs::current_path(),stage; std::vector<Item> items; std::string message; pid_t child=-1; Uint32 start=SDL_GetTicks();
    SDL_GameController *controller=nullptr; SDL_Joystick *joystick=nullptr; int axis_x=0,axis_y=0; Uint8 hat=SDL_HAT_CENTERED;
    ~App(){ if(controller) SDL_GameControllerClose(controller); if(joystick) SDL_JoystickClose(joystick); }
    void open_pad(int idx){ if(controller||joystick) return; if(SDL_IsGameController(idx)) controller=SDL_GameControllerOpen(idx); else joystick=SDL_JoystickOpen(idx); }
    void scan(){ items.clear(); std::error_code ec; if(dir.has_parent_path()&&dir.parent_path()!=dir) items.push_back({dir.parent_path(),true});
        std::vector<Item> found; fs::directory_iterator it(dir,fs::directory_options::skip_permission_denied,ec),end;
        while(!ec&&it!=end){ auto p=it->path(); std::error_code e; bool d=it->is_directory(e); bool file=it->is_regular_file(e); std::string ext=p.extension().string(); std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return (char)std::tolower(c);}); if(d||(file&&(pick_kind==0?ext==".pkg":p.filename()=="work.bin"))) found.push_back({p,d}); it.increment(ec); }
        std::sort(found.begin(),found.end(),[](const Item&a,const Item&b){return a.dir!=b.dir?a.dir>b.dir:a.path.filename().string()<b.path.filename().string();}); items.insert(items.end(),found.begin(),found.end()); pick_index=scroll=0; if(ec) message="Could not read this directory.";
    }
    void browse(int kind){ pick_kind=kind; picker=true; message.clear(); scan(); }
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
    void action(int a){ // 0 up, 1 down, 2 left, 3 right, 4 select, 5 back, 6 tab
        if(child>0){if(a==5)message="Import is running; please wait.";return;}
        if(picker){if(a==5){picker=false;return;}if(a==0&&!items.empty())pick_index=std::max(0,pick_index-1);if(a==1&&!items.empty())pick_index=std::min((int)items.size()-1,pick_index+1);if(a==4&&!items.empty()){auto item=items[pick_index];if(item.dir){dir=item.path;scan();}else{(pick_kind==0?pkg:license)=item.path;picker=false; message.clear();}}if(pick_index<scroll)scroll=pick_index;if(pick_index>=scroll+8)scroll=pick_index-7;return;}
        if(a==5){if(tab==1){tab=0;focus=0;message.clear();}else running=false;return;}if(a==6){tab=1-tab;focus=0;message.clear();return;}if(tab==0){if(a==2)selected=std::max(0,selected-1);if(a==3)selected=std::min(3,selected+1);if(a==4)message="Games are sample entries for now.";}
        else{if(a==0)focus=std::max(0,focus-1);if(a==1)focus=std::min(2,focus+1);if(a==4){if(focus<2)browse(focus);else start_import();}}
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
static const char *game_subtitles[]={"BEYOND THE HORIZON","A WORLD BETWEEN STARS","INTO THE WILD","AFTER DARK"};
static void image_cover(int image,float x,float y,float w,float h,float radius,float alpha=1){
    if(!image){rect(x,y,w,h,rgba(51,68,76),radius);return;}
    int iw=0,ih=0;nvgImageSize(vg,image,&iw,&ih);if(iw<=0||ih<=0)return;
    float scale=std::max(w/iw,h/ih),dw=iw*scale,dh=ih*scale;
    nvgBeginPath(vg);nvgRoundedRect(vg,x,y,w,h,radius);
    nvgFillPaint(vg,nvgImagePattern(vg,x+(w-dw)/2,y+(h-dh)/2,dw,dh,0,image,alpha));nvgFill(vg);
}
static void footer_key(float x,const char *key,const char *meaning){rect(x,449,21,21,rgba(245,248,249),10.5);centered(x+10.5,459.5,key,11,rgba(16,23,28),true);label(x+28,453,meaning,10,rgba(204,214,220),true);}
static void header(const App &a){
    brand(25,17,26);label(60,19,"V2N",16,rgba(244,248,250),true);label(60,37,"LAUNCHER",8,rgba(157,177,188),true);
    rect(423,17,95,28,a.tab==0?rgba(255,255,255,38):rgba(255,255,255,0),14);centered(470,31,"GAMES",11,a.tab==0?rgba(255,255,255):rgba(160,179,190),true);
    rect(526,17,88,28,a.tab==1?rgba(255,255,255,38):rgba(255,255,255,0),14);centered(570,31,"IMPORT",11,a.tab==1?rgba(255,255,255):rgba(160,179,190),true);
}
static void footer(const App &a){rect(0,438,640,42,rgba(5,10,14,245));rect(0,438,640,1,rgba(255,255,255,28));label(25,452,"L / R",10,rgba(232,240,244),true);label(62,452,"TABS",10,rgba(153,170,181),true);footer_key(383,"A","SELECT");footer_key(498,"B",a.picker?"BACK":a.tab==0?"EXIT":"BACK");}
static void splash(Uint32 elapsed){
    rect(0,0,640,480,rgba(10,19,27));gradient(0,0,640,480,rgba(43,64,73),rgba(7,13,20));brand(284,125,72);centered(320,227,"V2N Launcher",26,rgba(247,250,251),true);centered(320,253,"YOUR GAMES, READY TO GO",10,rgba(172,190,199),true);label(208,361,"Getting things ready",11,rgba(181,196,203));rect(208,386,224,3,rgba(255,255,255,35),1.5);rect(208,386,224*std::min(1.0f,elapsed/1600.f),3,rgba(229,240,242),1.5);centered(320,450,"VITA2NATIVE",9,rgba(113,136,146),true);
}
static void draw_games(const App &a){
    label(25,72,"Recent games",20,rgba(248,251,253),true);label(525,79,"4 GAMES",10,rgba(174,192,200),true);
    nvgSave(vg);nvgScissor(vg,24,105,616,160);
    image_cover(artwork[a.selected],25,107,310,154,4);gradient(25,180,310,81,rgba(8,15,21,0),rgba(8,15,21,190));
    label(41,218,game_names[a.selected],19,rgba(255,255,255),true,275);label(42,240,game_subtitles[a.selected],9,rgba(226,235,236),true,260);
    outline(25,107,310,154,4,rgba(248,252,255),2);
    for(int i=1;i<4;i++){int id=(a.selected+i)%4;float x=345+(i-1)*103;image_cover(artwork[id],x,110,94,150,4);gradient(x,205,94,55,rgba(5,12,17,0),rgba(5,12,17,220));label(x+8,225,game_names[id],10,rgba(255,255,255),true,80);outline(x,110,94,150,4,rgba(255,255,255,45));}
    nvgRestore(vg);
    label(26,276,game_names[a.selected],17,rgba(245,249,250),true);label(26,298,"READY TO PLAY  ·  SAMPLE GAME",10,rgba(165,189,196),true);rect(26,299,4,4,rgba(121,205,130),2);
    rect(0,323,640,115,rgba(15,29,40,205));rect(0,323,640,1,rgba(255,255,255,20));
    label(25,341,"YOUR LIBRARY",11,rgba(205,223,224),true);label(25,360,"A home for your Vita games.",15,rgba(244,248,249),true);label(25,383,"Choose a game above to get started.",11,rgba(160,179,187));
    rect(405,343,210,71,rgba(255,255,255,12),5);outline(405,343,210,71,5,rgba(255,255,255,35));label(420,354,"ADD A GAME",10,rgba(210,227,228),true);label(420,374,"Switch to Import  →",13,rgba(244,248,250));
    if(!a.message.empty()){rect(25,399,355,23,rgba(16,27,34,240),4);label(33,405,a.message,10,rgba(232,239,241),false,335);}
}
static void draw_import(const App &a){
    label(25,72,"Import a game",20,rgba(248,251,253),true);label(25,99,"Select your package and its license to add a game.",12,rgba(174,193,201));
    for(int i=0;i<2;i++){float y=135+i*76;rect(25,y,590,64,rgba(24,40,51,225),5);outline(25,y,590,64,5,a.focus==i?rgba(241,248,250):rgba(119,150,162,90),a.focus==i?2:1);rect(39,y+14,36,36,rgba(255,255,255,20),4);centered(57,y+32,i==0?"P":"L",15,rgba(226,238,240),true);label(89,y+11,i==0?"Game package":"License file",13,rgba(244,249,251),true);std::string name=(i==0?a.pkg:a.license).filename().string();label(89,y+34,name.empty()?(i==0?"Choose a .pkg file":"Choose a work.bin file"):name,11,rgba(173,194,202),false,350);rect(534,y+17,64,30,rgba(255,255,255,24),4);centered(566,y+32,"BROWSE",10,rgba(240,247,249),true);}
    label(26,297,"IMPORT LOCATION",10,rgba(155,179,188),true);label(26,315,fs::current_path().string(),11,rgba(224,235,237),false,395);
    rect(453,304,162,40,a.focus==2?rgba(225,237,239):rgba(92,115,124),5);centered(534,324,a.child>0?"IMPORTING...":"IMPORT GAME",11,a.focus==2?rgba(14,28,35):rgba(225,236,239),true);
    rect(25,365,590,47,rgba(255,255,255,12),5);label(39,373,a.message.empty()?"Your files will be prepared for vita2native.":a.message,11,rgba(199,218,224),false,560);if(a.message.empty())label(39,391,"Your title is saved in the current directory.",10,rgba(143,168,178));
}
static void draw_picker(const App &a){
    label(25,72,a.pick_kind==0?"Choose a package":"Choose work.bin",20,rgba(248,251,253),true);label(25,100,a.dir.string(),11,rgba(174,193,201),false,590);
    if(a.items.empty())label(36,156,"No matching files or folders.",12,rgba(168,189,197));
    for(int row=0;row<8&&a.scroll+row<(int)a.items.size();row++){int idx=a.scroll+row;float y=127+row*37;const auto &it=a.items[idx];if(idx==a.pick_index){rect(25,y,590,33,rgba(255,255,255,24),4);outline(25,y,590,33,4,rgba(239,247,250),1.5);}label(38,y+9,it.dir?"DIR":"FILE",10,rgba(159,188,197),true);std::string name=it.path==a.dir.parent_path()?"..":it.path.filename().string();label(90,y+8,name,12,rgba(238,247,249),false,505);}
}
static void draw(App &a){
    Uint32 elapsed=SDL_GetTicks()-a.start;if(elapsed<1600){splash(elapsed);return;}
    image_cover(artwork[a.selected],0,0,640,440,0,.48f);rect(0,0,640,440,rgba(12,25,35,190));gradient(0,0,640,440,rgba(30,43,52,75),rgba(10,22,30,240));header(a);
    if(a.picker)draw_picker(a);else if(a.tab==0)draw_games(a);else draw_import(a);footer(a);
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
    if(capture){app.start=SDL_GetTicks()-2000;const char *screen=getenv("V2N_LAUNCHER_SCREEN");if(screen&&strcmp(screen,"import")==0)app.tab=1;if(screen&&strcmp(screen,"picker")==0){app.tab=1;app.browse(0);}}
    for(int i=0;i<SDL_NumJoysticks();i++)app.open_pad(i);
    bool dirty=true, was_splash=true;
    while(app.running){
    auto before=std::make_tuple(app.tab,app.focus,app.selected,app.picker,app.pick_index,app.scroll,app.dir,app.pkg,app.license,app.message,app.child);
    SDL_Event e;while(SDL_PollEvent(&e)){
        if(e.type==SDL_WINDOWEVENT&&(e.window.event==SDL_WINDOWEVENT_EXPOSED||e.window.event==SDL_WINDOWEVENT_SIZE_CHANGED||e.window.event==SDL_WINDOWEVENT_SHOWN))dirty=true;if(e.type==SDL_QUIT){if(app.child<=0)app.running=false;}
        else if(e.type==SDL_CONTROLLERDEVICEADDED||e.type==SDL_JOYDEVICEADDED)app.open_pad(e.type==SDL_CONTROLLERDEVICEADDED?e.cdevice.which:e.jdevice.which);
        else if(e.type==SDL_CONTROLLERDEVICEREMOVED&&app.controller&&!SDL_GameControllerGetAttached(app.controller)){SDL_GameControllerClose(app.controller);app.controller=nullptr;}
        else if(e.type==SDL_JOYDEVICEREMOVED&&app.joystick&&!SDL_JoystickGetAttached(app.joystick)){SDL_JoystickClose(app.joystick);app.joystick=nullptr;}
        else if(e.type==SDL_KEYDOWN){auto k=e.key.keysym.sym;int action=-1;if(k==SDLK_UP)action=0;else if(k==SDLK_DOWN)action=1;else if(k==SDLK_LEFT)action=2;else if(k==SDLK_RIGHT)action=3;else if(k==SDLK_RETURN||k==SDLK_SPACE||k==SDLK_KP_ENTER)action=4;else if(k==SDLK_ESCAPE||k==SDLK_BACKSPACE)action=5;else if(k==SDLK_TAB||k==SDLK_q||k==SDLK_e)action=6;if(action>=0)app.action(action);}
        else if(e.type==SDL_CONTROLLERBUTTONDOWN){int action=-1;switch(e.cbutton.button){case SDL_CONTROLLER_BUTTON_DPAD_UP:action=0;break;case SDL_CONTROLLER_BUTTON_DPAD_DOWN:action=1;break;case SDL_CONTROLLER_BUTTON_DPAD_LEFT:action=2;break;case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:action=3;break;case SDL_CONTROLLER_BUTTON_A:action=4;break;case SDL_CONTROLLER_BUTTON_B:action=5;break;case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:action=6;break;}if(action>=0)app.action(action);}
        else if(e.type==SDL_JOYBUTTONDOWN&&app.joystick){int action=-1;switch(e.jbutton.button){case 0:action=4;break;case 1:action=5;break;case 4:case 5:action=6;break;}if(action>=0)app.action(action);}
        else if(e.type==SDL_JOYHATMOTION&&app.joystick){Uint8 h=e.jhat.value;if((h&SDL_HAT_UP)&&!(app.hat&SDL_HAT_UP))app.action(0);if((h&SDL_HAT_DOWN)&&!(app.hat&SDL_HAT_DOWN))app.action(1);if((h&SDL_HAT_LEFT)&&!(app.hat&SDL_HAT_LEFT))app.action(2);if((h&SDL_HAT_RIGHT)&&!(app.hat&SDL_HAT_RIGHT))app.action(3);app.hat=h;}
        else if((e.type==SDL_CONTROLLERAXISMOTION&&e.caxis.axis<=1)||(e.type==SDL_JOYAXISMOTION&&app.joystick&&e.jaxis.axis<=1)){int axis=e.type==SDL_CONTROLLERAXISMOTION?e.caxis.axis:e.jaxis.axis;int value=e.type==SDL_CONTROLLERAXISMOTION?e.caxis.value:e.jaxis.value;int &prev=axis==0?app.axis_x:app.axis_y;int next=value>16000?1:value< -16000?-1:0;if(next&&next!=prev)app.action(axis==0?(next<0?2:3):(next<0?0:1));prev=next;}
    }app.poll();
    if(before!=std::make_tuple(app.tab,app.focus,app.selected,app.picker,app.pick_index,app.scroll,app.dir,app.pkg,app.license,app.message,app.child))dirty=true;
    bool splash_active=SDL_GetTicks()-app.start<1600;if(splash_active||was_splash!=splash_active)dirty=true;was_splash=splash_active;
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
