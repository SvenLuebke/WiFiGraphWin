// WifiGraphWin.cpp - WiFi monitor with graphs for Windows 10/11
//
// Once per second, for every WiFi adapter, it records:
//   * negotiated link rate  TX / RX   (Mbps)           - Native WiFi API (wlanapi)
//   * measured throughput   upload / download (KB/s or MB/s) - adapter byte counters (iphlpapi)
//   * signal strength       (dBm, plus quality in %)   - Native WiFi API
// and draws them as three stacked live graphs. All samples are appended to
// WifiGraphWin_log.csv in the working directory.
//
// Build (MSVC, "x64 Native Tools Command Prompt"):
//   cl /EHsc /O2 /DUNICODE /D_UNICODE WifiGraphWin.cpp /link /SUBSYSTEM:WINDOWS ^
//      user32.lib gdi32.lib wlanapi.lib ole32.lib iphlpapi.lib
//
// Build (MinGW-w64):
//   g++ -O2 -municode -mwindows WifiGraphWin.cpp -o WifiGraphWin.exe ^
//       -lwlanapi -lgdi32 -luser32 -lole32 -liphlpapi -static
//
// Keys:  1 = 1 min   2 = 5 min   3 = 15 min   4 = 60 min   C = clear history

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <wlanapi.h>
#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <deque>
#include <map>
#include <string>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "wlanapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif

// ----------------------------------------------------------------- data ----
struct Sample {
    double timeSec = 0;        // seconds since program start
    double txMbps = 0, rxMbps = 0;   // link rate, Mbps
    double upBps = 0, downBps = 0;   // measured throughput, bytes/s
    bool   hasThroughput = false;    // throughput valid (needs a previous counter reading)
    double rssiDbm = 0;        // dBm
    bool   rssiEstimated = false;    // true if estimated from signal quality
    int    qualityPercent = 0; // 0..100 %
    bool   connected = false;
};

struct Series {
    std::wstring name;         // adapter description
    std::wstring ssid;         // last known SSID
    COLORREF     color = 0;
    std::deque<Sample> samples;
    // byte-counter state for throughput calculation
    bool     hasPrevCounters = false;
    ULONG64  prevInBytes = 0, prevOutBytes = 0;
    double   prevTimeSec = 0;
};

struct App {
    HANDLE   wlanHandle = nullptr;
    LARGE_INTEGER qpcFrequency{}, qpcStart{};
    double   visibleSpanSec = 300.0;         // visible window in seconds
    std::map<std::wstring, Series> series;
    std::wstring status;                     // shown on errors / no adapter
    FILE*    logFile = nullptr;
    HFONT    font = nullptr, fontBold = nullptr;
};

static const double kHistory = 3600.0;       // seconds of history kept

static const COLORREF kPalette[] = {
    RGB(80, 200, 120), RGB(90, 160, 255), RGB(255, 170, 60),
    RGB(230, 100, 200), RGB(250, 220, 80), RGB(120, 220, 230)
};

static double NowSec(const App& app) {
    LARGE_INTEGER counter; QueryPerformanceCounter(&counter);
    return double(counter.QuadPart - app.qpcStart.QuadPart) / double(app.qpcFrequency.QuadPart);
}

static std::string Narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), &result[0], size, nullptr, nullptr);
    return result;
}

// ------------------------------------------------------------- logging ----
static FILE* OpenLog() {
    FILE* logFile = nullptr;
    if (fopen_s(&logFile, "WifiGraphWin_log.csv", "ab") != 0 || !logFile) return nullptr;
    fseek(logFile, 0, SEEK_END);
    if (ftell(logFile) == 0)
        fputs("timestamp,adapter,ssid,link_tx_mbps,link_rx_mbps,"
              "upload_KBps,download_KBps,upload_MBps,download_MBps,"
              "signal_dbm,signal_percent\n", logFile);
    return logFile;
}

static void LogSample(FILE* logFile, const Series& series, const Sample& sample) {
    if (!logFile) return;
    time_t now = time(nullptr);
    tm localTime; localtime_s(&localTime, &now);
    char timestamp[32];
    strftime(timestamp, sizeof timestamp, "%Y-%m-%d %H:%M:%S", &localTime);

    fprintf(logFile, "%s,\"%s\",\"%s\",", timestamp, Narrow(series.name).c_str(), Narrow(series.ssid).c_str());
    if (!sample.connected) {
        fputs(",,,,,,,\n", logFile);                      // adapter not connected: blank fields
    } else {
        fprintf(logFile, "%.1f,%.1f,", sample.txMbps, sample.rxMbps);
        if (sample.hasThroughput)
            fprintf(logFile, "%.2f,%.2f,%.4f,%.4f,", sample.upBps / 1e3, sample.downBps / 1e3,
                    sample.upBps / 1e6, sample.downBps / 1e6);
        else
            fputs(",,,,", logFile);                       // first sample: no delta yet
        fprintf(logFile, "%.0f,%d\n", sample.rssiDbm, sample.qualityPercent);
    }
    fflush(logFile);
}

// -------------------------------------------------------------- polling ----
static void Poll(App& app) {
    if (!app.wlanHandle) return;

    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    DWORD result = WlanEnumInterfaces(app.wlanHandle, nullptr, &interfaces);
    if (result != ERROR_SUCCESS) {
        app.status = L"WlanEnumInterfaces failed (error " + std::to_wstring(result) + L")";
        return;
    }
    app.status = interfaces->dwNumberOfItems ? L"" : L"No WiFi adapter found.";
    double now = NowSec(app);

    for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
        const WLAN_INTERFACE_INFO& interfaceInfo = interfaces->InterfaceInfo[i];

        wchar_t guidStr[64];
        StringFromGUID2(interfaceInfo.InterfaceGuid, guidStr, 64);
        Series& series = app.series[guidStr];
        if (series.name.empty()) {
            series.name  = interfaceInfo.strInterfaceDescription;
            series.color = kPalette[(app.series.size() - 1) % (sizeof kPalette / sizeof kPalette[0])];
        }

        Sample sample;
        sample.timeSec = now;

        if (interfaceInfo.isState == wlan_interface_state_connected) {
            // --- link rate, SSID, signal quality
            PWLAN_CONNECTION_ATTRIBUTES connection = nullptr;
            DWORD connectionSize = 0;
            if (WlanQueryInterface(app.wlanHandle, &interfaceInfo.InterfaceGuid, wlan_intf_opcode_current_connection,
                                   nullptr, &connectionSize, (PVOID*)&connection, nullptr) == ERROR_SUCCESS && connection) {
                const auto& association = connection->wlanAssociationAttributes;
                sample.txMbps = association.ulTxRate / 1000.0;    // reported in Kbps
                sample.rxMbps = association.ulRxRate / 1000.0;
                sample.qualityPercent = (int)association.wlanSignalQuality;
                sample.connected = true;

                wchar_t ssid[64] = {};
                MultiByteToWideChar(CP_UTF8, 0, (const char*)association.dot11Ssid.ucSSID,
                                    (int)association.dot11Ssid.uSSIDLength, ssid, 63);
                series.ssid = ssid;
                WlanFreeMemory(connection);
            }

            // --- signal strength in dBm (falls back to an estimate from quality %)
            LONG* pRssi = nullptr; DWORD rssiSize = 0;
            if (WlanQueryInterface(app.wlanHandle, &interfaceInfo.InterfaceGuid, wlan_intf_opcode_rssi,
                                   nullptr, &rssiSize, (PVOID*)&pRssi, nullptr) == ERROR_SUCCESS && pRssi) {
                sample.rssiDbm = (double)*pRssi;
                WlanFreeMemory(pRssi);
            } else {
                sample.rssiDbm = sample.qualityPercent / 2.0 - 100.0;
                sample.rssiEstimated = true;
            }

            // --- throughput from the adapter's byte counters
            bool countersRead = false; ULONG64 inBytes = 0, outBytes = 0;
            NET_LUID luid;
            if (ConvertInterfaceGuidToLuid(&interfaceInfo.InterfaceGuid, &luid) == NO_ERROR) {
                MIB_IF_ROW2 ifRow; memset(&ifRow, 0, sizeof ifRow);
                ifRow.InterfaceLuid = luid;
                if (GetIfEntry2(&ifRow) == NO_ERROR) { inBytes = ifRow.InOctets; outBytes = ifRow.OutOctets; countersRead = true; }
            }
            if (countersRead) {
                if (series.hasPrevCounters && now > series.prevTimeSec &&
                    inBytes >= series.prevInBytes && outBytes >= series.prevOutBytes) {
                    double elapsedSec = now - series.prevTimeSec;
                    sample.downBps = double(inBytes - series.prevInBytes) / elapsedSec;
                    sample.upBps   = double(outBytes - series.prevOutBytes) / elapsedSec;
                    sample.hasThroughput = true;
                }
                series.prevInBytes = inBytes; series.prevOutBytes = outBytes;
                series.prevTimeSec = now; series.hasPrevCounters = true;
            } else {
                series.hasPrevCounters = false;
            }
        } else {
            series.hasPrevCounters = false;
        }

        series.samples.push_back(sample);
        LogSample(app.logFile, series, sample);
        while (!series.samples.empty() && now - series.samples.front().timeSec > kHistory)
            series.samples.pop_front();
    }
    WlanFreeMemory(interfaces);
}

// -------------------------------------------------------------- drawing ----
static double NiceMax(double value) {
    if (value <= 0) return 1;
    value *= 1.1;
    double magnitude = std::pow(10.0, std::floor(std::log10(value)));
    static const double multipliers[] = { 1, 1.5, 2, 2.5, 3, 4, 5, 6, 8, 10 };
    for (double multiplier : multipliers) if (value <= multiplier * magnitude) return multiplier * magnitude;
    return 10 * magnitude;
}

static std::wstring FmtDur(int sec) {
    wchar_t label[32];
    if (sec == 0)           return L"now";
    if (sec < 60)           swprintf_s(label, L"-%ds", sec);
    else if (sec % 60 == 0) swprintf_s(label, L"-%dm", sec / 60);
    else                    swprintf_s(label, L"-%dm%02ds", sec / 60, sec % 60);
    return label;
}

static std::wstring FmtRate(double bps) {          // auto KB/s or MB/s (decimal units)
    wchar_t label[32];
    if (bps >= 1e6) swprintf_s(label, L"%.2f MB/s", bps / 1e6);
    else            swprintf_s(label, L"%.1f KB/s", bps / 1e3);
    return label;
}

static void Text(HDC dc, int x, int y, const std::wstring& text, COLORREF color, UINT align = TA_LEFT) {
    SetTextColor(dc, color);
    SetTextAlign(dc, align | TA_TOP);
    TextOutW(dc, x, y, text.c_str(), (int)text.size());
}

typedef double (*Getter)(const Sample&, int pass);      // returns NAN when there is no value
static double GLink(const Sample& sample, int pass) { return sample.connected ? (pass == 0 ? sample.txMbps : sample.rxMbps) : NAN; }
static double GThr (const Sample& sample, int pass) { return (sample.connected && sample.hasThroughput) ? (pass == 0 ? sample.upBps : sample.downBps) : NAN; }
static double GSig (const Sample& sample, int)      { return sample.connected ? sample.rssiDbm : NAN; }

static void DrawPanel(HDC dc, const App& app, RECT panel, const std::wstring& title, Getter getValue, int passes,
                      double scale, double yMin, double yMax, int gridDivisions, double windowStart, bool timeLabels) {
    const COLORREF gridColor = RGB(58, 62, 72), textColor = RGB(200, 205, 215);

    SelectObject(dc, app.fontBold);
    Text(dc, panel.left, panel.top - 18, title, textColor);
    SelectObject(dc, app.font);

    auto toPixelX = [&](double timeSec) { return panel.left + (int)((timeSec - windowStart) / app.visibleSpanSec * (panel.right - panel.left)); };
    auto toPixelY = [&](double value) { return panel.bottom - (int)((value - yMin) / (yMax - yMin) * (panel.bottom - panel.top)); };

    HPEN gridPen = CreatePen(PS_SOLID, 1, gridColor);
    HGDIOBJ oldPen = SelectObject(dc, gridPen);
    double range = yMax - yMin;
    for (int i = 0; i <= gridDivisions; ++i) {           // horizontal grid + Y labels
        double value = yMin + range * i / gridDivisions;
        int gridY = toPixelY(value);
        MoveToEx(dc, panel.left, gridY, nullptr); LineTo(dc, panel.right, gridY);
        wchar_t label[32];
        if (range < 2) swprintf_s(label, L"%.2f", value);
        else if (range < 20) swprintf_s(label, L"%.1f", value);
        else swprintf_s(label, L"%.0f", value);
        Text(dc, panel.left - 6, gridY - 8, label, textColor, TA_RIGHT);
    }
    for (int i = 0; i <= 4; ++i) {                       // vertical grid (+ time labels)
        int gridX = panel.left + (panel.right - panel.left) * i / 4;
        MoveToEx(dc, gridX, panel.top, nullptr); LineTo(dc, gridX, panel.bottom);
        if (timeLabels) {
            int secAgo = (int)(app.visibleSpanSec * (4 - i) / 4.0);
            Text(dc, gridX, panel.bottom + 6, FmtDur(secAgo), textColor,
                 i == 0 ? TA_LEFT : (i == 4 ? TA_RIGHT : TA_CENTER));
        }
    }
    SelectObject(dc, oldPen); DeleteObject(gridPen);

    SaveDC(dc);                                          // data lines, clipped to the panel
    IntersectClipRect(dc, panel.left, panel.top - 2, panel.right + 1, panel.bottom + 2);
    for (auto& entry : app.series) {
        const Series& series = entry.second;
        for (int pass = 0; pass < passes; ++pass) {      // 0 = TX/upload (solid), 1 = RX/download (dashed)
            HPEN linePen = CreatePen(pass == 0 ? PS_SOLID : PS_DASH, pass == 0 ? 2 : 1, series.color);
            HGDIOBJ oldLinePen = SelectObject(dc, linePen);
            bool penDown = false;
            for (const Sample& sample : series.samples) {
                double value = getValue(sample, pass);
                if (std::isnan(value)) { penDown = false; continue; }   // gap
                int pixelX = toPixelX(sample.timeSec), pixelY = toPixelY(value * scale);
                if (!penDown) { MoveToEx(dc, pixelX, pixelY, nullptr); penDown = true; }
                else LineTo(dc, pixelX, pixelY);
            }
            SelectObject(dc, oldLinePen); DeleteObject(linePen);
        }
    }
    RestoreDC(dc, -1);

    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(110, 115, 130));
    oldPen = SelectObject(dc, borderPen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, panel.left, panel.top, panel.right + 1, panel.bottom + 1);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(borderPen);
}

static void Paint(HDC dc, const App& app, int width, int height) {
    const COLORREF bgColor = RGB(24, 26, 32), textColor = RGB(200, 205, 215);

    HBRUSH bgBrush = CreateSolidBrush(bgColor);
    RECT fullRect{ 0, 0, width, height };
    FillRect(dc, &fullRect, bgBrush);
    DeleteObject(bgBrush);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, app.font);

    double now = NowSec(app), windowStart = now - app.visibleSpanSec;
    double maxLinkMbps = 0, maxThroughputBps = 0;

    // ---- header / legend
    int cursorY = 8;
    Text(dc, 12, cursorY, L"WifiGraphWin  -  solid line = TX / upload,  dashed line = RX / download", textColor);
    cursorY += 24;

    for (auto& entry : app.series) {
        const Series& series = entry.second;

        SelectObject(dc, app.fontBold);
        Text(dc, 40, cursorY, series.name, series.color);
        SIZE nameSize; GetTextExtentPoint32W(dc, series.name.c_str(), (int)series.name.size(), &nameSize);
        SelectObject(dc, app.font);

        const Sample* lastSample = series.samples.empty() ? nullptr : &series.samples.back();
        wchar_t linkLine[256], statsLine[256];
        if (lastSample && lastSample->connected) {
            swprintf_s(linkLine, L"   %s   link TX %.1f / RX %.1f Mbps", series.ssid.c_str(), lastSample->txMbps, lastSample->rxMbps);
            std::wstring upText   = lastSample->hasThroughput ? FmtRate(lastSample->upBps)   : L"--";
            std::wstring downText = lastSample->hasThroughput ? FmtRate(lastSample->downBps) : L"--";
            swprintf_s(statsLine, L"up %s    down %s    signal %s%.0f dBm (%d%%)",
                       upText.c_str(), downText.c_str(), lastSample->rssiEstimated ? L"~" : L"",
                       lastSample->rssiDbm, lastSample->qualityPercent);
        } else {
            swprintf_s(linkLine, L"   not connected");
            statsLine[0] = 0;
        }
        Text(dc, 40 + nameSize.cx, cursorY, linkLine, textColor);
        Text(dc, 40, cursorY + 18, statsLine, textColor);

        HPEN penSolid = CreatePen(PS_SOLID, 2, series.color), penDashed = CreatePen(PS_DASH, 1, series.color);
        HGDIOBJ oldPen = SelectObject(dc, penSolid); MoveToEx(dc, 12, cursorY + 5, nullptr);  LineTo(dc, 30, cursorY + 5);
        SelectObject(dc, penDashed);                 MoveToEx(dc, 12, cursorY + 12, nullptr); LineTo(dc, 30, cursorY + 12);
        SelectObject(dc, oldPen); DeleteObject(penSolid); DeleteObject(penDashed);
        cursorY += 42;

        for (const Sample& sample : series.samples) {
            if (sample.timeSec < windowStart || !sample.connected) continue;
            maxLinkMbps = std::max(maxLinkMbps, std::max(sample.txMbps, sample.rxMbps));
            if (sample.hasThroughput) maxThroughputBps = std::max(maxThroughputBps, std::max(sample.upBps, sample.downBps));
        }
    }
    if (!app.status.empty()) { Text(dc, 12, cursorY, app.status, RGB(255, 120, 120)); cursorY += 20; }

    // ---- three stacked panels
    const int titleHeight = 18, panelGap = 8, panelLeft = 70, panelRight = width - 20;
    int panelsTop = cursorY + 4, panelsBottom = height - 48;
    int availHeight = panelsBottom - panelsTop - 3 * titleHeight - 2 * panelGap;
    if (availHeight < 120 || panelRight - panelLeft < 50) return;
    int linkHeight = availHeight * 40 / 100, throughputHeight = availHeight * 30 / 100;
    int signalHeight = availHeight - linkHeight - throughputHeight;
    RECT linkPanel{ panelLeft, panelsTop + titleHeight, panelRight, panelsTop + titleHeight + linkHeight };
    RECT throughputPanel{ panelLeft, linkPanel.bottom + panelGap + titleHeight, panelRight, linkPanel.bottom + panelGap + titleHeight + throughputHeight };
    RECT signalPanel{ panelLeft, throughputPanel.bottom + panelGap + titleHeight, panelRight, throughputPanel.bottom + panelGap + titleHeight + signalHeight };

    // 1) link rate
    DrawPanel(dc, app, linkPanel, L"Link rate (Mbps)", GLink, 2, 1.0, 0, NiceMax(maxLinkMbps > 0 ? maxLinkMbps : 100), 5, windowStart, false);

    // 2) measured throughput, unit chosen automatically
    bool useMegabytes = maxThroughputBps >= 1e6;
    double throughputScale = useMegabytes ? 1e-6 : 1e-3;
    double throughputMax = NiceMax(maxThroughputBps > 0 ? maxThroughputBps * throughputScale : 1.0);
    DrawPanel(dc, app, throughputPanel, useMegabytes ? L"Throughput (MB/s)" : L"Throughput (KB/s)", GThr, 2, throughputScale, 0, throughputMax, 5, windowStart, false);

    // 3) signal strength
    DrawPanel(dc, app, signalPanel, L"Signal strength (dBm)", GSig, 1, 1.0, -100, -30, 7, windowStart, true);

    Text(dc, width / 2, height - 20,
         L"Keys: 1=1 min  2=5 min  3=15 min  4=60 min  C=clear   |   logging to WifiGraphWin_log.csv",
         RGB(130, 135, 150), TA_CENTER);
}

// ------------------------------------------------------------ window proc --
static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    // Set in WM_CREATE; messages that arrive earlier must not use it.
    App* app = (App*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (message) {
    case WM_CREATE:
        app = (App*)((CREATESTRUCTW*)lParam)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);
        app->font     = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        app->fontBold = CreateFontW(-14, 0, 0, 0, FW_BOLD,   0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        Poll(*app);
        SetTimer(hwnd, 1, 1000, nullptr);
        return 0;
    case WM_TIMER:
        Poll(*app);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
        switch (wParam) {
        case '1': app->visibleSpanSec = 60;   break;
        case '2': app->visibleSpanSec = 300;  break;
        case '3': app->visibleSpanSec = 900;  break;
        case '4': app->visibleSpanSec = 3600; break;
        case 'C': for (auto& entry : app->series) entry.second.samples.clear(); break;
        default: return 0;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paintInfo;
        HDC dc = BeginPaint(hwnd, &paintInfo);
        RECT clientRect; GetClientRect(hwnd, &clientRect);
        HDC memDc = CreateCompatibleDC(dc);
        HBITMAP backBuffer = CreateCompatibleBitmap(dc, clientRect.right, clientRect.bottom);
        HGDIOBJ oldBitmap = SelectObject(memDc, backBuffer);
        Paint(memDc, *app, clientRect.right, clientRect.bottom);
        BitBlt(dc, 0, 0, clientRect.right, clientRect.bottom, memDc, 0, 0, SRCCOPY);
        SelectObject(memDc, oldBitmap); DeleteObject(backBuffer); DeleteDC(memDc);
        EndPaint(hwnd, &paintInfo);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        DeleteObject(app->font); DeleteObject(app->fontBold);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    App app;
    QueryPerformanceFrequency(&app.qpcFrequency);
    QueryPerformanceCounter(&app.qpcStart);
    app.logFile = OpenLog();

    DWORD negotiatedVersion = 0;
    DWORD result = WlanOpenHandle(2, nullptr, &negotiatedVersion, &app.wlanHandle);
    if (result != ERROR_SUCCESS) {
        app.wlanHandle = nullptr;
        app.status = L"Cannot open WLAN API (error " + std::to_wstring(result) +
                     L"). Is the 'WLAN AutoConfig' service running?";
    }

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc   = WndProc;
    windowClass.hInstance     = instance;
    windowClass.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"WifiGraphWnd";
    RegisterClassW(&windowClass);

    HWND hwnd = CreateWindowW(windowClass.lpszClassName, L"WifiGraphWin", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1000, 800, nullptr, nullptr, instance, &app);
    ShowWindow(hwnd, showCommand);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (app.wlanHandle) WlanCloseHandle(app.wlanHandle, nullptr);
    if (app.logFile) fclose(app.logFile);
    return (int)msg.wParam;
}
