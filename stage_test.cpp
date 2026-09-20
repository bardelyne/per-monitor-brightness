// Staged, unbuffered bisect of the engine's dependencies. Single-threaded on
// purpose: isolates whether a fault lives in the COM/WMI/DDC calls or in the
// engine's threading.

#include <windows.h>

#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>

#include <comdef.h>
#include <wbemidl.h>

#include <cstdio>
#include <vector>

#define STEP(...)                     \
    do {                              \
        std::printf("[step] ");       \
        std::printf(__VA_ARGS__);     \
        std::printf("\n");            \
        std::fflush(stdout);          \
    } while (0)

static std::vector<HMONITOR> g_monitors;

static BOOL CALLBACK EnumProc(HMONITOR h, HDC, LPRECT, LPARAM) {
    g_monitors.push_back(h);
    return TRUE;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    STEP("1. printf works");

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    STEP("2. CoInitializeEx = 0x%08lX", (unsigned long)hr);

    hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                              RPC_C_AUTHN_LEVEL_DEFAULT,
                              RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE,
                              nullptr);
    STEP("3. CoInitializeSecurity = 0x%08lX", (unsigned long)hr);

    STEP("4. about to construct _bstr_t");
    {
        _bstr_t ns(L"root\\wmi");
        STEP("5. _bstr_t ok, len=%u", (unsigned)ns.length());
    }

    IWbemLocator* locator = nullptr;
    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, (LPVOID*)&locator);
    STEP("6. CoCreateInstance(WbemLocator) = 0x%08lX  ptr=%p",
         (unsigned long)hr, (void*)locator);
    if (FAILED(hr) || !locator) {
        return 1;
    }

    IWbemServices* svc = nullptr;
    hr = locator->ConnectServer(_bstr_t(L"root\\wmi"), nullptr, nullptr, nullptr,
                                0, nullptr, nullptr, &svc);
    STEP("7. ConnectServer = 0x%08lX  ptr=%p", (unsigned long)hr, (void*)svc);
    if (FAILED(hr) || !svc) {
        return 1;
    }

    hr = CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                           RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                           nullptr, EOAC_NONE);
    STEP("8. CoSetProxyBlanket = 0x%08lX", (unsigned long)hr);

    const wchar_t* queries[] = {L"SELECT * FROM WmiMonitorID",
                                L"SELECT * FROM WmiMonitorBrightness",
                                L"SELECT * FROM WmiMonitorBrightnessMethods"};
    for (const wchar_t* q : queries) {
        IEnumWbemClassObject* e = nullptr;
        hr = svc->ExecQuery(_bstr_t(L"WQL"), _bstr_t(q),
                            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                            nullptr, &e);
        STEP("9. ExecQuery(%ls) = 0x%08lX", q, (unsigned long)hr);
        if (FAILED(hr) || !e) {
            continue;
        }
        int n = 0;
        for (;;) {
            IWbemClassObject* obj = nullptr;
            ULONG got = 0;
            if (e->Next(WBEM_INFINITE, 1, &obj, &got) != S_OK || got != 1) {
                break;
            }
            ++n;
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(obj->Get(L"InstanceName", 0, &v, nullptr, nullptr)) &&
                v.vt == VT_BSTR && v.bstrVal) {
                STEP("     instance: %ls", v.bstrVal);
            }
            VariantClear(&v);

            VariantInit(&v);
            if (SUCCEEDED(obj->Get(L"__RELPATH", 0, &v, nullptr, nullptr)) &&
                v.vt == VT_BSTR && v.bstrVal) {
                STEP("     relpath : %ls", v.bstrVal);
            }
            VariantClear(&v);
            obj->Release();
        }
        STEP("   -> %d instance(s)", n);
        e->Release();
    }

    STEP("10. EnumDisplayMonitors");
    EnumDisplayMonitors(nullptr, nullptr, &EnumProc, 0);
    STEP("11. got %zu HMONITOR(s)", g_monitors.size());

    for (size_t i = 0; i < g_monitors.size(); ++i) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        BOOL ok = GetMonitorInfoW(g_monitors[i], &mi);
        STEP("12.%zu GetMonitorInfoW ok=%d dev=%ls", i, ok, mi.szDevice);

        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        ok = EnumDisplayDevicesW(mi.szDevice, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME);
        STEP("13.%zu EnumDisplayDevicesW(iface) ok=%d id=%ls", i, ok, dd.DeviceID);

        DWORD count = 0;
        ok = GetNumberOfPhysicalMonitorsFromHMONITOR(g_monitors[i], &count);
        STEP("14.%zu physical count ok=%d n=%lu", i, ok, (unsigned long)count);
        if (!ok || count == 0) {
            continue;
        }

        std::vector<PHYSICAL_MONITOR> physical(count);
        ok = GetPhysicalMonitorsFromHMONITOR(g_monitors[i], count, physical.data());
        STEP("15.%zu GetPhysicalMonitors ok=%d", i, ok);
        if (!ok) {
            continue;
        }

        MC_VCP_CODE_TYPE type{};
        DWORD cur = 0, max = 0;
        ok = GetVCPFeatureAndVCPFeatureReply(physical[0].hPhysicalMonitor, 0x10,
                                             &type, &cur, &max);
        STEP("16.%zu VCP 0x10 ok=%d cur=%lu max=%lu err=%lu", i, ok,
             (unsigned long)cur, (unsigned long)max,
             (unsigned long)GetLastError());

        DestroyPhysicalMonitors(count, physical.data());
        STEP("17.%zu destroyed", i);
    }

    svc->Release();
    locator->Release();
    CoUninitialize();
    STEP("18. done cleanly");
    return 0;
}
