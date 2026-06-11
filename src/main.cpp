#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <commdlg.h>
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <shellapi.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "AudioDucker.h"
#include "resource.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ── D3D ──
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRTView = nullptr;
static UINT                     g_RWidth = 0, g_RHeight = 0;

#define WM_TRAYICON (WM_USER + 100)
static NOTIFYICONDATAW g_nid = {};
static HWND g_hwnd = nullptr;
static void UpdateTrayIcon(HWND hwnd);

// ── State ──
static AudioDucker g_Ducker;
static std::vector<std::string> g_Pri, g_Sec;
static int  g_SPri=-1, g_SSec=-1, g_ActD=0, g_RecD=0, g_AtkD=150, g_RelD=800, g_DuckP=50;
static float g_ThrDb=-30.0f;
static bool g_Run=false, g_Dirty=false;
static std::string g_CfgDir, g_CfgName="default", g_CfgPath;
static std::chrono::system_clock::time_point g_ApplyTime;
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
static void SaveSet(){
    std::ofstream f(SP()); if(!f)return;
    f<<"{\"lastConfig\":\""<<g_CfgPath<<"\"}\n";
}
static void LoadSet(){
    std::ifstream f(SP()); if(!f){ g_CfgPath=DP(); g_CfgName="default"; return; }
    std::string line; std::getline(f,line);
    auto s=line.find_last_of('"'), t=line.find_last_of('"',s-1);
    if(s==std::string::npos||t==std::string::npos||s-t<2){ g_CfgPath=DP(); g_CfgName="default"; return; }
    g_CfgPath=line.substr(t+1,s-t-1);
    g_CfgName=N(g_CfgPath); auto d=g_CfgName.rfind('.'); if(d!=std::string::npos) g_CfgName=g_CfgName.substr(0,d);
    if(g_CfgPath.find(':')==std::string::npos) g_CfgPath=g_CfgDir+g_CfgPath;
}
static void SetPath(const std::string& p){ g_CfgPath=p; g_CfgName=N(p); auto d=g_CfgName.rfind('.'); if(d!=std::string::npos) g_CfgName=g_CfgName.substr(0,d); SaveSet(); }
static void SV(){ AC(); AD(); AudioDucker::ExportConfig(g_Ducker.GetApps(),g_Ducker.GetDuckConfig(),g_CfgPath); }
static void TA(){ if(!g_Dirty)return; auto n=std::chrono::steady_clock::now(); if(n-g_SaveTime<std::chrono::milliseconds(800))return; g_SaveTime=n; g_Dirty=false; SV(); }

// ── Theme ──
static void Theme(){
    ImGui::StyleColorsDark(); auto& s=ImGui::GetStyle();
    s.FrameRounding=5; s.GrabRounding=5; s.FramePadding=ImVec2(8,5); s.ItemSpacing=ImVec2(8,5);
    s.WindowPadding=ImVec2(12,12);
    auto& c=s.Colors;
    c[ImGuiCol_WindowBg]=ImVec4(0.08f,0.08f,0.10f,1);
    c[ImGuiCol_FrameBg]=ImVec4(0.15f,0.15f,0.19f,1);
    c[ImGuiCol_FrameBgHovered]=ImVec4(0.20f,0.20f,0.25f,1);
    c[ImGuiCol_Button]=ImVec4(0.22f,0.42f,0.78f,0.75f);
    c[ImGuiCol_ButtonHovered]=ImVec4(0.28f,0.50f,0.88f,0.88f);
    c[ImGuiCol_SliderGrab]=ImVec4(0.28f,0.52f,0.92f,0.85f);
    c[ImGuiCol_Header]=ImVec4(0.22f,0.42f,0.78f,0.45f);
    c[ImGuiCol_HeaderHovered]=ImVec4(0.28f,0.50f,0.88f,0.65f);
}

// ── D3D helpers ──
static bool D3D_Create(HWND h){ DXGI_SWAP_CHAIN_DESC d={}; d.BufferCount=2; d.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; d.BufferDesc.RefreshRate.Numerator=60; d.BufferDesc.RefreshRate.Denominator=1; d.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH; d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; d.OutputWindow=h; d.SampleDesc.Count=1; d.Windowed=TRUE; d.SwapEffect=DXGI_SWAP_EFFECT_DISCARD; D3D_FEATURE_LEVEL fl=D3D_FEATURE_LEVEL_11_0; HRESULT hr=D3D11CreateDeviceAndSwapChain(0,D3D_DRIVER_TYPE_HARDWARE,0,0,&fl,1,D3D11_SDK_VERSION,&d,&g_pSwapChain,&g_pd3dDevice,0,&g_pd3dDeviceContext); if(FAILED(hr))return false; ID3D11Texture2D* bb=0; g_pSwapChain->GetBuffer(0,IID_PPV_ARGS(&bb)); if(bb){g_pd3dDevice->CreateRenderTargetView(bb,0,&g_mainRTView);bb->Release();} return true; }
static void D3D_KRT(){ if(g_mainRTView){g_mainRTView->Release();g_mainRTView=0;} }
static void D3D_CRT(){ ID3D11Texture2D* bb=0; g_pSwapChain->GetBuffer(0,IID_PPV_ARGS(&bb)); if(bb){g_pd3dDevice->CreateRenderTargetView(bb,0,&g_mainRTView);bb->Release();} }
static void D3D_Kill(){ D3D_KRT(); if(g_pSwapChain){g_pSwapChain->Release();g_pSwapChain=0;} if(g_pd3dDeviceContext){g_pd3dDeviceContext->Release();g_pd3dDeviceContext=0;} if(g_pd3dDevice){g_pd3dDevice->Release();g_pd3dDevice=0;} }

// ── WndProc ──
static LRESULT WINAPI WP(HWND h,UINT m,WPARAM w,LPARAM l){
    if(ImGui_ImplWin32_WndProcHandler(h,m,w,l)) return true;
    switch(m){
    case WM_SIZE: if(w!=SIZE_MINIMIZED){g_RWidth=LOWORD(l);g_RHeight=HIWORD(l);} else ShowWindow(h,SW_HIDE); return 0;
    case WM_CLOSE: ShowWindow(h,SW_HIDE); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_TRAYICON:
        if(l==WM_LBUTTONDBLCLK){
            ShowWindow(h,SW_SHOW); SetForegroundWindow(h);
        }
        if(l==WM_RBUTTONUP){
            POINT pt; GetCursorPos(&pt);
            HMENU menu=CreatePopupMenu();
            AppendMenuW(menu,MF_STRING,1,g_Run?L"停止闪避":L"开启闪避");
            AppendMenuW(menu,MF_STRING,2,L"显示窗口");
            AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
            AppendMenuW(menu,MF_STRING,3,L"退出");
            SetForegroundWindow(h);
            int cmd=TrackPopupMenu(menu,TPM_RIGHTBUTTON|TPM_RETURNCMD,pt.x,pt.y,0,h,nullptr);
            DestroyMenu(menu);
            if(cmd==1){ g_Run=!g_Run; g_Dirty=true; if(g_Run){AC();AD();g_Ducker.Start();} else g_Ducker.Stop(); UpdateTrayIcon(h); }
            else if(cmd==2) ShowWindow(h,SW_SHOW);
            else if(cmd==3){ g_nid.uFlags=0; Shell_NotifyIconW(NIM_DELETE,&g_nid); DestroyWindow(h); }
        }
    }
    return DefWindowProcW(h,m,w,l);
}

// ── Tray icon ──
static void UpdateTrayIcon(HWND hwnd){
    static bool added=false;
    g_nid.cbSize=sizeof(g_nid); g_nid.hWnd=hwnd; g_nid.uID=1;
    g_nid.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;
    g_nid.uCallbackMessage=WM_TRAYICON;
    g_nid.hIcon = LoadIcon(GetModuleHandleW(nullptr),
        MAKEINTRESOURCE(g_Run ? IDI_GREEN : IDI_RED));
    wcscpy(g_nid.szTip, L"OpenAudioDucking - ");
    wcscat(g_nid.szTip, g_Run ? L"运行中" : L"已停止");
    Shell_NotifyIconW(added ? NIM_MODIFY : NIM_ADD, &g_nid);
    added=true;
}

// ── VU bar ──
static void DrawVU(ImDrawList* dl,ImVec2 mi,ImVec2 ma,float pk){
    if(pk<0)pk=0; if(pk>1)pk=1; float w=(ma.x-mi.x)*pk;
    ImU32 c=pk<0.6f?IM_COL32(40,140,40,80):(pk<0.85f?IM_COL32(200,180,40,90):IM_COL32(200,40,40,100));
    dl->AddRectFilled(mi,ImVec2(mi.x+w,ma.y),c);
}

// ── App list ──
static void RenderList(const char* tag, std::vector<std::string>& items, int& sel,
                       const std::map<std::wstring,AppStatus>& sm, int idOff, float width,
                       bool& scanReq)
{
    ImGui::BeginChild(("##L"+std::string(tag)).c_str(), ImVec2(width,180), ImGuiChildFlags_Borders);
    ImDrawList* dl=ImGui::GetWindowDrawList();
    for(int i=0;i<(int)items.size();i++){
        std::wstring wn(items[i].begin(),items[i].end()); auto it=sm.find(wn);
        float pk=it!=sm.end()?it->second.smoothedPeak:0, dBv=it!=sm.end()?it->second.peakDb:-100;
        bool dk=it!=sm.end()?it->second.isDucked:false;
        ImGui::PushID(idOff+i);
        bool s=(sel==i);
        if(ImGui::Selectable("##s",&s)) sel=(sel==i)?-1:i;
        ImVec2 mi=ImGui::GetItemRectMin(), ma=ImGui::GetItemRectMax();
        if(pk>0.001f) DrawVU(dl,mi,ma,pk);
        char bf[256]; snprintf(bf,sizeof(bf),"%s  %s  %+.1f dB",items[i].c_str(),(pk>0.001f?"[+]":"[-]"),dBv);
        dl->AddText(ImVec2(mi.x+4,mi.y+2), dk?IM_COL32(255,220,100,255):IM_COL32(255,255,255,255),bf);
        ImGui::PopID();
    }
    ImGui::EndChild();
    if(ImGui::Button("扫描",ImVec2(55,0))) scanReq=true;
    ImGui::SameLine();
    if(ImGui::Button("添加",ImVec2(55,0))){
        auto p=OD(L"Exe(*.exe)\0*.exe\0All\0*.*\0");
        if(!p.empty()){ items.push_back(N(S(p))); sel=-1; g_Dirty=true; }
    }
    ImGui::SameLine();
    if(ImGui::Button("删除",ImVec2(55,0))){
        if(sel>=0&&sel<(int)items.size()){ items.erase(items.begin()+sel); g_Dirty=true; }
        sel=-1;
    }
}


// ── Tooltip helper ──
static void Tip(const char* t){ if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s",t); }

// ── main ──
int main(int, char**)
{
    { wchar_t b[MAX_PATH]; GetModuleFileNameW(0,b,MAX_PATH);
      g_CfgDir=S(b); auto p=g_CfgDir.find_last_of("/\\"); g_CfgDir=g_CfgDir.substr(0,p+1); }

    WNDCLASSEXW wc={sizeof(wc),CS_CLASSDC,WP,0,0,GetModuleHandleW(0),
        LoadIcon(GetModuleHandleW(0),MAKEINTRESOURCE(IDI_APP)),LoadCursor(0,IDC_ARROW),0,0,L"OpenAudioDucking",
        LoadIcon(GetModuleHandleW(0),MAKEINTRESOURCE(IDI_APP))};
    RegisterClassExW(&wc);
    HWND hwnd=CreateWindowW(wc.lpszClassName,L"OpenAudioDucking",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,820,555,0,0,wc.hInstance,0);
    if(!D3D_Create(hwnd)){ D3D_Kill(); return 1; }
    ShowWindow(hwnd,SW_SHOWDEFAULT); UpdateWindow(hwnd);
    g_hwnd = hwnd;
    UpdateTrayIcon(hwnd);

    IMGUI_CHECKVERSION(); ImGui::CreateContext(); auto& io=ImGui::GetIO();
    io.IniFilename=0;
    // Font
    { ImFontConfig fc; fc.OversampleH=2; fc.OversampleV=2;
      static const ImWchar zh[]={0x0020,0x00FF,0x2000,0x206F,0x3000,0x30FF,0x4E00,0x9FFF,0xFF00,0xFFEF,0};
      if(!io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", 15.0f, &fc, zh))
        io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/simsun.ttc", 15.0f, &fc, zh);
    }
    Theme();
    ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(g_pd3dDevice,g_pd3dDeviceContext);

    LoadSet();
    { std::vector<AppConfig> a; DuckConfig c;
      if(AudioDucker::ImportConfig(g_CfgPath,a,c)){ g_Ducker.SetApps(a); g_Ducker.SetDuckConfig(c); }
      DU(); AC(); }

    bool done=false;
    while(!done){
        MSG msg; bool had=false;
        while(PeekMessageW(&msg,0,0,0,PM_REMOVE)){ had=true; TranslateMessage(&msg); DispatchMessageW(&msg); if(msg.message==WM_QUIT)done=true; }
        if(done)break; if(!had) Sleep(5);

        if(g_RWidth&&g_RHeight){ D3D_KRT(); g_pSwapChain->ResizeBuffers(0,g_RWidth,g_RHeight,DXGI_FORMAT_UNKNOWN,0); g_RWidth=g_RHeight=0; D3D_CRT(); }

        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        g_Ducker.Process(); auto& sm=g_Ducker.GetStatusMap();
        TA();

        {
            ImGuiViewport* vp=ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->WorkPos); ImGui::SetNextWindowSize(vp->WorkSize);
            ImGui::Begin("Main",0,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoBringToFrontOnFocus);

            // ═══ App lists ═══
            float avail=ImGui::GetContentRegionAvail().x;
            float gap=10, cw=(avail-gap)*0.5f;
            ImGui::TextUnformatted("主要应用");
            ImGui::SameLine(cw+gap);
            ImGui::TextUnformatted("次要应用");

            bool scanP=false, scanS=false;
            ImGui::BeginChild("##PCol",ImVec2(cw,240),ImGuiChildFlags_None);
            RenderList("P",g_Pri,g_SPri,sm,0,cw,scanP);
            ImGui::EndChild();
            ImGui::SameLine(0,gap);
            ImGui::BeginChild("##SCol",ImVec2(cw,240),ImGuiChildFlags_None);
            RenderList("S",g_Sec,g_SSec,sm,2000,cw,scanS);
            ImGui::EndChild();
            if(scanP) ImGui::OpenPopup("选择应用##P");
            if(scanS) ImGui::OpenPopup("选择应用##S");

            ImGui::Spacing();

            // ═══ Params (collapsible) ═══
            {
                char hdr[128]="闪避参数";
                if(g_ApplyTime.time_since_epoch().count()>0){
                    auto t=std::chrono::system_clock::to_time_t(g_ApplyTime);
                    struct tm tm_buf; localtime_s(&tm_buf,&t);
                    char ts[32]; strftime(ts,sizeof(ts),"%H:%M:%S",&tm_buf);
                    snprintf(hdr,sizeof(hdr),"闪避参数 - %s 更新",ts);
                }
                if(ImGui::CollapsingHeader(hdr,ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::PushItemWidth(70);
                auto row=[&](const char* lb,const char* tip,const char* id,int* v,const char* un,
                             const char* lb2=nullptr,const char* tip2=nullptr,const char* id2=nullptr,int* v2=nullptr,const char* un2=nullptr){
                    ImGui::Text("%s",lb); Tip(tip); ImGui::SameLine(115); ImGui::InputInt(id,v,0,0); ImGui::SameLine(); ImGui::Text("%s",un);
                    if(lb2){
                        ImGui::SameLine(280); ImGui::Text("%s",lb2); Tip(tip2); ImGui::SameLine(395); ImGui::InputInt(id2,v2,0,0); ImGui::SameLine(); ImGui::Text("%s",un2);
                    }
                };
                row("开始渐变","音量降低的过渡时长","##atk",&g_AtkD,"ms","结束渐变","音量恢复的过渡时长","##rel",&g_RelD,"ms");
                row("延迟启动","持续超过阈值多久后触发闪避","##ad",&g_ActD,"ms","延迟恢复","持续低于阈值多久后触发恢复","##rd",&g_RecD,"ms");
                ImGui::PopItemWidth();

                ImGui::Text("闪避量"); Tip("次要应用降低的音量百分比");
                ImGui::SameLine(115); ImGui::PushItemWidth(150);
                ImGui::SliderInt("##dp",&g_DuckP,0,100,"%d %%"); ImGui::PopItemWidth();

                ImGui::Text("触发阈值"); Tip("主应用音量超过此 dB 值时判定为播放中");
                ImGui::SameLine(115); ImGui::PushItemWidth(200);
                ImGui::SliderFloat("##th",&g_ThrDb,-100,0,"%.1f dB"); ImGui::PopItemWidth();
            }
            }

            ImGui::Spacing();

            // ═══ Buttons row ═══
            if(ImGui::Button("导入配置",ImVec2(100,0))){
                auto p=OD(L"JSON(*.json)\0*.json\0All\0*.*\0");
                if(!p.empty()){ std::vector<AppConfig> a; DuckConfig c; if(AudioDucker::ImportConfig(S(p),a,c)){ g_Ducker.SetApps(a); g_Ducker.SetDuckConfig(c); DU(); g_Dirty=true; SetPath(S(p)); } }
            }
            ImGui::SameLine();
            if(ImGui::Button("导出配置",ImVec2(100,0))){ AC(); AD(); auto p=SD(L"JSON(*.json)\0*.json\0",L"json"); if(!p.empty()) AudioDucker::ExportConfig(g_Ducker.GetApps(),g_Ducker.GetDuckConfig(),S(p)); }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.15f,0.55f,0.30f,0.85f));
            if(ImGui::Button("应用参数",ImVec2(80,0))){ AC(); g_Dirty=true; g_ApplyTime=std::chrono::system_clock::now(); }
            ImGui::PopStyleColor();

            // ═══ Status bar + Toggle ═══
            const char* ph=g_Ducker.GetPhaseName();
            char sbar[128]; snprintf(sbar,sizeof(sbar),"状态: %s",g_Run?ph:"已停止");
            float tw=220;
            ImGui::TextUnformatted(sbar);
            ImGui::SameLine();
            ImGui::Text("当前配置: %s",g_CfgPath.empty()?"默认":N(g_CfgPath).c_str());
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth()-tw-ImGui::GetStyle().WindowPadding.x);

            ImVec4 bc=g_Run?ImVec4(0.12f,0.62f,0.22f,0.90f):ImVec4(0.72f,0.18f,0.18f,0.90f);
            ImVec4 bh=g_Run?ImVec4(0.16f,0.68f,0.26f,0.95f):ImVec4(0.78f,0.22f,0.22f,0.95f);
            ImGui::PushStyleColor(ImGuiCol_Button,bc); ImGui::PushStyleColor(ImGuiCol_ButtonHovered,bh);
            if(ImGui::Button(g_Run?"  运行中  ":"  已停止  ",ImVec2(tw,34))){ g_Run=!g_Run; g_Dirty=true; if(g_Run){AC();AD();g_Ducker.Start();} else g_Ducker.Stop(); UpdateTrayIcon(hwnd); }
            ImGui::PopStyleColor(2);

            // ═══ Scan Popup ═══
            static int popSel=-1;
            static std::vector<std::string> popApps;
            static bool popPri=false;
            for(const char* t:{"选择应用##P","选择应用##S"}){
                ImVec2 c=ImGui::GetMainViewport()->GetCenter();
                ImGui::SetNextWindowPos(c,ImGuiCond_Appearing,ImVec2(0.5f,0.5f));
                if(ImGui::BeginPopupModal(t,nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
                    if(ImGui::IsWindowAppearing()){
                        popSel=-1; popPri=t[12]=='P';
                        auto apps=g_Ducker.GetActiveAudioSessions(); popApps.clear();
                        for(auto& ws:apps){
                            std::string n(ws.begin(),ws.end());
                            if(std::find(g_Pri.begin(),g_Pri.end(),n)==g_Pri.end()&&std::find(g_Sec.begin(),g_Sec.end(),n)==g_Sec.end())
                                popApps.push_back(n);
                        }
                    }
                    ImGui::TextUnformatted("选择要添加的应用:");
                    ImGui::BeginChild("##scanlist",ImVec2(300,200),ImGuiChildFlags_Borders);
                    for(int i=0;i<(int)popApps.size();i++){
                        bool s=(popSel==i);
                        if(ImGui::Selectable(popApps[i].c_str(),&s)) popSel=(popSel==i)?-1:i;
                    }
                    ImGui::EndChild();
                    ImGui::Spacing();
                    if(ImGui::Button("确定",ImVec2(100,0))){
                        if(popSel>=0&&popSel<(int)popApps.size()){ (popPri?g_Pri:g_Sec).push_back(popApps[popSel]); g_Dirty=true; }
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if(ImGui::Button("取消",ImVec2(100,0))) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
            }

            ImGui::End();
        }

        ImGui::Render(); const float cc[4]={0.05f,0.05f,0.07f,1};
        g_pd3dDeviceContext->OMSetRenderTargets(1,&g_mainRTView,0);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRTView,cc);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData()); g_pSwapChain->Present(1,0);
    }

    SV();
    g_nid.uFlags = 0; Shell_NotifyIconW(NIM_DELETE, &g_nid);
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    D3D_Kill(); DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName,wc.hInstance);
    return 0;
}
