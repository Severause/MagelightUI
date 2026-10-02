// AppCore reference-driver control: same SA page, same scripted navigation and
// injection as harness.cpp, but rendered by Ultralight's own D3D11 driver.
#include <AppCore/AppCore.h>
#include <cstdio>
#include <string>
using namespace ultralight;

struct Driver : public AppListener {
    RefPtr<View> view; int frame = 0;
    void OnUpdate() override {
        ++frame;
        if (frame == 150) view->EvaluateScript("(function(){var it=Array.from(document.querySelectorAll('.sa-rail__item')).find(function(e){return e.textContent.trim().indexOf('Inventory')===0;}); if(it) it.click();})()");
        if (frame == 230) view->EvaluateScript("(function(){var c=Array.from(document.querySelectorAll('.sa-rail__child')).find(function(e){return e.textContent.trim()==='Stats';}); if(c) c.click();})()");
        if (frame == 260 || frame == 320) view->EvaluateScript("window.receivePageData(JSON.stringify({page:'inventory',actors:[{formId:20,name:'Prisoner',carryWeight:{current:12,max:300}}],selectedActorFormId:20,selectedActorName:'Prisoner',goldCount:0,categories:{},totalItems:3,totalWeight:12,totalValue:0,stats:{level:1,race:'Nord',sex:'Male',className:'Spellsword',health:100,magicka:100,stamina:100,perkCount:1,perkPoints:0,skills:{oneHanded:20,twoHanded:25,archery:15,block:20,heavyArmor:15,smithing:20,destruction:15,restoration:15,conjuration:15,alteration:15,enchanting:15,illusion:15,lightArmor:20,sneak:15,lockpicking:15,pickpocket:15,speech:20,alchemy:15}}}))");
        if (frame == 400) { FILE* f = fopen("inject.js", "rb"); if (f) { std::string js; char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) js.append(buf, n); fclose(f); view->EvaluateScript(js.c_str()); } }
    }
};

int main() {
    Settings settings;
    settings.file_system_path = "./assets/";
    settings.force_cpu_renderer = false;
    Config config;
    RefPtr<App> app = App::Create(settings, config);
    RefPtr<Window> window = Window::Create(app->main_monitor(), 1280, 720, false, kWindowFlags_Titled);
    window->SetTitle("appcoreprobe");
    ViewConfig vc; vc.is_transparent = true; vc.is_accelerated = true;
    RefPtr<View> view = app->renderer()->CreateView(2560, 1440, vc, nullptr);
    RefPtr<Overlay> overlay = Overlay::Create(window, view, 0, 0);
    view->LoadURL("file:///sa/index.html");
    Driver d; d.view = view;
    app->set_listener(&d);
    app->Run();
    return 0;
}

