#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <string>
#include <vector>
#include <map>
#include <chrono>

struct DuckConfig
{
    int activationDelayMs = 0;     // 延迟多久启动
    int recoveryDelayMs = 0;       // 延迟多久恢复
    int attackDurationMs = 150;    // 开始渐变时长
    int releaseDurationMs = 800;   // 结束渐变时长
    float thresholdDb = -30.0f;    // 触发阈值 dB  (-100~0)
    int duckPercent = 50;          // 闪避量
};

struct AppConfig
{
    std::wstring processName;
    bool isPrimary = false;
};

struct AppStatus
{
    std::wstring processName;
    bool isPlaying = false;
    bool isDucked = false;
    float peakLinear = 0.0f;
    float peakDb = -100.0f;
    float smoothedPeak = 0.0f;  // EMA 平滑用于阈值检测 & VU 动画
};

class AudioDucker
{
public:
    AudioDucker();
    ~AudioDucker();

    void AddApp(const AppConfig& cfg);
    void RemoveApp(const std::wstring& processName);
    void ClearApps();

    const std::vector<AppConfig>& GetApps() const { return apps_; }
    void SetApps(const std::vector<AppConfig>& apps) { apps_ = apps; }

    DuckConfig& GetDuckConfig() { return duckConfig_; }
    const DuckConfig& GetDuckConfig() const { return duckConfig_; }
    void SetDuckConfig(const DuckConfig& cfg) { duckConfig_ = cfg; }

    static bool ExportConfig(const std::vector<AppConfig>& apps,
                             const DuckConfig& cfg, const std::string& path);
    static bool ImportConfig(const std::string& path,
                             std::vector<AppConfig>& outApps, DuckConfig& outCfg);

    static bool IsProcessRunning(const std::wstring& processName);

    void Start();
    void Stop();
    bool IsRunning() const { return running_; }

    void Process();
    const std::map<std::wstring, AppStatus>& GetStatusMap() const { return statusMap_; }
    std::vector<std::wstring> GetActiveAudioSessions();

    // 软渐变：每个应用跟踪实际音量与目标音量
    struct VolState {
        float current = 1.0f;
        float target  = 1.0f;
    };

private:
    void EnumerateSessions();
    std::wstring ProcessNameFromPid(DWORD pid);

    bool InitCOM();
    void CleanupCOM();
    IMMDeviceEnumerator* pEnum_ = nullptr;
    IMMDevice* pDevice_ = nullptr;
    IAudioSessionManager2* pMgr_ = nullptr;
    bool comOk_ = false;

    std::vector<AppConfig> apps_;
    DuckConfig duckConfig_;
    bool running_ = false;

    std::map<std::wstring, AppStatus> statusMap_;
    std::map<std::wstring, VolState> volStates_;
    std::map<std::wstring, float> smoothCache_;  // EMA 平滑峰值跨帧持久

    // 状态机
    enum Phase { Idle, WaitingToDuck, Attacking, Ducking, WaitingToRestore, Releasing };
    Phase phase_ = Idle;
    std::chrono::steady_clock::time_point phaseStart_;
    std::chrono::steady_clock::time_point lastTick_;
};
