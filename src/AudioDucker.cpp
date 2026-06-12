#include <initguid.h>
#include "AudioDucker.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>
#include <tlhelp32.h>
#include <psapi.h>

// MinGW 手补
static const GUID IID_IAudioMeterInformation =
    {0xC02216F6,0x8C67,0x4B5B,{0x9D,0x00,0xD0,0x08,0xE7,0x3E,0x00,0x64}};
struct AudioMeterHelper : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float *pf) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT32 *pn) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 n, float *af) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD *pdw) = 0;
};

// ── PID name cache ────────────────────────────────────
static std::unordered_map<DWORD, std::wstring> g_PidCache;
static std::chrono::steady_clock::time_point g_CacheTime;

static void RefreshPidCache()
{
    auto now = std::chrono::steady_clock::now();
    if (now - g_CacheTime < std::chrono::milliseconds(2000)) return;
    g_CacheTime = now;
    g_PidCache.clear();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe))
    {
        do { g_PidCache[pe.th32ProcessID] = pe.szExeFile; }
        while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

static std::wstring PidToName(DWORD pid);

static std::wstring PidToExe(DWORD pid)
{
    std::wstring name;
    HANDLE h=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ, FALSE, pid);
    if(h){
        wchar_t buf[MAX_PATH];
        if(GetModuleBaseNameW(h,nullptr,buf,MAX_PATH))
            name=buf;
        CloseHandle(h);
    }
    if(name.empty()) name=PidToName(pid);
    return name;
}

static std::wstring PidToName(DWORD pid)
{
    auto it = g_PidCache.find(pid);
    return (it != g_PidCache.end()) ? it->second : L"";
}

static float PeakToDb(float peak)
{
    if (peak < 1e-7f) return -100.0f;
    return 20.0f * log10f(peak);
}

// ── COM ───────────────────────────────────────────────
bool AudioDucker::InitCOM()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return false;
    hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                          IID_IMMDeviceEnumerator, (void**)&pEnum_);
    if (FAILED(hr)) return false;
    hr = pEnum_->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice_);
    if (FAILED(hr)) return false;
    hr = pDevice_->Activate(IID_IAudioSessionManager2, CLSCTX_ALL, nullptr,
                            (void**)&pMgr_);
    if (FAILED(hr)) return false;
    comOk_ = true;
    return true;
}

void AudioDucker::CleanupCOM()
{
    if (pMgr_) { pMgr_->Release(); pMgr_ = nullptr; }
    if (pDevice_) { pDevice_->Release(); pDevice_ = nullptr; }
    if (pEnum_) { pEnum_->Release(); pEnum_ = nullptr; }
    if (comOk_) CoUninitialize();
    comOk_ = false;
}

// ── Helpers ───────────────────────────────────────────
std::wstring AudioDucker::ProcessNameFromPid(DWORD pid)
{
    return PidToName(pid);
}

static bool IEqual(const std::wstring& a, const std::wstring& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (::towlower(a[i]) != ::towlower(b[i])) return false;
    return true;
}

static bool IsApp(const std::wstring& session, const AppConfig& app)
{
    return IEqual(session, app.processName);
}

// ── Enumerate sessions ────────────────────────────────
void AudioDucker::EnumerateSessions()
{
    statusMap_.clear();
    if (!comOk_ || !pMgr_) return;

    RefreshPidCache();

    IAudioSessionEnumerator* pSes = nullptr;
    if (FAILED(pMgr_->GetSessionEnumerator(&pSes))) return;

    int cnt = 0; pSes->GetCount(&cnt);
    for (int i = 0; i < cnt; i++)
    {
        IAudioSessionControl* pCtrl = nullptr;
        if (FAILED(pSes->GetSession(i, &pCtrl))) continue;

        IAudioSessionControl2* pCtrl2 = nullptr;
        if (FAILED(pCtrl->QueryInterface(IID_IAudioSessionControl2, (void**)&pCtrl2)))
        { pCtrl->Release(); continue; }

        DWORD pid = 0; pCtrl2->GetProcessId(&pid);
        std::wstring name = PidToName(pid);
        if (name.empty()) { pCtrl2->Release(); pCtrl->Release(); continue; }

        bool found = false;
        for (auto& a : apps_) { if (IsApp(name, a)) { found = true; break; } }
        if (!found) { pCtrl2->Release(); pCtrl->Release(); continue; }

        float peak = 0.0f;
        void* pRaw = nullptr;
        if (SUCCEEDED(pCtrl->QueryInterface(IID_IAudioMeterInformation, &pRaw)))
        {
            auto* pm = static_cast<AudioMeterHelper*>(pRaw);
            pm->GetPeakValue(&peak);
            pm->Release();
        }

        // 快升慢降平滑
        const float alphaUp   = 0.7f;
        const float alphaDown = 0.4f;
        float oldSmooth = 0.0f;
        {
            auto it = smoothCache_.find(name);
            if (it != smoothCache_.end()) oldSmooth = it->second;
        }
        float alpha = (peak > oldSmooth) ? alphaUp : alphaDown;
        float smooth = oldSmooth * (1.0f - alpha) + peak * alpha;
        smoothCache_[name] = smooth;

        AppStatus st;
        st.processName = name;
        st.isPlaying = (PeakToDb(smooth) > duckConfig_.thresholdDb);
        st.isDucked = false;
        st.peakLinear = peak;
        st.peakDb = PeakToDb(smooth);
        st.smoothedPeak = smooth;

        // ── 同一遍遍历内做软渐变音量 ──
        bool isSecondary = false;
        for (auto& a : apps_) { if (!a.isPrimary && IsApp(name, a)) { isSecondary = true; break; } }
        if (isSecondary)
        {
            auto& vs = volStates_[name];
            const float tickMs = 40.0f;
            float duration = (vs.target < vs.current)
                ? std::max(1.0f, (float)duckConfig_.attackDurationMs)
                : std::max(1.0f, (float)duckConfig_.releaseDurationMs);
            float step = 1.0f / (duration / tickMs);

            if (vs.current < vs.target)
                vs.current = std::min(vs.target, vs.current + step);
            else if (vs.current > vs.target)
                vs.current = std::max(vs.target, vs.current - step);

            ISimpleAudioVolume* pVol = nullptr;
            if (SUCCEEDED(pCtrl->QueryInterface(IID_ISimpleAudioVolume, (void**)&pVol)))
            { pVol->SetMasterVolume(vs.current, nullptr); pVol->Release(); }
            st.isDucked = (vs.current < 0.99f);
        }

        statusMap_[name] = st;

        pCtrl2->Release(); pCtrl->Release();
    }
    pSes->Release();
}

// ── Ctor / Dtor ───────────────────────────────────────
AudioDucker::AudioDucker()  { InitCOM(); }
AudioDucker::~AudioDucker() { Stop(); CleanupCOM(); }
void AudioDucker::AddApp(const AppConfig& c)    { apps_.push_back(c); }
void AudioDucker::RemoveApp(const std::wstring& n) { apps_.erase(std::remove_if(apps_.begin(), apps_.end(), [&](auto& a){ return IEqual(a.processName, n); }), apps_.end()); }
void AudioDucker::ClearApps()                    { apps_.clear(); }

const char* AudioDucker::GetPhaseName() const
{
    switch(phase_){
    case Idle: return "空闲";
    case WaitingToDuck: return "等待启动";
    case Attacking: return "闪避中";
    case Ducking: return "已闪避";
    case WaitingToRestore: return "等待恢复";
    case Releasing: return "恢复中";
    default: return "";
    }
}
void AudioDucker::Start()
{
    running_ = true; phase_ = Idle;
    phaseStart_ = std::chrono::steady_clock::now();
    // Init all secondary app targets to 1.0
    for (auto& a : apps_)
        if (!a.isPrimary && volStates_.find(a.processName) == volStates_.end())
            volStates_[a.processName] = VolState{1.0f, 1.0f};
}
void AudioDucker::Stop()
{
    running_ = false; phase_ = Idle;
    // Set all targets to 1.0 for smooth restore
    for (auto& [name, vs] : volStates_) vs.target = 1.0f;
}

// ── GetActiveAudioSessions ─────────────────────
std::vector<std::wstring> AudioDucker::GetActiveAudioSessions()
{
    std::vector<std::wstring> result;
    if (!comOk_ || !pMgr_) return result;

    IAudioSessionEnumerator* pSes = nullptr;
    if (FAILED(pMgr_->GetSessionEnumerator(&pSes))) return result;

    int cnt = 0; pSes->GetCount(&cnt);
    for (int i = 0; i < cnt; i++)
    {
        IAudioSessionControl* pCtrl = nullptr;
        if (FAILED(pSes->GetSession(i, &pCtrl))) continue;
        IAudioSessionControl2* pCtrl2 = nullptr;
        if (FAILED(pCtrl->QueryInterface(IID_IAudioSessionControl2, (void**)&pCtrl2)))
        { pCtrl->Release(); continue; }

        DWORD pid = 0; pCtrl2->GetProcessId(&pid);

        std::wstring name;
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_VM_READ, FALSE, pid);
        if (hProc)
        {
            wchar_t path[MAX_PATH]; DWORD len = MAX_PATH;
            if (QueryFullProcessImageNameW(hProc, 0, path, &len))
            {
                std::wstring full(path);
                auto pos = full.find_last_of(L"\\/");
                name = (pos != std::wstring::npos) ? full.substr(pos + 1) : full;
            }
            CloseHandle(hProc);
        }

        if (!name.empty()
            && name != L"audiodg.exe" && name != L"System" && name != L"Idle"
            && name != L"csrss.exe" && name != L"winlogon.exe"
            && name.find(L"svchost") != 0
            && name.find(L"OpenAudioDucking") == std::wstring::npos)
        {
            if (std::find(result.begin(), result.end(), name) == result.end())
                result.push_back(name);
        }
        pCtrl2->Release(); pCtrl->Release();
    }
    pSes->Release();
    std::sort(result.begin(), result.end());
    return result;
}

// ── Process ───────────────────────────────────────────
void AudioDucker::Process()
{
    using clock = std::chrono::steady_clock;
    auto now = clock::now();
    if (now - lastTick_ < std::chrono::milliseconds(40)) return;
    lastTick_ = now;
    if (!comOk_) return;

    // Init any new secondary apps
    for (auto& a : apps_) {
        if (!a.isPrimary && volStates_.find(a.processName) == volStates_.end())
            volStates_[a.processName] = VolState{1.0f, 1.0f};
    }

    if (!running_) {
        // 不运行时只恢复音量：target 已全部设为 1.0，靠平滑自己回到位
        phase_ = Idle;
        EnumerateSessions();
        return;
    }

    EnumerateSessions();

    bool primaryPlaying = false;
    for (auto& [name, st] : statusMap_)
    {
        for (auto& app : apps_)
            if (app.isPrimary && IsApp(name, app) && st.isPlaying) { primaryPlaying = true; break; }
        if (primaryPlaying) break;
    }

    auto elapsed = [&]() -> float {
        return std::chrono::duration<float, std::milli>(now - phaseStart_).count();
    };

    // ── Phase transitions ──
    if (primaryPlaying && phase_ == Idle)
        { phase_ = duckConfig_.activationDelayMs > 0 ? WaitingToDuck : Attacking; phaseStart_ = now; }
    else if (primaryPlaying && phase_ == WaitingToDuck && elapsed() >= (float)duckConfig_.activationDelayMs)
        { phase_ = Attacking; phaseStart_ = now; }
    else if (primaryPlaying && phase_ == Releasing)
        { phase_ = duckConfig_.activationDelayMs > 0 ? WaitingToDuck : Attacking; phaseStart_ = now; }
    else if (!primaryPlaying && phase_ == Attacking)
        { phase_ = duckConfig_.recoveryDelayMs > 0 ? WaitingToRestore : Releasing; phaseStart_ = now; }
    else if (!primaryPlaying && phase_ == Ducking)
        { phase_ = duckConfig_.recoveryDelayMs > 0 ? WaitingToRestore : Releasing; phaseStart_ = now; }
    else if (!primaryPlaying && phase_ == WaitingToDuck)
        { phase_ = Idle; }
    else if (!primaryPlaying && phase_ == WaitingToRestore && elapsed() >= (float)duckConfig_.recoveryDelayMs)
        { phase_ = Releasing; phaseStart_ = now; }
    else if (primaryPlaying && phase_ == WaitingToRestore)
        { phase_ = duckConfig_.activationDelayMs > 0 ? WaitingToDuck : Attacking; phaseStart_ = now; }

    // ── Set targets ──
    float duckRatio = 1.0f - duckConfig_.duckPercent / 100.0f;
    bool duckPhase = (phase_ == Attacking || phase_ == Ducking || phase_ == WaitingToRestore);
    for (auto& a : apps_)
    {
        if (a.isPrimary) continue;
        auto it = volStates_.find(a.processName);
        if (it == volStates_.end()) continue;
        if (duckPhase)
            it->second.target = duckRatio;
        else if (phase_ == WaitingToDuck && it->second.current < 0.95f)
            it->second.target = duckRatio;  // 从闪避态进来的，保持闪避不跳
        else
            it->second.target = 1.0f;
    }
}

// ═════ Import / Export ═══════════════════════════════
static std::string Ws2Utf8(const std::wstring& ws)
{
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)len, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), &out[0], len, nullptr, nullptr);
    return out;
}
static std::wstring Utf82Ws(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], len);
    return out;
}
static std::string EscJ(const std::string& s)
{ std::string o; o.reserve(s.size()); for (char c:s) { if(c=='"'||c=='\\'){o+='\\';o+=c;} else o+=c; } return o; }
static std::string Trim(const std::string& s)
{ auto st=std::find_if_not(s.begin(),s.end(),::isspace); auto en=std::find_if_not(s.rbegin(),s.rend(),::isspace).base(); return (st<en)?std::string(st,en):""; }
static void WriteArr(std::ofstream& f, const std::string& key, const std::vector<AppConfig>& apps, bool primary)
{
    f << "    \"" << EscJ(key) << "\": [\n"; bool first = true;
    for (auto& a : apps) { if (a.isPrimary!=primary) continue; if(!first) f<<",\n"; first=false; f<<"      \""<<EscJ(Ws2Utf8(a.processName))<<"\""; }
    f << "\n    ]";
}

bool AudioDucker::ExportConfig(const std::vector<AppConfig>& apps, const DuckConfig& cfg, const std::string& path)
{
    std::ofstream f(path); if (!f) return false;
    f << "{\n"; WriteArr(f,"primary",apps,true); f<<",\n"; WriteArr(f,"secondary",apps,false); f<<",\n";
    f<<"    \"activationDelayMs\": " <<cfg.activationDelayMs <<",\n";
    f<<"    \"recoveryDelayMs\": "   <<cfg.recoveryDelayMs   <<",\n";
    f<<"    \"attackDurationMs\": "  <<cfg.attackDurationMs  <<",\n";
    f<<"    \"releaseDurationMs\": " <<cfg.releaseDurationMs <<",\n";
    f<<"    \"thresholdDb\": "       <<cfg.thresholdDb       <<",\n";
    f<<"    \"duckPercent\": "       <<cfg.duckPercent       <<"\n";
    f<<"}\n"; return true;
}

static std::string ReadAll(const std::string& path)
{ std::ifstream f(path); if(!f) return {}; std::stringstream ss; ss<<f.rdbuf(); return ss.str(); }
static std::string ExtractQ(const std::string& line, size_t pos)
{ auto s=line.find('"',pos); if(s==std::string::npos) return {}; auto e=line.find('"',s+1); if(e==std::string::npos) return {}; return line.substr(s+1,e-s-1); }
static float ExtractFloat(const std::string& line, const std::string& key)
{
    auto p=line.find(key); if(p==std::string::npos) return -999;
    p=line.find(':',p+key.size()); if(p==std::string::npos) return -999;
    std::string n; for(size_t i=p+1;i<line.size();++i){ if(std::isdigit((unsigned char)line[i])||line[i]=='-'||line[i]=='.') n+=line[i]; else if(!n.empty()) break; }
    return n.empty()?-999:(float)std::stod(n);
}

bool AudioDucker::ImportConfig(const std::string& path, std::vector<AppConfig>& outApps, DuckConfig& outCfg)
{
    auto content=ReadAll(path); if(content.empty()) return false;
    outApps.clear(); std::stringstream ss(content); std::string line; bool inPri=false,inSec=false;
    while(std::getline(ss,line))
    {
        auto t=Trim(line);
        if(t.find("\"primary\"")!=std::string::npos) { inPri=true; inSec=false; continue; }
        if(t.find("\"secondary\"")!=std::string::npos) { inSec=true; inPri=false; continue; }
        if(t=="["||t=="]") { inPri=inSec=false; continue; }
        if(t.find(']')!=std::string::npos) { inPri=inSec=false; continue; }
        if((inPri||inSec) && !t.empty() && t[0]=='"') { auto n=ExtractQ(t,0); if(!n.empty()){ AppConfig a; a.processName=Utf82Ws(n); a.isPrimary=inPri; outApps.push_back(a); } }
        float fv;
        if((fv=ExtractFloat(t,"activationDelayMs"))>-999) outCfg.activationDelayMs=(int)fv;
        if((fv=ExtractFloat(t,"recoveryDelayMs"))>-999)   outCfg.recoveryDelayMs=(int)fv;
        if((fv=ExtractFloat(t,"attackDurationMs"))>-999)  outCfg.attackDurationMs=(int)fv;
        if((fv=ExtractFloat(t,"releaseDurationMs"))>-999) outCfg.releaseDurationMs=(int)fv;
        if((fv=ExtractFloat(t,"thresholdDb"))>-999)       outCfg.thresholdDb=fv;
        if((fv=ExtractFloat(t,"duckPercent"))>-999)       outCfg.duckPercent=(int)fv;
    }
    return !outApps.empty();
}

bool AudioDucker::IsProcessRunning(const std::wstring& processName)
{
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0); if(snap==INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe={sizeof(pe)}; bool found=false;
    if(Process32FirstW(snap,&pe)) { do{ if(IEqual(pe.szExeFile,processName)){found=true;break;} } while(Process32NextW(snap,&pe)); }
    CloseHandle(snap); return found;
}
