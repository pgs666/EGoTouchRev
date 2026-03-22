#include <windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <vector>
#include <string>
#include <mutex>
#include <deque>
#include <thread>
#include <chrono>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "btmcu/PenUsbTransport.h"
#include "Logger.h"

// Data
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Hex Dump state
struct LogMessage {
    std::string time;
    std::string direction; // "Tx" or "Rx"
    std::vector<uint8_t> data;
};

static std::mutex g_logMutex;
static std::deque<LogMessage> g_logs;
static const size_t MAX_LOG_COUNT = 500;

void AddLog(const std::string& dir, const std::vector<uint8_t>& data) {
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    struct tm parts;
    localtime_s(&parts, &now_c);
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &parts);

    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logs.push_back({timeStr, dir, data});
    if (g_logs.size() > MAX_LOG_COUNT) {
        g_logs.pop_front();
    }
}

#include <setupapi.h>
#include <optional>
#pragma comment(lib, "setupapi.lib")

// The specific GUID required for Himax Pen USB MCU interface
static const GUID PEN_MCU_DEVICE_INTERFACE_GUID = {0xdd0ebedb, 0xf1d6, 0x4cfa, {0xac, 0xca, 0x71, 0xe6, 0x6d, 0x31, 0x78, 0xca}};

// Global Device State
static std::unique_ptr<Himax::Pen::IPenUsbTransport> g_transport;
static std::thread g_readThread;
static bool g_keepReading = false;
static std::wstring g_devicePath = L"";

std::optional<std::wstring> FindPenMcuDevicePath() {
    HDEVINFO devInfo = SetupDiGetClassDevsW(&PEN_MCU_DEVICE_INTERFACE_GUID,
                                            nullptr,
                                            nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    std::optional<std::wstring> devicePath = std::nullopt;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA ifData{};
        ifData.cbSize = sizeof(ifData);
        if (!SetupDiEnumDeviceInterfaces(devInfo, nullptr, &PEN_MCU_DEVICE_INTERFACE_GUID, index, &ifData)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            continue;
        }

        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &requiredSize, nullptr);
        if (requiredSize < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            continue;
        }

        std::vector<uint8_t> detailBuffer(requiredSize, 0);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, requiredSize, nullptr, nullptr)) {
            continue;
        }

        devicePath = detail->DevicePath;
        break;
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return devicePath;
}

void ReadThreadFunc() {
    while (g_keepReading) {
        if (!g_transport || !g_transport->IsOpen()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        std::vector<uint8_t> rxBuf;
        auto result = g_transport->ReadPacket(rxBuf, 1000); // 1s timeout
        if (result.has_value() && !rxBuf.empty()) {
            AddLog("Rx", rxBuf);
        } else {
            // Might just be timeout
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

void StartReadThread() {
    g_keepReading = true;
    g_readThread = std::thread(ReadThreadFunc);
}

void StopReadThread() {
    g_keepReading = false;
    if (g_readThread.joinable()) {
        g_readThread.join();
    }
}

// UI Helpers
void DrawHexDump(const std::vector<uint8_t>& data) {
    for (size_t i = 0; i < data.size(); ++i) {
        if (i == 4 || i == 5) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%02X", data[i]); // highlight cmd_id
        } else if (i == 6) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%02X", data[i]); // highlight tag
        } else {
            ImGui::Text("%02X", data[i]);
        }
        if (i < data.size() - 1) ImGui::SameLine();
    }
}

void SendPacket(uint16_t cmdId, const std::vector<uint8_t>& payload) {
    if (!g_transport || !g_transport->IsOpen()) return;

    // Header 8 bytes: 0x07 (reportId), 0x01/0x00 (direction etc), 0x02, 0x00, cmd_lo, cmd_hi, 0x11, 0x00
    std::vector<uint8_t> txData(8 + payload.size(), 0);
    txData[0] = 0x07;
    txData[1] = 0x01; // direction or padding depending on command (07 00 vs 07 01 seen in logs, we'll try 01)
    txData[2] = 0x02;
    txData[3] = 0x00;
    txData[4] = cmdId & 0xFF;         // cmd_lo
    txData[5] = (cmdId >> 8) & 0xFF;  // cmd_hi
    txData[6] = 0x11;                 // signature
    txData[7] = 0x00;                 // option / length indicator

    // We can match specific patterns from the reverse engineering document
    if (cmdId == 0x7101) txData[1] = 0x00;
    if (cmdId == 0x7701) txData[1] = 0x00;
    if (cmdId == 0x8001) { txData[1] = 0x01; txData[7] = 0x20; }
    
    std::copy(payload.begin(), payload.end(), txData.begin() + 8);

    auto res = g_transport->WritePacket(txData);
    if (res.has_value()) {
        AddLog("Tx", txData);
    } else {
        AddLog("Tx [ERR]", txData);
    }
}

int main(int, char**)
{
    // AllocConsole();
    // FILE* dummy;
    // freopen_s(&dummy, "CONOUT$", "w", stdout);
    // freopen_s(&dummy, "CONOUT$", "w", stderr);

    Common::Logger::Init("BtMcuTestTool");
    LOG_INFO("Tool", "main", "System", "--- BtMcuTestTool Starts ---");

    g_transport = Himax::Pen::CreatePenUsbTransportWin32();
    StartReadThread();

    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"BtMcuTestToolApp", nullptr };
    ::RegisterClassExW(&wc);
    
    // Create application window (Hide Main Window: Create as WS_POPUP and WS_EX_TOOLWINDOW off-screen)
    HWND hwnd = ::CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"BT MCU Test Tool (Hidden)", WS_POPUP, 
                                  -10000, -10000, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // Hide the main window completely so user just sees the floating ImGui windows
    ::ShowWindow(hwnd, SW_HIDE);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleViewports;
    io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleFonts;
    
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    bool done = false;
    char customCmdBuf[16] = "7101";
    char customPayloadBuf[128] = "";
    char ackBuf[16] = "00";

    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done) break;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Main UI
        ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
        ImGui::Begin("BT MCU Pen USB Protocol Test Tool");

        // 1. Connection
        ImGui::Text("Connection Status: "); ImGui::SameLine();
        if (g_transport->IsOpen()) {
            ImGui::TextColored(ImVec4(0,1,0,1), "CONNECTED");
        } else {
            ImGui::TextColored(ImVec4(1,0,0,1), "DISCONNECTED");
        }

        if (ImGui::Button("Connect (Auto Enum)")) {
            auto pathOpt = FindPenMcuDevicePath();
            if (pathOpt.has_value()) {
                auto res = g_transport->Open(pathOpt.value());
                if (!res.has_value()) {
                    LOG_ERROR("Tool", "Open", "UI", "Failed to open MCU device.");
                } else {
                    LOG_INFO("Tool", "Open", "UI", "Successfully connected to MCU device.");
                    g_devicePath = pathOpt.value();
                }
            } else {
                LOG_ERROR("Tool", "Open", "UI", "Failed to find MCU device interface GUID.");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            g_transport->Close();
        }

        ImGui::Separator();

        // 2. Quick Commands
        ImGui::Text("Quick Commands");
        if (ImGui::Button("Query Status (0x7101)")) { SendPacket(0x7101, {}); }
        ImGui::SameLine();
        if (ImGui::Button("Query Info (0x7701)")) { SendPacket(0x7701, {}); }
        ImGui::SameLine();
        if (ImGui::Button("Init Param (0x7D01)")) { SendPacket(0x7D01, {0x20}); }
        ImGui::SameLine();
        if (ImGui::Button("Match Info Reply (0x7E01)")) { SendPacket(0x7E01, {0x01}); }

        ImGui::Separator();

        // 3. Custom Commands
        ImGui::Text("Custom Command");
        ImGui::InputText("Command ID (Hex)", customCmdBuf, sizeof(customCmdBuf), ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::InputText("Payload (Hex Space Separated)", customPayloadBuf, sizeof(customPayloadBuf));
        if (ImGui::Button("Send Custom")) {
            uint32_t cmdId = 0;
            sscanf_s(customCmdBuf, "%x", &cmdId);
            
            std::vector<uint8_t> payload;
            char* next_token = nullptr;
            char* token = strtok_s(customPayloadBuf, " ", &next_token);
            while (token != nullptr) {
                uint32_t val = 0;
                if (sscanf_s(token, "%x", &val) == 1) {
                    payload.push_back(static_cast<uint8_t>(val));
                }
                token = strtok_s(nullptr, " ", &next_token);
            }
            SendPacket(static_cast<uint16_t>(cmdId), payload);
        }

        ImGui::Separator();

        // 4. ACK
        ImGui::Text("Send ACK (0x8001)");
        ImGui::InputText("ACK Code (Hex)", ackBuf, sizeof(ackBuf), ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        if (ImGui::Button("Send ACK")) {
            uint32_t ackVal = 0;
            sscanf_s(ackBuf, "%x", &ackVal);
            SendPacket(0x8001, {static_cast<uint8_t>(ackVal)});
        }

        ImGui::Separator();

        // 5. Logs
        ImGui::Text("Hex Dump (%zu / %zu messages)", g_logs.size(), MAX_LOG_COUNT);
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            std::lock_guard<std::mutex> lock(g_logMutex);
            g_logs.clear();
        }

        ImGui::BeginChild("LogRegion", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            for (const auto& log : g_logs) {
                ImGui::Text("[%s] %s: ", log.time.c_str(), log.direction.c_str());
                ImGui::SameLine();
                DrawHexDump(log.data);
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
    }

    StopReadThread();
    if (g_transport) {
        g_transport->Close();
        g_transport.reset();
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    Common::Logger::Shutdown();
    return 0;
}

bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
