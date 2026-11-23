// Exercise the input state machine and importer without a display or physical pad.
#define main launcher_main
#include "main.cpp"
#undef main
#include <cassert>
#include <fstream>
#include <sys/stat.h>

static void finish_motion(App &app) {
    app.motion_start=SDL_GetTicks()-App::motion_duration;
    app.update_motion();
}
static void finish_import(App &app) {
    for(int i=0;i<500&&app.child>0;i++){ SDL_Delay(10);app.poll(); }
    assert(app.child<0);
}
int main() {
    assert(SDL_Init(SDL_INIT_TIMER)==0);
    char temp[]="/tmp/v2n-launcher-test-XXXXXX";
    assert(mkdtemp(temp));fs::current_path(temp);
    std::ofstream("game.pkg")<<"package";std::ofstream("work.bin")<<"license";
    fs::path extractor=fs::current_path()/"fake-extract";
    {std::ofstream f(extractor);f<<"#!/bin/sh\nmkdir -p \"$6/PCSE12345\"\nprintf ok > \"$6/PCSE12345/eboot.bin\"\n";}
    assert(chmod(extractor.c_str(),0755)==0);setenv("V2N_EXTRACT",extractor.c_str(),1);
    App app;
    app.action(2);assert(app.selected==3 && app.moving);finish_motion(app);assert(!app.moving);
    app.action(3);assert(app.selected==0);app.action(3);app.action(3);finish_motion(app);
    assert(app.selected==1 && app.moving);finish_motion(app);assert(app.selected==2 && app.moving);finish_motion(app);assert(!app.moving);
    app.action(1);assert(app.home_focus==1);app.action(4);assert(app.screen==2 && app.all_focus==2);
    app.action(0);assert(app.all_focus==0);app.action(3);assert(app.all_focus==1);
    app.action(1);assert(app.all_focus==3);app.action(4);assert(app.selected==3 && !app.message.empty());
    app.action(5);assert(app.screen==0 && app.home_focus==1);
    app.action(3);assert(app.home_focus==2);app.action(4);assert(app.screen==1);
    app.action(4);assert(app.picker);int i=0;while(i<(int)app.items.size()&&app.items[i].path.filename()!="game.pkg")i++;
    assert(i<(int)app.items.size());app.pick_index=i;app.action(4);assert(!app.picker && app.pkg.filename()=="game.pkg");
    app.action(1);app.action(4);i=0;while(i<(int)app.items.size()&&app.items[i].path.filename()!="work.bin")i++;
    assert(i<(int)app.items.size());app.pick_index=i;app.action(4);assert(app.license.filename()=="work.bin");
    app.action(1);app.action(4);assert(app.child>0);finish_import(app);assert(fs::exists("PCSE12345/eboot.bin"));
    app.pkg=fs::current_path()/"game.pkg";app.license=fs::current_path()/"work.bin";app.start_import();finish_import(app);
    assert(app.message.find("Title exists")!=std::string::npos);assert(fs::exists(app.stage/"PCSE12345/eboot.bin"));
    app.action(5);assert(app.screen==0 && app.home_focus==2);app.action(0);assert(app.home_focus==0);
    fs::current_path("/tmp");fs::remove_all(temp);SDL_Quit();puts("launcher navigation, circular motion and import passed");
}
