// Standalone harness for brightness_engine.h.
//
// Verifies the two things the Control Center mod depends on:
//   1. every display is discovered and routed to a working transport;
//   2. a fast slider drag stays responsive, i.e. posting hundreds of values
//      results in only as many hardware writes as the bus can absorb.
//
// Restores each display's original brightness before exiting.

#include "brightness_engine.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

void LogLine(const wchar_t* msg) {
    std::wprintf(L"      | %ls\n", msg);
}

const wchar_t* TransportName(brightness::Transport t) {
    switch (t) {
        case brightness::Transport::DdcCi:
            return L"DDC/CI";
        case brightness::Transport::Wmi:
            return L"WMI";
        default:
            return L"none";
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    brightness::Engine engine;
    engine.SetLogger(&LogLine);

    std::wprintf(L"Starting engine...\n");
    engine.Start();

    std::vector<brightness::Display> displays = engine.GetDisplays();
    std::wprintf(L"\n=== %zu display(s) ===\n", displays.size());
    for (size_t i = 0; i < displays.size(); ++i) {
        const brightness::Display& d = displays[i];
        std::wprintf(L"[%zu] %-22ls  transport=%-7ls  now=%3d%%  vcpMax=%lu%ls\n", i,
                     d.name.c_str(), TransportName(d.transport), d.percent,
                     static_cast<unsigned long>(d.vcpMax),
                     d.isPrimary ? L"  (primary)" : L"");
        std::wprintf(L"     gdi=%ls  rect=(%ld,%ld)-(%ld,%ld)\n",
                     d.gdiDeviceName.c_str(), d.rect.left, d.rect.top,
                     d.rect.right, d.rect.bottom);
        std::wprintf(L"     id=%ls\n", d.stableId.c_str());
    }

    std::vector<brightness::Display> controllable;
    for (const brightness::Display& d : displays) {
        if (d.transport != brightness::Transport::None && d.percent >= 0) {
            controllable.push_back(d);
        }
    }

    if (controllable.empty()) {
        std::wprintf(L"\nNo controllable display found. Nothing to test.\n");
        engine.Stop();
        return 1;
    }

    // --- Slider-drag simulation -------------------------------------------
    // A real drag fires value-changed far faster than DDC can answer. Post a
    // dense ramp with no throttling at all and see what reaches the hardware.
    std::wprintf(
        L"\n=== Coalescing test: posting 200 values per display as fast as "
        L"possible ===\n");

    engine.ResetWrites();
    std::chrono::steady_clock::time_point postStart =
        std::chrono::steady_clock::now();

    const int kSteps = 200;
    for (int step = 0; step <= kSteps; ++step) {
        // Sweep down to 40% and back, like a user scrubbing the slider.
        int pct = (step <= kSteps / 2)
                      ? 100 - (step * 60 / (kSteps / 2))
                      : 40 + ((step - kSteps / 2) * 60 / (kSteps / 2));
        for (const brightness::Display& d : controllable) {
            engine.SetPercent(d.stableId, pct);
        }
    }

    long long postMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - postStart)
                           .count();
    std::wprintf(
        L"\nPosted %d values x %zu display(s) in %lld ms "
        L"(this is what the UI thread would block for).\n",
        kSteps + 1, controllable.size(), postMs);

    std::wprintf(L"Draining...\n");
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::wprintf(L"Hardware writes actually issued: %u (of %zu posted)\n",
                 engine.Writes(),
                 static_cast<size_t>(kSteps + 1) * controllable.size());

    // --- Restore ----------------------------------------------------------
    std::wprintf(L"\n=== Restoring original brightness ===\n");
    for (const brightness::Display& d : controllable) {
        std::wprintf(L"  %ls -> %d%%\n", d.name.c_str(), d.percent);
        engine.SetPercent(d.stableId, d.percent);
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));

    engine.Stop();
    std::wprintf(L"\nDone.\n");
    return 0;
}
