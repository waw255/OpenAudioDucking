#include <windows.h>
#include <d3d11.h>
#include <commdlg.h>
#include <string>
#include <vector>
#include <cstdio>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <chrono>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "AudioDucker.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ── D3D ──
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRTView = nullptr;
static UINT                     g_RWidth = 0, g_RHeight = 0;

// ── State ──
static AudioDucker g_Ducker;
static std::vector<std::string> g_Pri, g_Sec;
static int  g_SPri=-1, g_SSec=-1, g_ActD=0, g_RecD=0, g_AtkD=150, g_RelD=800, g_DuckP=50;
static float g_ThrDb=-30.0f;
static bool g_Run=false, g_Dirty=false;
static std::string g_CfgDir, g_CfgName="default", g_CfgPath;
static std::chrono::steady_clock::time_point g_SaveTime;

// ── Helpers ──
static std::string S(const std::wstring& w){ if(w.empty())return""; int l=WideCharToMultiByte(CP_UTF8,0,w.data(),(int)w.size(),0,0,0,0); std::string r(l,0); WideCharToMultiByte(CP_UTF8,0,w.data(),(int)w.size(),&r[0],l,0,0); return r; }
static std::wstring W(const std::string& s){ if(s.empty())return L""; int l=MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),0,0); std::wstring r(l,0); MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),&r[0],l); return r; }
static std::string N(const std::string& p){ auto x=p.find_last_of("/\\"); return(x!=std::string::npos)?p.substr(x+1):p; }
static std::wstring OD(const wchar_t* f){ wchar_t b[MAX_PATH*10]={}; OPENFILENAMEW o={sizeof(o)}; o.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_ALLOWMULTISELECT|OFN_EXPLORER; o.lpstrFilter=f; o.lpstrFile=b; o.nMaxFile=MAX_PATH*10; return GetOpenFileNameW(&o)?std::wstring(b):std::wstring{}; }
static std::wstring SD(const wchar_t* f,const wchar_t* e){ wchar_t b[MAX_PATH]={}; OPENFILENAMEW o={sizeof(o)}; o.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST; o.lpstrFilter=f; o.lpstrDefExt=e; o.lpstrFile=b; o.nMaxFile=MAX_PATH; return GetSaveFileNameW(&o)?std::wstring(b):std::wstring{}; }

// ── Sync ──
static void AC(){ auto& c=g_Ducker.GetDuckConfig(); c.activationDelayMs=g_ActD; c.recoveryDelayMs=g_RecD; c.attackDurationMs=g_AtkD; c.releaseDurationMs=g_RelD; c.thresholdDb=g_ThrDb; c.duckPercent=g_DuckP; }
static void DU(){
    g_Pri.clear(); g_Sec.clear();
    for(auto& a:g_Ducker.GetApps()){ std::string n(a.processName.begin(),a.processName.end()); a.isPrimary?g_Pri.push_back(n):g_Sec.push_back(n); }
    auto& c=g_Ducker.GetDuckConfig(); g_ActD=c.activationDelayMs; g_RecD=c.recoveryDelayMs; g_AtkD=c.attackDurationMs; g_RelD=c.releaseDurationMs; g_ThrDb=c.thresholdDb; g_DuckP=c.duckPercent; g_Run=g_Ducker.IsRunning();
}
static void AD(){ std::vector<AppConfig> a; for(auto& n:g_Pri){ AppConfig c; c.processName.assign(n.begin(),n.end()); c.isPrimary=true; a.push_back(c); } for(auto& n:g_Sec){ AppConfig c; c.processName.assign(n.begin(),n.end()); c.isPrimary=false; a.push_back(c); } g_Ducker.SetApps(a); }

// ── Auto-save ──
static std::string SP(){ return g_CfgDir+"settings.json"; }
static std::string DP(){ return g_CfgDir+"config.json"; }

static void SaveSettings(){
    std::ofstream f(SP()); if(!f)return;
    f<<"{\"lastConfig\":\""<<g_CfgPath<<"\"}\n";
}
static void LoadSettings(){
    std::ifstream f(SP()); if(!f){ g_CfgPath=DP(); g_CfgName="default"; return; }
    std::string line; std::getline(f,line);
    // {"lastConfig":"C:\\path\\file.json"}
    auto s=line.find_last_of('"');        // closing " of the value
    auto t=line.find_last_of('"',s-1);    // opening " of the value
    if(s==std::string::npos||t==std::string::npos||s-t<2){ g_CfgPath=DP(); g_CfgName="default"; return; }
    g_CfgPath=line.substr(t+1,s-t-1);
    g_CfgName=N(g_CfgPath); auto d=g_CfgName.rfind('.'); if(d!=std::string::npos) g_CfgName=g_CfgName.substr(0,d);
    if(g_CfgPath.find(':')==std::string::npos) g_CfgPath=g_CfgDir+g_CfgPath;
}

static void SetCfgPath(const std::string& p){
    g_CfgPath=p; g_CfgName=N(p); auto d=g_CfgName.rfind('.'); if(d!=std::string::npos) g_CfgName=g_CfgName.substr(0,d);
    SaveSettings();
}
static void SV(){ AC(); AD(); AudioDucker::ExportConfig(g_Ducker.GetApps(),g_Ducker.GetDuckConfig(),g_CfgPath); }
static void TA(){ if(!g_Dirty)return; auto n=std::chrono::steady_clock::now(); if(n-g_SaveTime<std::chrono::milliseconds(800))return; g_SaveTime=n; g_Dirty=false; SV(); }

// ── Theme ──
static void TM(){ ImGui::StyleColorsDark(); auto& s=ImGui::GetStyle(); s.FrameRounding=5; s.ChildRounding=5; s.GrabRounding=5; s.ScrollbarRounding=5; s.FramePadding=ImVec2(8,5); s.ItemSpacing=ImVec2(10,6); auto& c=s.Colors; c[ImGuiCol_WindowBg]=ImVec4(0.08f,0.08f,0.10f,1); c[ImGuiCol_ChildBg]=ImVec4(0.11f,0.11f,0.14f,1); c[ImGuiCol_FrameBg]=ImVec4(0.16f,0.16f,0.20f,1); c[ImGuiCol_Button]=ImVec4(0.20f,0.40f,0.75f,0.75f); c[ImGuiCol_ButtonHovered]=ImVec4(0.25f,0.48f,0.85f,0.88f); c[ImGuiCol_SliderGrab]=ImVec4(0.28f,0.52f,0.92f,0.85f); }

// ── D3D helpers ──
static bool D3D_Create(HWND h){ DXGI_SWAP_CHAIN_DESC d={}; d.BufferCount=2; d.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; d.BufferDesc.RefreshRate.Numerator=60; d.BufferDesc.RefreshRate.Denominator=1; d.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH; d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; d.OutputWindow=h; d.SampleDesc.Count=1; d.Windowed=TRUE; d.SwapEffect=DXGI_SWAP_EFFECT_DISCARD; D3D_FEATURE_LEVEL fl=D3D_FEATURE_LEVEL_11_0; HRESULT hr=D3D11CreateDeviceAndSwapChain(0,D3D_DRIVER_TYPE_HARDWARE,0,0,&fl,1,D3D11_SDK_VERSION,&d,&g_pSwapChain,&g_pd3dDevice,0,&g_pd3dDeviceContext); if(FAILED(hr))return false; ID3D11Texture2D* bb=0; g_pSwapChain->GetBuffer(0,IID_PPV_ARGS(&bb)); if(bb){g_pd3dDevice->CreateRenderTargetView(bb,0,&g_mainRTView);bb->Release();} return true; }
static void D3D_KRT(){ if(g_mainRTView){g_mainRTView->Release();g_mainRTView=0;} }
static void D3D_CRT(){ ID3D11Texture2D* bb=0; g_pSwapChain->GetBuffer(0,IID_PPV_ARGS(&bb)); if(bb){g_pd3dDevice->CreateRenderTargetView(bb,0,&g_mainRTView);bb->Release();} }
static void D3D_Kill(){ D3D_KRT(); if(g_pSwapChain){g_pSwapChain->Release();g_pSwapChain=0;} if(g_pd3dDeviceContext){g_pd3dDeviceContext->Release();g_pd3dDeviceContext=0;} if(g_pd3dDevice){g_pd3dDevice->Release();g_pd3dDevice=0;} }

// ── WndProc ──
static LRESULT WINAPI WP(HWND h,UINT m,WPARAM w,LPARAM l){
    if(ImGui_ImplWin32_WndProcHandler(h,m,w,l)) return true;
    switch(m){
    case WM_SIZE: if(w!=SIZE_MINIMIZED){g_RWidth=LOWORD(l);g_RHeight=HIWORD(l);} return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h,m,w,l);
}

// ── App list column ──
static void RenderList(const char* label, std::vector<std::string>& items, int& sel,
                       const std::map<std::wstring,AppStatus>& sm, int idOff, float width)
{
    ImGui::BeginChild(("##cl"+std::string(label)).c_str(), ImVec2(width,180), ImGuiChildFlags_Borders);
    ImDrawList* dl=ImGui::GetWindowDrawList();
    for(int i=0;i<(int)items.size();i++){
        std::wstring wn(items[i].begin(),items[i].end()); auto it=sm.find(wn);
        float pk=it!=sm.end()?it->second.smoothedPeak:0;
        float dBv=it!=sm.end()?it->second.peakDb:-100;
        bool dk=it!=sm.end()?it->second.isDucked:false;

        ImGui::PushID(idOff+i);
        bool s=(sel==i);
        if(ImGui::Selectable("##ss",&s))
            sel = (sel==i) ? -1 : i;
        ImVec2 mi=ImGui::GetItemRectMin(), ma=ImGui::GetItemRectMax();

        if(pk>0.001f){
            float w=(ma.x-mi.x)*std::min(pk,1.0f);
            ImU32 c=pk<0.6f?IM_COL32(40,140,40,80):(pk<0.85f?IM_COL32(200,180,40,90):IM_COL32(200,40,40,100));
            dl->AddRectFilled(mi,ImVec2(mi.x+w,ma.y),c);
        }
        char bf[256];
        snprintf(bf,sizeof(bf),"%s  %s  %+.1f dB",items[i].c_str(),(pk>0.001f?"[+]":"[-]"),dBv);
        dl->AddText(ImVec2(mi.x+4,mi.y+2), dk?IM_COL32(255,220,100,255):IM_COL32(255,255,255,255), bf);
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// ── main ──
int main(int, char**)
{
    // Config dir
    { wchar_t b[MAX_PATH]; GetModuleFileNameW(0,b,MAX_PATH);
      g_CfgDir=S(b); auto p=g_CfgDir.find_last_of("/\\"); g_CfgDir=g_CfgDir.substr(0,p+1); }

    // Window
    WNDCLASSEXW wc={sizeof(wc),CS_CLASSDC,WP,0,0,GetModuleHandleW(0),0,0,0,0,L"OpenAudioDucking",0};
    RegisterClassExW(&wc);
    HWND hwnd=CreateWindowW(wc.lpszClassName,L"OpenAudioDucking",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,760,600,0,0,wc.hInstance,0);
    if(!D3D_Create(hwnd)){ D3D_Kill(); return 1; }
    ShowWindow(hwnd,SW_SHOWDEFAULT); UpdateWindow(hwnd);

    // ImGui
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); auto& io=ImGui::GetIO();
    io.IniFilename=0;
    { ImFontConfig fc; fc.OversampleH=2; fc.OversampleV=2;
      static const ImWchar zh[]={0x0020,0x00FF,0x2000,0x206F,0x3000,0x30FF,0x4E00,0x9FFF,0xFF00,0xFFEF,0};
      io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc",15,&fc,zh); }
    TM();
    ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(g_pd3dDevice,g_pd3dDeviceContext);

    // Load config
    LoadSettings();
    { std::vector<AppConfig> a; DuckConfig c;
      if(AudioDucker::ImportConfig(g_CfgPath,a,c)){ g_Ducker.SetApps(a); g_Ducker.SetDuckConfig(c); }
      DU(); AC(); }

    bool done=false;
    while(!done){
        MSG msg;
        bool hadMsg=false;
        while(PeekMessageW(&msg,0,0,0,PM_REMOVE)){ hadMsg=true; TranslateMessage(&msg); DispatchMessageW(&msg); if(msg.message==WM_QUIT)done=true; }
        if(done)break;
        if(!hadMsg) Sleep(5);  // idle, save CPU

        // Deferred resize
        if(g_RWidth&&g_RHeight){ D3D_KRT(); g_pSwapChain->ResizeBuffers(0,g_RWidth,g_RHeight,DXGI_FORMAT_UNKNOWN,0); g_RWidth=g_RHeight=0; D3D_CRT(); }

        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        g_Ducker.Process(); auto& sm=g_Ducker.GetStatusMap();
        TA();

        {
            ImGuiViewport* vp=ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->WorkPos); ImGui::SetNextWindowSize(vp->WorkSize);
            ImGui::Begin("Main",0,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoBringToFrontOnFocus);

            char title[128]; snprintf(title,sizeof(title),"OpenAudioDucking - %s",g_CfgName.c_str());
            ImGui::SetCursorPosX((vp->WorkSize.x-ImGui::CalcTextSize(title).x)*0.5f);
            ImGui::TextUnformatted(title);

            ImGui::SetCursorPos(ImVec2(16, 36));
            ImGui::TextUnformatted("主要应用");
            ImGui::SetCursorPos(ImVec2(16, 56));
            RenderList("P",g_Pri,g_SPri,sm,0,340);
            ImGui::SetCursorPos(ImVec2(16, 246));
            if(ImGui::Button("扫描##p",ImVec2(60,0))) ImGui::OpenPopup("选择应用##p");
            ImGui::SameLine();
            if(ImGui::Button("添加##p",ImVec2(60,0))){
                auto p=OD(L"Exe(*.exe)\0*.exe\0All\0*.*\0");
                if(!p.empty()){ g_Pri.push_back(N(S(p))); g_SPri=-1; g_Dirty=true; }
            }
            ImGui::SameLine();
            if(ImGui::Button("删除##p",ImVec2(60,0))){
                if(g_SPri>=0&&g_SPri<(int)g_Pri.size()){ g_Pri.erase(g_Pri.begin()+g_SPri); g_Dirty=true; }
                g_SPri=-1;
            }

            ImGui::SetCursorPos(ImVec2(400, 36));
            ImGui::TextUnformatted("次要应用");
            ImGui::SetCursorPos(ImVec2(400, 56));
            RenderList("S",g_Sec,g_SSec,sm,2000,340);
            ImGui::SetCursorPos(ImVec2(400, 246));
            if(ImGui::Button("扫描##s",ImVec2(60,0))) ImGui::OpenPopup("选择应用##s");
            ImGui::SameLine();
            if(ImGui::Button("添加##s",ImVec2(60,0))){
                auto p=OD(L"Exe(*.exe)\0*.exe\0All\0*.*\0");
                if(!p.empty()){ g_Sec.push_back(N(S(p))); g_SSec=-1; g_Dirty=true; }
            }
            ImGui::SameLine();
            if(ImGui::Button("删除##s",ImVec2(60,0))){
                if(g_SSec>=0&&g_SSec<(int)g_Sec.size()){ g_Sec.erase(g_Sec.begin()+g_SSec); g_Dirty=true; }
                g_SSec=-1;
            }

            // ── Shared popup for scanning running audio apps ──
            static int popSel=-1;
            static std::vector<std::string> popApps;
            static bool popForPrimary=false;
            for(const char* tag:{"选择应用##p","选择应用##s"}){
                ImVec2 pp=ImGui::GetMainViewport()->WorkPos;
                ImGui::SetNextWindowPos(ImVec2(pp.x+100,pp.y+80),ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(380,320));
                if(ImGui::BeginPopupModal(tag,nullptr,ImGuiWindowFlags_NoResize)){
                    if(ImGui::IsWindowAppearing()){
                        popSel=-1; popForPrimary=(strstr(tag,"##p")!=nullptr);
                        auto apps=g_Ducker.GetActiveAudioSessions();
                        popApps.clear();
                        for(auto& ws:apps){
                            std::string n(ws.begin(),ws.end());
                            if(std::find(g_Pri.begin(),g_Pri.end(),n)==g_Pri.end()&&std::find(g_Sec.begin(),g_Sec.end(),n)==g_Sec.end())
                                popApps.push_back(n);
                        }
                    }
                    ImGui::Text("选择要添加的应用:");
                    ImGui::BeginChild("##scanlist",ImVec2(0,210),ImGuiChildFlags_Borders);
                    for(int i=0;i<(int)popApps.size();i++){
                        bool sel=(popSel==i);
                        if(ImGui::Selectable(popApps[i].c_str(),&sel)) popSel=(popSel==i)?-1:i;
                    }
                    ImGui::EndChild();
                    ImGui::Spacing();
                    if(ImGui::Button("确定",ImVec2(100,0))){
                        if(popSel>=0&&popSel<(int)popApps.size()){
                            if(popForPrimary) g_Pri.push_back(popApps[popSel]);
                            else g_Sec.push_back(popApps[popSel]);
                            g_Dirty=true;
                        }
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if(ImGui::Button("取消",ImVec2(100,0))) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
            }

            ImGui::SetCursorPos(ImVec2(16, 286));
            ImGui::SeparatorText("闪避参数");

            ImGui::PushItemWidth(65);
            ImGui::SetCursorPosX(16); ImGui::Text("开始渐变"); ImGui::SameLine(100); ImGui::InputInt("##atk",&g_AtkD,0,0); ImGui::SameLine(); ImGui::Text("ms");
            ImGui::SameLine(260); ImGui::Text("结束渐变"); ImGui::SameLine(360); ImGui::InputInt("##rel",&g_RelD,0,0); ImGui::SameLine(); ImGui::Text("ms");

            ImGui::SetCursorPosX(16); ImGui::Text("延迟启动"); ImGui::SameLine(100); ImGui::InputInt("##ad",&g_ActD,0,0); ImGui::SameLine(); ImGui::Text("ms");
            ImGui::SameLine(260); ImGui::Text("延迟恢复"); ImGui::SameLine(360); ImGui::InputInt("##rd",&g_RecD,0,0); ImGui::SameLine(); ImGui::Text("ms");
            ImGui::PopItemWidth();

            ImGui::SetCursorPosX(16); ImGui::Text("闪避量  "); ImGui::SameLine(100); ImGui::PushItemWidth(55); ImGui::InputInt("##dp",&g_DuckP,0,0); ImGui::PopItemWidth(); ImGui::SameLine(); ImGui::Text("%%");
            ImGui::SameLine(260); ImGui::PushItemWidth(140); ImGui::SliderInt("##dps",&g_DuckP,0,100,"%d%%"); ImGui::PopItemWidth();

            ImGui::SetCursorPosX(16); ImGui::Text("触发阈值"); ImGui::SameLine(100); ImGui::PushItemWidth(65); ImGui::InputFloat("##th",&g_ThrDb,0,0,"%.1f"); ImGui::PopItemWidth(); ImGui::SameLine(); ImGui::Text("dB");
            ImGui::SameLine(260); ImGui::PushItemWidth(200); ImGui::SliderFloat("##ths",&g_ThrDb,-100,0,"%.1f dB"); ImGui::PopItemWidth();

            ImGui::Spacing();

            if(ImGui::Button("导入配置",ImVec2(100,0))){
                auto p=OD(L"JSON(*.json)\0*.json\0All\0*.*\0");
                if(!p.empty()){ std::vector<AppConfig> a; DuckConfig c; if(AudioDucker::ImportConfig(S(p),a,c)){ g_Ducker.SetApps(a); g_Ducker.SetDuckConfig(c); DU(); g_Dirty=true; SetCfgPath(S(p)); } }
            }
            ImGui::SameLine();
            if(ImGui::Button("导出配置",ImVec2(100,0))){ AC(); AD(); auto p=SD(L"JSON(*.json)\0*.json\0",L"json"); if(!p.empty()) AudioDucker::ExportConfig(g_Ducker.GetApps(),g_Ducker.GetDuckConfig(),S(p)); }
            ImGui::SameLine(540);
            ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.15f,0.55f,0.30f,0.85f));
            if(ImGui::Button("应用",ImVec2(70,0))){ AC(); g_Dirty=true; }
            ImGui::PopStyleColor();

            ImGui::Spacing();
            float tw=220; ImGui::SetCursorPosX((vp->WorkSize.x-tw)*0.5f);
            ImVec4 bc=g_Run?ImVec4(0.12f,0.62f,0.22f,0.90f):ImVec4(0.72f,0.18f,0.18f,0.90f);
            ImVec4 bh=g_Run?ImVec4(0.16f,0.68f,0.26f,0.95f):ImVec4(0.78f,0.22f,0.22f,0.95f);
            ImGui::PushStyleColor(ImGuiCol_Button,bc); ImGui::PushStyleColor(ImGuiCol_ButtonHovered,bh);
            if(ImGui::Button(g_Run?"  运行中  ":"  已停止  ",ImVec2(tw,40)))
                { g_Run=!g_Run; g_Dirty=true; if(g_Run){AC();AD();g_Ducker.Start();} else g_Ducker.Stop(); }
            ImGui::PopStyleColor(2);

            ImGui::End();
        }

        ImGui::Render();
        const float cc[4]={0.05f,0.05f,0.07f,1};
        g_pd3dDeviceContext->OMSetRenderTargets(1,&g_mainRTView,0);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRTView,cc);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1,0);
    }

    SV();
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    D3D_Kill(); DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName,wc.hInstance);
    return 0;
}
