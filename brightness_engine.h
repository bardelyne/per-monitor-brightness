// Per-monitor brightness engine.
//
// Two transports, picked per display at enumeration time:
//   * DDC/CI  (external monitors) -- low-level VCP 0x10 over I2C. ~56 ms/write.
//   * WMI     (internal laptop panels) -- WmiMonitorBrightnessMethods.
//
// Every hardware call happens on one worker thread. Callers post a target
// percentage and return immediately; the worker coalesces, so a slider drag
// that posts 200 values only puts as many on the wire as the bus can carry.
//
// Designed to be #included by the Windhawk mod as-is: no globals, no console,
// no dependency on anything outside the Win32/COM SDK.

#pragma once

#include <windows.h>

#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>

#include <comdef.h>
#include <wbemidl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cwctype>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace brightness {

// Minimum gap between DDC/CI writes. Long enough that the shell's UI thread
// gets the bus back and a slider tracks the pointer; short enough that an
// external monitor still visibly follows a drag.
inline constexpr std::chrono::milliseconds kDdcCooldown{140};

// What other displays do when the internal panel's brightness changes on its
// own -- function keys, mostly.
enum class FollowMode {
    Off,       // leave them alone
    Match,     // set them to the same percentage
    Relative,  // shift them by the same delta, preserving their offset
};

enum class Transport {
    None,   // display found but no way to control it
    DdcCi,  // external, VCP 0x10
    Wmi,    // internal panel
};

struct Display {
    // Stable across reboots, hotplug and monitor reordering: derived from the
    // EDID device instance path. HMONITOR is not stable, so it is never a key.
    std::wstring stableId;
    std::wstring name;           // "LS27F32xG", "Built-in display"
    std::wstring gdiDeviceName;  // "\\.\DISPLAY5"
    RECT rect{};
    bool isPrimary = false;
    Transport transport = Transport::None;
    int percent = -1;  // last known, -1 if never read

    // DdcCi
    HANDLE hPhysical = nullptr;
    DWORD vcpMax = 100;  // raw scale; a Samsung G32 reports 50, not 100

    // Wmi
    std::wstring wmiPath;  // __RELPATH of the WmiMonitorBrightnessMethods instance
};

namespace detail {

// Reports an exception that escaped a thread body. Deliberately not Wh_Log:
// this header is testable outside Windhawk, so it uses the debugger channel
// the mod's own logging ends up on anyway.
inline void ReportEscaped(const wchar_t* where, const wchar_t* what) {
    wchar_t line[1024];
    wsprintfW(line, L"[per-monitor-brightness] %ls threw: %ls", where, what);
    OutputDebugStringW(line);
}

// Runs a thread body so that nothing can escape it.
//
// This is not defensive tidiness. An exception leaving a thread procedure
// calls std::terminate(), which aborts the *host* process -- and everything
// below talks to COM, WMI and I2C, all of which fail in ways that throw when
// the shell is still starting up and those services are not ready yet. An
// unguarded throw here takes ShellHost down, and it restarts into the same
// throw, so the shell never comes back.
template <class Fn>
void RunGuarded(const wchar_t* where, Fn&& fn) noexcept {
    try {
        fn();
    } catch (const _com_error& e) {
        wchar_t buf[512];
        wsprintfW(buf, L"_com_error %08X (%ls)", static_cast<unsigned>(e.Error()),
                  e.ErrorMessage() ? e.ErrorMessage() : L"?");
        ReportEscaped(where, buf);
    } catch (const std::exception& e) {
        wchar_t buf[512];
        MultiByteToWideChar(CP_ACP, 0, e.what(), -1, buf, 512);
        ReportEscaped(where, buf);
    } catch (...) {
        ReportEscaped(where, L"unknown exception");
    }
}


inline std::wstring GetStrProp(IWbemClassObject* obj, const wchar_t* name) {
    VARIANT v;
    VariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(obj->Get(name, 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR &&
        v.bstrVal) {
        out = v.bstrVal;
    }
    VariantClear(&v);
    return out;
}

inline int GetIntProp(IWbemClassObject* obj, const wchar_t* name) {
    VARIANT v;
    VariantInit(&v);
    int out = -1;
    if (SUCCEEDED(obj->Get(name, 0, &v, nullptr, nullptr))) {
        switch (v.vt) {
            case VT_I4:
                out = v.lVal;
                break;
            case VT_UI1:
                out = v.bVal;
                break;
            case VT_I2:
                out = v.iVal;
                break;
            case VT_UI4:
                out = static_cast<int>(v.ulVal);
                break;
            default:
                break;
        }
    }
    VariantClear(&v);
    return out;
}

// WMI exposes uint16[] (e.g. UserFriendlyName) as a SAFEARRAY of VT_I4.
inline std::wstring GetU16ArrayProp(IWbemClassObject* obj, const wchar_t* name) {
    VARIANT v;
    VariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(obj->Get(name, 0, &v, nullptr, nullptr)) && (v.vt & VT_ARRAY) &&
        v.parray) {
        SAFEARRAY* sa = v.parray;
        VARTYPE elem = static_cast<VARTYPE>(v.vt & VT_TYPEMASK);
        LONG lb = 0, ub = -1;
        SafeArrayGetLBound(sa, 1, &lb);
        SafeArrayGetUBound(sa, 1, &ub);
        for (LONG i = lb; i <= ub; ++i) {
            LONG ch = 0;
            if (elem == VT_UI1) {
                BYTE b = 0;
                if (FAILED(SafeArrayGetElement(sa, &i, &b))) {
                    break;
                }
                ch = b;
            } else {
                if (FAILED(SafeArrayGetElement(sa, &i, &ch))) {
                    break;
                }
            }
            if (ch == 0) {
                break;
            }
            out.push_back(static_cast<wchar_t>(ch));
        }
    }
    VariantClear(&v);
    return out;
}

inline std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

// "\\?\DISPLAY#SAM78C9#5&31ef9774&0&UID4352#{guid}"
//   -> "DISPLAY\SAM78C9\5&31ef9774&0&UID4352"
// which is the WMI InstanceName minus its "_0" suffix.
inline std::wstring DeviceInterfaceToInstanceId(std::wstring s) {
    if (s.rfind(L"\\\\?\\", 0) == 0) {
        s.erase(0, 4);
    }
    size_t guid = s.rfind(L'#');
    if (guid != std::wstring::npos) {
        s.erase(guid);
    }
    std::replace(s.begin(), s.end(), L'#', L'\\');
    return s;
}

}  // namespace detail

// Thin wrapper over root\wmi. Lives entirely on the worker thread, so the
// interface pointers never cross an apartment.
class WmiSession {
   public:
    WmiSession() = default;
    WmiSession(const WmiSession&) = delete;
    WmiSession& operator=(const WmiSession&) = delete;

    ~WmiSession() {
        if (setBrightnessInDef_) {
            setBrightnessInDef_->Release();
        }
        if (brightnessClass_) {
            brightnessClass_->Release();
        }
        if (services_) {
            services_->Release();
        }
        if (locator_) {
            locator_->Release();
        }
    }

    bool Init() {
        HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                      reinterpret_cast<LPVOID*>(&locator_));
        if (FAILED(hr)) {
            return false;
        }

        hr = locator_->ConnectServer(_bstr_t(L"root\\wmi"), nullptr, nullptr,
                                     nullptr, 0, nullptr, nullptr, &services_);
        if (FAILED(hr)) {
            return false;
        }

        CoSetProxyBlanket(services_, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                          nullptr, RPC_C_AUTHN_LEVEL_CALL,
                          RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        return true;
    }

    bool Ok() const { return services_ != nullptr; }

    IWbemServices* Services() const { return services_; }

    template <class Fn>
    void ForEach(const wchar_t* wql, Fn&& fn) {
        if (!services_) {
            return;
        }
        IEnumWbemClassObject* e = nullptr;
        HRESULT hr = services_->ExecQuery(
            _bstr_t(L"WQL"), _bstr_t(wql),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &e);
        if (FAILED(hr) || !e) {
            return;
        }
        for (;;) {
            IWbemClassObject* obj = nullptr;
            ULONG got = 0;
            if (e->Next(WBEM_INFINITE, 1, &obj, &got) != S_OK || got != 1) {
                break;
            }
            fn(obj);
            obj->Release();
        }
        e->Release();
    }

    bool SetBrightness(const std::wstring& objectPath, int percent) {
        if (!services_ || objectPath.empty()) {
            return false;
        }
        if (!brightnessClass_) {
            if (FAILED(services_->GetObject(
                    _bstr_t(L"WmiMonitorBrightnessMethods"), 0, nullptr,
                    &brightnessClass_, nullptr))) {
                return false;
            }
            if (FAILED(brightnessClass_->GetMethod(
                    L"WmiSetBrightness", 0, &setBrightnessInDef_, nullptr))) {
                return false;
            }
        }

        IWbemClassObject* in = nullptr;
        if (FAILED(setBrightnessInDef_->SpawnInstance(0, &in)) || !in) {
            return false;
        }

        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = 0;  // Timeout: apply immediately, never revert
        in->Put(L"Timeout", 0, &v, 0);
        VariantClear(&v);

        VariantInit(&v);
        v.vt = VT_UI1;
        v.bVal = static_cast<BYTE>(std::clamp(percent, 0, 100));
        in->Put(L"Brightness", 0, &v, 0);
        VariantClear(&v);

        HRESULT hr = services_->ExecMethod(
            _bstr_t(objectPath.c_str()), _bstr_t(L"WmiSetBrightness"), 0,
            nullptr, in, nullptr, nullptr);
        in->Release();
        return SUCCEEDED(hr);
    }

   private:
    IWbemLocator* locator_ = nullptr;
    IWbemServices* services_ = nullptr;
    IWbemClassObject* brightnessClass_ = nullptr;
    IWbemClassObject* setBrightnessInDef_ = nullptr;
};

class Engine {
   public:
    using LogFn = void (*)(const wchar_t*);

    Engine() = default;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    ~Engine() { Stop(); }

    void SetLogger(LogFn fn) { log_ = fn; }

    // Per-write logging is invaluable while debugging and pure noise in daily
    // use, so it is off unless asked for.
    void SetVerboseWrites(bool verbose) { verboseWrites_.store(verbose); }

    void SetFollowMode(FollowMode mode) {
        std::lock_guard<std::mutex> lock(mutex_);
        followMode_ = mode;
    }

    void Start() {
        if (worker_.joinable()) {
            return;
        }
        quit_ = false;
        worker_ = std::thread(
            [this] { detail::RunGuarded(L"WorkerMain", [this] { WorkerMain(); }); });

        // Blocking on purpose, and safe because of when this is called.
        //
        // The first enumeration talks to WMI and does an I2C round trip per
        // external monitor, so it is not fast. That is fine here: the mod does
        // not call Start() from Wh_ModInit -- it waits until the host is up
        // (see StartEngineIfNeeded in the mod) precisely because touching COM
        // before then aborts the shell. By the time we get here the host has
        // finished starting and nothing of its is being delayed.
        //
        // Still bounded, so a wedged WMI connection cannot hang the caller.
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait_for(lock, std::chrono::seconds(5),
                            [this] { return enumerated_ || quit_; });
        }
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        work_.notify_all();
        ready_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        if (eventThread_.joinable()) {
            eventThread_.join();
        }
    }

    // Snapshot for the UI. Ordered left-to-right by desktop position, so the
    // slider order matches how the monitors physically sit on the desk.
    std::vector<Display> GetDisplays() {
        std::lock_guard<std::mutex> lock(mutex_);
        return displays_;
    }

    // Non-blocking. Repeated calls for the same display collapse into a single
    // hardware write, so a slider drag can never outrun the I2C bus.
    void SetPercent(const std::wstring& stableId, int percent) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Stamped on request, not on completion: a drag keeps refreshing
            // this, so the whole drag plus its trailing echoes stay covered.
            lastRequest_[stableId] = std::chrono::steady_clock::now();
            pending_[stableId] = std::clamp(percent, 0, 100);
            // Reflect optimistically so the UI stays glued to the thumb.
            for (auto& d : displays_) {
                if (d.stableId == stableId) {
                    d.percent = std::clamp(percent, 0, 100);
                }
            }
        }
        work_.notify_all();
    }

    void RequestRescan() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rescan_ = true;
        }
        work_.notify_all();
    }

    // Re-read what the hardware actually reports, for when brightness was
    // changed behind our back -- laptop function keys, the monitor's own OSD,
    // or another application.
    void RequestRefresh() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            refresh_ = true;
        }
        work_.notify_all();
    }

    // Called on the worker thread once the display list changed (structural =
    // true) or once values were re-read (false). The callee is responsible for
    // marshalling to whatever thread it needs.
    void SetOnChanged(std::function<void(bool)> fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        onChanged_ = std::move(fn);
    }

    // Diagnostics for the standalone harness.
    unsigned Writes() const { return writes_.load(); }
    void ResetWrites() { writes_.store(0); }

   private:
    static constexpr BYTE kVcpLuminance = 0x10;

    void Log(const wchar_t* fmt, ...) {
        if (!log_) {
            return;
        }
        wchar_t buf[512];
        va_list args;
        va_start(args, fmt);
        vswprintf(buf, sizeof(buf) / sizeof(buf[0]), fmt, args);
        va_end(args);
        log_(buf);
    }

    void WorkerMain() {
        HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        // Deliberately no CoInitializeSecurity: it is process-wide and this
        // thread starts from Wh_ModInit, before the host's own startup code
        // runs, so we would likely win the race and impose our settings on the
        // whole process. CoSetProxyBlanket on the IWbemServices proxy is what
        // actually governs our WMI calls.

        // Created here, not as a plain member: these interface pointers belong
        // to this thread's apartment and must not outlive it. Releasing them
        // from ~Engine() on another thread, after the CoUninitialize() below,
        // is a crash.
        wmi_ = std::make_unique<WmiSession>();
        wmi_->Init();

        Rescan();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            enumerated_ = true;
        }
        ready_.notify_all();
        NotifyChanged(true);

        // Only worth a thread if something here reports brightness events;
        // DDC/CI has no equivalent, so this is the internal panel only.
        bool haveWmiPanel = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& d : displays_) {
                if (d.transport == Transport::Wmi) {
                    haveWmiPanel = true;
                    break;
                }
            }
        }
        if (haveWmiPanel) {
            eventThread_ = std::thread([this] {
                detail::RunGuarded(L"EventThreadMain", [this] { EventThreadMain(); });
            });
        }

        for (;;) {
            std::map<std::wstring, int> batch;
            bool doRescan = false;
            bool doRefresh = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                work_.wait(lock, [this] {
                    return quit_ || rescan_ || refresh_ || !pending_.empty();
                });
                if (quit_) {
                    break;
                }
                doRescan = std::exchange(rescan_, false);
                batch.swap(pending_);
                // Never read back while writes are still queued: the value in
                // flight has not reached the panel yet, so reading now would
                // yank the slider backwards under the user's finger. refresh_
                // stays set and we come back to it once the queue drains.
                doRefresh = batch.empty() && refresh_;
                if (doRefresh) {
                    refresh_ = false;
                }
            }

            if (doRescan) {
                Rescan();
                NotifyChanged(true);
            }

            // Each write blocks for tens of ms. Anything the UI posts while
            // we are on the wire lands in pending_ and overwrites its
            // predecessor, so we always resume with the newest value.
            bool wroteDdc = false;
            for (const auto& entry : batch) {
                if (Apply(entry.first, entry.second) == Transport::DdcCi) {
                    wroteDdc = true;
                }
            }

            // Breathing room after an I2C write, and it is not politeness.
            //
            // A DDC/CI transaction serialises against the display driver, and
            // while one is on the wire the shell's UI thread stalls. Writing
            // back-to-back for the whole of a slider drag -- which is what
            // coalescing alone will happily do, one write every ~60 ms --
            // starves the slider of pointer input, so the thumb falls behind
            // the cursor and stops short. Drag quickly to the right-hand end
            // and you land somewhere in the sixties, which reads as the value
            // "drifting" on its own.
            //
            // So cap the write rate and let the queue coalesce in the gap. The
            // newest value always wins, so the value the user let go of is
            // still the one that lands, just up to kDdcCooldown later.
            if (wroteDdc) {
                std::unique_lock<std::mutex> lock(mutex_);
                work_.wait_for(lock, kDdcCooldown, [this] { return quit_; });
            }

            if (doRefresh) {
                RefreshValues();
                NotifyChanged(false);
            }
        }

        ReleasePhysicalMonitors();

        // Tear down the WMI interfaces on the thread that created them, while
        // the apartment is still alive.
        wmi_.reset();

        if (SUCCEEDED(comHr)) {
            CoUninitialize();
        }
    }

    void NotifyChanged(bool structural) {
        std::function<void(bool)> fn;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fn = onChanged_;
        }
        if (!fn) {
            return;
        }
        try {
            fn(structural);
        } catch (...) {
            // Must never escape: this runs on a worker thread, and an
            // unhandled exception there is std::terminate -> the host aborts.
            Log(L"change callback threw; ignored");
        }
    }

    // Re-reads the level each display actually reports. Unlike Rescan this
    // keeps the existing DDC handles and display list, so it costs one I2C
    // round trip per external monitor and nothing structural changes.
    void RefreshValues() {
        struct Target {
            std::wstring id;
            Transport transport;
            HANDLE hPhysical;
            std::wstring wmiKey;
        };

        std::vector<Target> targets;
        bool needWmi = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& d : displays_) {
                targets.push_back({d.stableId, d.transport, d.hPhysical,
                                   detail::ToLower(d.stableId)});
                if (d.transport == Transport::Wmi) {
                    needWmi = true;
                }
            }
        }

        std::map<std::wstring, WmiFacts> facts;
        if (needWmi) {
            facts = CollectWmiFacts();
        }

        std::vector<std::pair<std::wstring, int>> updates;
        for (const Target& target : targets) {
            int percent = -1;
            switch (target.transport) {
                case Transport::DdcCi: {
                    MC_VCP_CODE_TYPE type{};
                    DWORD current = 0, maximum = 0;
                    if (GetVCPFeatureAndVCPFeatureReply(target.hPhysical,
                                                        kVcpLuminance, &type,
                                                        &current, &maximum) &&
                        maximum > 0) {
                        percent = static_cast<int>((current * 100 + maximum / 2) /
                                                   maximum);
                    }
                    break;
                }
                case Transport::Wmi: {
                    auto it = facts.find(target.wmiKey);
                    if (it != facts.end()) {
                        percent = it->second.currentPercent;
                    }
                    break;
                }
                case Transport::None:
                    break;
            }
            if (percent >= 0) {
                updates.emplace_back(target.id, percent);
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& update : updates) {
                for (auto& d : displays_) {
                    if (d.stableId == update.first) {
                        d.percent = update.second;
                        break;
                    }
                }
            }
        }

        Log(L"refreshed %zu display value(s)", updates.size());
    }

    // Windows raises WmiMonitorBrightnessEvent whenever the internal panel's
    // brightness changes -- function keys, power policy, anything. This is how
    // the stock slider stays live, and it beats polling. External monitors have
    // no counterpart: DDC/CI cannot report anything the host did not ask for.
    void EventThreadMain() {
        HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        {
            WmiSession session;
            if (!session.Init() || !session.Services()) {
                Log(L"brightness events: no WMI connection");
            } else {
                IEnumWbemClassObject* events = nullptr;
                HRESULT hr = session.Services()->ExecNotificationQuery(
                    _bstr_t(L"WQL"),
                    _bstr_t(L"SELECT * FROM WmiMonitorBrightnessEvent"),
                    WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                    nullptr, &events);

                if (FAILED(hr) || !events) {
                    Log(L"brightness events unavailable: %08X",
                        static_cast<unsigned>(hr));
                } else {
                    Log(L"listening for brightness events");
                    for (;;) {
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            if (quit_) {
                                break;
                            }
                        }

                        IWbemClassObject* obj = nullptr;
                        ULONG got = 0;
                        // Short timeout so Stop() is not kept waiting.
                        HRESULT next = events->Next(500, 1, &obj, &got);
                        if (next == WBEM_S_TIMEDOUT || got != 1 || !obj) {
                            continue;
                        }
                        if (next != S_OK) {
                            break;
                        }

                        std::wstring instance =
                            detail::GetStrProp(obj, L"InstanceName");
                        int level = detail::GetIntProp(obj, L"Brightness");
                        obj->Release();

                        if (!instance.empty() && level >= 0) {
                            OnBrightnessEvent(instance, level);
                        }
                    }
                    events->Release();
                }
            }
        }  // session released here, inside its own apartment

        if (SUCCEEDED(comHr)) {
            CoUninitialize();
        }
    }

    void OnBrightnessEvent(const std::wstring& instanceName, int percent) {
        std::wstring key = instanceName;
        if (key.size() > 2 && key.compare(key.size() - 2, 2, L"_0") == 0) {
            key.erase(key.size() - 2);
        }
        key = detail::ToLower(key);

        std::wstring changedId;
        std::vector<std::pair<std::wstring, int>> follow;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            Display* target = nullptr;
            for (auto& d : displays_) {
                if (detail::ToLower(d.stableId) == key) {
                    target = &d;
                    break;
                }
            }
            if (!target || target->percent == percent) {
                return;
            }

            // Our own writes raise this event too, and matching on the value
            // is not enough: coalescing means the event for a value we wrote
            // can arrive after we have already written a newer one, so the
            // values disagree and the echo looks like an external change.
            // Following would then compute a delta against the wrong baseline
            // and shove the other monitors around at random.
            //
            // So the test is per display and purely temporal. This does not
            // swallow the keypresses we want to follow: those change the
            // internal panel, which we never write to in response -- following
            // only ever writes to the *other* displays.
            auto requested = lastRequest_.find(target->stableId);
            if (requested != lastRequest_.end() &&
                std::chrono::steady_clock::now() - requested->second <
                    std::chrono::milliseconds(1500)) {
                return;
            }

            int previous = target->percent;
            changedId = target->stableId;
            target->percent = percent;

            if (followMode_ != FollowMode::Off) {
                int delta = (previous >= 0) ? percent - previous : 0;
                for (auto& d : displays_) {
                    if (d.stableId == changedId ||
                        d.transport == Transport::None) {
                        continue;
                    }
                    int base = (d.percent < 0) ? percent : d.percent;
                    int want = (followMode_ == FollowMode::Match)
                                   ? percent
                                   : std::clamp(base + delta, 0, 100);
                    if (want != d.percent) {
                        follow.emplace_back(d.stableId, want);
                    }
                }
            }
        }

        Log(L"brightness event: %ls is now %d%%", instanceName.c_str(), percent);

        // Outside the lock: SetPercent takes it.
        for (const auto& entry : follow) {
            SetPercent(entry.first, entry.second);
        }

        NotifyChanged(false);
    }

    void ReleasePhysicalMonitors() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& d : displays_) {
            if (d.hPhysical) {
                PHYSICAL_MONITOR pm{};
                pm.hPhysicalMonitor = d.hPhysical;
                DestroyPhysicalMonitors(1, &pm);
                d.hPhysical = nullptr;
            }
        }
    }

    struct WmiFacts {
        std::wstring name;
        std::wstring methodsPath;
        int currentPercent = -1;
        bool hasBrightnessMethods = false;
    };

    std::map<std::wstring, WmiFacts> CollectWmiFacts() {
        std::map<std::wstring, WmiFacts> facts;
        if (!wmi_) {
            return facts;
        }

        auto key = [](std::wstring instanceName) {
            // "DISPLAY\BOE0AAE\4&..._0" -> "display\boe0aae\4&..."
            if (instanceName.size() > 2 &&
                instanceName.compare(instanceName.size() - 2, 2, L"_0") == 0) {
                instanceName.erase(instanceName.size() - 2);
            }
            return detail::ToLower(instanceName);
        };

        wmi_->ForEach(L"SELECT * FROM WmiMonitorID", [&](IWbemClassObject* o) {
            std::wstring inst = detail::GetStrProp(o, L"InstanceName");
            if (!inst.empty()) {
                facts[key(inst)].name =
                    detail::GetU16ArrayProp(o, L"UserFriendlyName");
            }
        });

        wmi_->ForEach(L"SELECT * FROM WmiMonitorBrightness",
                     [&](IWbemClassObject* o) {
                         std::wstring inst =
                             detail::GetStrProp(o, L"InstanceName");
                         if (!inst.empty()) {
                             facts[key(inst)].currentPercent =
                                 detail::GetIntProp(o, L"CurrentBrightness");
                         }
                     });

        wmi_->ForEach(L"SELECT * FROM WmiMonitorBrightnessMethods",
                     [&](IWbemClassObject* o) {
                         std::wstring inst =
                             detail::GetStrProp(o, L"InstanceName");
                         if (!inst.empty()) {
                             WmiFacts& f = facts[key(inst)];
                             f.hasBrightnessMethods = true;
                             f.methodsPath = detail::GetStrProp(o, L"__RELPATH");
                         }
                     });

        return facts;
    }

    static BOOL CALLBACK EnumProc(HMONITOR h, HDC, LPRECT, LPARAM data) {
        reinterpret_cast<std::vector<HMONITOR>*>(data)->push_back(h);
        return TRUE;
    }

    void Rescan() {
        ReleasePhysicalMonitors();

        std::vector<HMONITOR> handles;
        EnumDisplayMonitors(nullptr, nullptr, &Engine::EnumProc,
                            reinterpret_cast<LPARAM>(&handles));

        std::map<std::wstring, WmiFacts> facts = CollectWmiFacts();
        std::vector<Display> found;

        for (HMONITOR h : handles) {
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(h, &mi)) {
                continue;
            }

            Display d;
            d.gdiDeviceName = mi.szDevice;
            d.rect = mi.rcMonitor;
            d.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

            DISPLAY_DEVICEW dd{};
            dd.cb = sizeof(dd);
            if (EnumDisplayDevicesW(mi.szDevice, 0, &dd,
                                    EDD_GET_DEVICE_INTERFACE_NAME)) {
                d.stableId = detail::DeviceInterfaceToInstanceId(dd.DeviceID);
            }
            if (d.stableId.empty()) {
                d.stableId = mi.szDevice;  // last resort, not hotplug-stable
            }

            DISPLAY_DEVICEW ddName{};
            ddName.cb = sizeof(ddName);
            std::wstring gdiName;
            if (EnumDisplayDevicesW(mi.szDevice, 0, &ddName, 0)) {
                gdiName = ddName.DeviceString;
            }

            std::map<std::wstring, WmiFacts>::const_iterator it =
                facts.find(detail::ToLower(d.stableId));
            const WmiFacts* f = (it != facts.end()) ? &it->second : nullptr;

            if (f && !f->name.empty()) {
                d.name = f->name;
            }

            // DDC/CI first: it is the only option for external panels, and on
            // a laptop it fails fast (ERROR_GEN_FAILURE) for the internal one.
            if (TryAttachDdcCi(h, &d)) {
                if (d.name.empty()) {
                    d.name = gdiName.empty() ? L"External display" : gdiName;
                }
            } else if (f && f->hasBrightnessMethods) {
                d.transport = Transport::Wmi;
                d.wmiPath = f->methodsPath;
                d.percent = f->currentPercent;
                if (d.name.empty()) {
                    d.name = L"Built-in display";
                }
            } else {
                d.transport = Transport::None;
                if (d.name.empty()) {
                    d.name = gdiName.empty() ? L"Display" : gdiName;
                }
            }

            found.push_back(std::move(d));
        }

        // Left-to-right, so slider order matches the physical layout.
        std::sort(found.begin(), found.end(),
                  [](const Display& a, const Display& b) {
                      if (a.rect.left != b.rect.left) {
                          return a.rect.left < b.rect.left;
                      }
                      return a.rect.top < b.rect.top;
                  });

        {
            std::lock_guard<std::mutex> lock(mutex_);
            displays_ = std::move(found);
        }
    }

    // Deliberately does NOT call GetMonitorCapabilities: plenty of monitors
    // (the Samsung G32 among them) fail it with 0xC0262C07 while answering raw
    // VCP reads and writes perfectly well.
    bool TryAttachDdcCi(HMONITOR h, Display* d) {
        DWORD count = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(h, &count) || count == 0) {
            return false;
        }

        std::vector<PHYSICAL_MONITOR> physical(count);
        if (!GetPhysicalMonitorsFromHMONITOR(h, count, physical.data())) {
            return false;
        }

        // A single HMONITOR maps to more than one physical monitor only in
        // clone/daisy-chain setups; the first one that answers wins.
        bool attached = false;
        for (DWORD i = 0; i < count; ++i) {
            MC_VCP_CODE_TYPE type{};
            DWORD current = 0, maximum = 0;
            if (!attached &&
                GetVCPFeatureAndVCPFeatureReply(physical[i].hPhysicalMonitor,
                                                kVcpLuminance, &type, &current,
                                                &maximum) &&
                maximum > 0) {
                d->transport = Transport::DdcCi;
                d->hPhysical = physical[i].hPhysicalMonitor;
                d->vcpMax = maximum;
                d->percent =
                    static_cast<int>((current * 100 + maximum / 2) / maximum);
                attached = true;
                continue;  // keep this handle alive
            }
            DestroyPhysicalMonitors(1, &physical[i]);
        }

        return attached;
    }

    Transport Apply(const std::wstring& stableId, int percent) {
        HANDLE hPhysical = nullptr;
        DWORD vcpMax = 100;
        Transport transport = Transport::None;
        std::wstring wmiPath;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& d : displays_) {
                if (d.stableId == stableId) {
                    transport = d.transport;
                    hPhysical = d.hPhysical;
                    vcpMax = d.vcpMax;
                    wmiPath = d.wmiPath;
                    break;
                }
            }
        }

        std::chrono::steady_clock::time_point start =
            std::chrono::steady_clock::now();
        bool ok = false;

        switch (transport) {
            case Transport::DdcCi: {
                // Map the percentage onto the monitor's own scale, which is
                // often not 0-100.
                DWORD raw = static_cast<DWORD>(
                    (static_cast<DWORD>(percent) * vcpMax + 50) / 100);
                ok = SetVCPFeature(hPhysical, kVcpLuminance, raw) != FALSE;
                break;
            }
            case Transport::Wmi:
                ok = wmi_ && wmi_->SetBrightness(wmiPath, percent);
                break;
            case Transport::None:
                break;
        }

        long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
        writes_.fetch_add(1);
        if (verboseWrites_.load()) {
            Log(L"apply %ls -> %d%% ok=%d (%lld ms)", stableId.c_str(), percent,
                ok ? 1 : 0, ms);
        }
        return transport;
    }

    std::thread worker_;
    std::thread eventThread_;
    std::map<std::wstring, std::chrono::steady_clock::time_point> lastRequest_;
    FollowMode followMode_ = FollowMode::Off;
    std::atomic<bool> verboseWrites_{false};
    std::mutex mutex_;
    std::condition_variable work_;
    std::condition_variable ready_;
    std::map<std::wstring, int> pending_;
    std::vector<Display> displays_;
    std::unique_ptr<WmiSession> wmi_;
    LogFn log_ = nullptr;
    std::atomic<unsigned> writes_{0};
    std::function<void(bool)> onChanged_;
    bool quit_ = false;
    bool rescan_ = false;
    bool refresh_ = false;
    bool enumerated_ = false;
};

}  // namespace brightness
