#include "ControlPipeClient.h"

#include <windows.h>

#include <string>
#include <vector>

namespace {

constexpr const char* kWindowClassName = "EGoTouchControlServiceTestWindow";
constexpr const char* kPipeName = R"(\\.\pipe\EGoTouchControlService)";
constexpr UINT_PTR kRefreshTimerId = 101;

enum : int {
    ID_BTN_REFRESH = 1001,
    ID_BTN_PING,
    ID_BTN_EVENT_DISPLAY_ON,
    ID_BTN_EVENT_DISPLAY_OFF,
    ID_BTN_EVENT_LID_ON,
    ID_BTN_EVENT_LID_OFF,
    ID_BTN_EVENT_RESUME,
    ID_BTN_EVENT_SHUTDOWN,
    ID_BTN_CMD_INIT,
    ID_BTN_CMD_DEINIT,
    ID_BTN_CMD_START,
    ID_BTN_CMD_STOP,
    ID_BTN_CMD_ENTER_IDLE,
    ID_BTN_CMD_EXIT_IDLE,
    ID_BTN_CMD_CHECK_BUS,
    ID_BTN_BUS_ALIVE,
    ID_BTN_BUS_DEAD,
    ID_BTN_CLEAR_HISTORY,
    ID_BTN_STOP_SERVICE,
};

struct UiContext {
    Control::ControlPipeClient client{kPipeName};
    HWND stateText = nullptr;
    HWND historyText = nullptr;
    HWND statusText = nullptr;
};

std::string JoinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        out += line;
        out += "\r\n";
    }
    return out;
}

void SetText(HWND hwnd, const std::string& text) {
    if (hwnd != nullptr) {
        SetWindowTextA(hwnd, text.c_str());
    }
}

bool SendRequest(UiContext* ctx, const std::string& request, std::vector<std::string>& payload, std::string& status) {
    std::string error;
    if (!ctx->client.Request(request, payload, error)) {
        status = "Request failed: " + error;
        return false;
    }
    status = "OK: " + request;
    return true;
}

void RefreshView(UiContext* ctx) {
    std::vector<std::string> payload;
    std::string status;

    if (SendRequest(ctx, "STATE", payload, status)) {
        SetText(ctx->stateText, JoinLines(payload));
    } else {
        SetText(ctx->stateText, "State unavailable.");
    }

    payload.clear();
    std::string historyStatus;
    if (SendRequest(ctx, "HISTORY", payload, historyStatus)) {
        SetText(ctx->historyText, JoinLines(payload));
    } else {
        SetText(ctx->historyText, "History unavailable.");
    }

    SetText(ctx->statusText, status + " | " + historyStatus);
}

void SendCommandAndRefresh(UiContext* ctx, const std::string& request) {
    std::vector<std::string> payload;
    std::string status;
    if (SendRequest(ctx, request, payload, status)) {
        if (!payload.empty()) {
            status += " | " + payload.front();
        }
    }
    SetText(ctx->statusText, status);
    RefreshView(ctx);
}

HWND AddButton(HWND parent, const char* text, int x, int y, int w, int h, int id) {
    return CreateWindowExA(
        0,
        "BUTTON",
        text,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x, y, w, h,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleA(nullptr),
        nullptr);
}

} // namespace

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* ctx = reinterpret_cast<UiContext*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* localCtx = new UiContext();
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(localCtx));
        ctx = localCtx;

        AddButton(hwnd, "Refresh", 12, 12, 90, 28, ID_BTN_REFRESH);
        AddButton(hwnd, "Ping", 108, 12, 90, 28, ID_BTN_PING);
        AddButton(hwnd, "DisplayOn", 204, 12, 100, 28, ID_BTN_EVENT_DISPLAY_ON);
        AddButton(hwnd, "DisplayOff", 310, 12, 100, 28, ID_BTN_EVENT_DISPLAY_OFF);
        AddButton(hwnd, "LidOn", 416, 12, 90, 28, ID_BTN_EVENT_LID_ON);
        AddButton(hwnd, "LidOff", 512, 12, 90, 28, ID_BTN_EVENT_LID_OFF);
        AddButton(hwnd, "Resume", 608, 12, 90, 28, ID_BTN_EVENT_RESUME);
        AddButton(hwnd, "ShutdownEvt", 704, 12, 100, 28, ID_BTN_EVENT_SHUTDOWN);

        AddButton(hwnd, "Init", 12, 48, 80, 28, ID_BTN_CMD_INIT);
        AddButton(hwnd, "Deinit", 98, 48, 80, 28, ID_BTN_CMD_DEINIT);
        AddButton(hwnd, "Start", 184, 48, 80, 28, ID_BTN_CMD_START);
        AddButton(hwnd, "Stop", 270, 48, 80, 28, ID_BTN_CMD_STOP);
        AddButton(hwnd, "EnterIdle", 356, 48, 90, 28, ID_BTN_CMD_ENTER_IDLE);
        AddButton(hwnd, "ExitIdle", 452, 48, 90, 28, ID_BTN_CMD_EXIT_IDLE);
        AddButton(hwnd, "CheckBus", 548, 48, 90, 28, ID_BTN_CMD_CHECK_BUS);
        AddButton(hwnd, "BusAlive", 644, 48, 80, 28, ID_BTN_BUS_ALIVE);
        AddButton(hwnd, "BusDead", 730, 48, 80, 28, ID_BTN_BUS_DEAD);

        AddButton(hwnd, "ClearHistory", 12, 84, 110, 28, ID_BTN_CLEAR_HISTORY);
        AddButton(hwnd, "StopService", 128, 84, 110, 28, ID_BTN_STOP_SERVICE);

        localCtx->stateText = CreateWindowExA(
            WS_EX_CLIENTEDGE,
            "EDIT",
            "",
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            12, 120, 798, 74,
            hwnd,
            nullptr,
            GetModuleHandleA(nullptr),
            nullptr);

        localCtx->historyText = CreateWindowExA(
            WS_EX_CLIENTEDGE,
            "EDIT",
            "",
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            12, 202, 798, 290,
            hwnd,
            nullptr,
            GetModuleHandleA(nullptr),
            nullptr);

        localCtx->statusText = CreateWindowExA(
            WS_EX_CLIENTEDGE,
            "EDIT",
            "Ready",
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_READONLY,
            12, 500, 798, 24,
            hwnd,
            nullptr,
            GetModuleHandleA(nullptr),
            nullptr);

        SetTimer(hwnd, kRefreshTimerId, 1000, nullptr);
        RefreshView(localCtx);
        return 0;
    }
    case WM_TIMER:
        if (ctx != nullptr && wparam == kRefreshTimerId) {
            RefreshView(ctx);
        }
        return 0;
    case WM_COMMAND:
        if (ctx == nullptr) {
            return 0;
        }
        switch (LOWORD(wparam)) {
        case ID_BTN_REFRESH:
            RefreshView(ctx);
            return 0;
        case ID_BTN_PING:
            SendCommandAndRefresh(ctx, "PING");
            return 0;
        case ID_BTN_EVENT_DISPLAY_ON:
            SendCommandAndRefresh(ctx, "EVENT DisplayOn");
            return 0;
        case ID_BTN_EVENT_DISPLAY_OFF:
            SendCommandAndRefresh(ctx, "EVENT DisplayOff");
            return 0;
        case ID_BTN_EVENT_LID_ON:
            SendCommandAndRefresh(ctx, "EVENT LidOn");
            return 0;
        case ID_BTN_EVENT_LID_OFF:
            SendCommandAndRefresh(ctx, "EVENT LidOff");
            return 0;
        case ID_BTN_EVENT_RESUME:
            SendCommandAndRefresh(ctx, "EVENT ResumeAutomatic");
            return 0;
        case ID_BTN_EVENT_SHUTDOWN:
            SendCommandAndRefresh(ctx, "EVENT Shutdown");
            return 0;
        case ID_BTN_CMD_INIT:
            SendCommandAndRefresh(ctx, "CMD Init");
            return 0;
        case ID_BTN_CMD_DEINIT:
            SendCommandAndRefresh(ctx, "CMD Deinit");
            return 0;
        case ID_BTN_CMD_START:
            SendCommandAndRefresh(ctx, "CMD StartStreaming");
            return 0;
        case ID_BTN_CMD_STOP:
            SendCommandAndRefresh(ctx, "CMD StopStreaming");
            return 0;
        case ID_BTN_CMD_ENTER_IDLE:
            SendCommandAndRefresh(ctx, "CMD EnterIdle");
            return 0;
        case ID_BTN_CMD_EXIT_IDLE:
            SendCommandAndRefresh(ctx, "CMD ExitIdle");
            return 0;
        case ID_BTN_CMD_CHECK_BUS:
            SendCommandAndRefresh(ctx, "CMD CheckBus");
            return 0;
        case ID_BTN_BUS_ALIVE:
            SendCommandAndRefresh(ctx, "BUS alive");
            return 0;
        case ID_BTN_BUS_DEAD:
            SendCommandAndRefresh(ctx, "BUS dead");
            return 0;
        case ID_BTN_CLEAR_HISTORY:
            SendCommandAndRefresh(ctx, "CLEAR_HISTORY");
            return 0;
        case ID_BTN_STOP_SERVICE:
            SendCommandAndRefresh(ctx, "STOP_SERVICE");
            return 0;
        default:
            break;
        }
        return 0;
    case WM_DESTROY:
        if (ctx != nullptr) {
            delete ctx;
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        }
        KillTimer(hwnd, kRefreshTimerId);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int cmd_show) {
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClassName;
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowExA(
        0,
        kWindowClassName,
        "EGoTouch Control Service Test Tool (No Device Path)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        840,
        590,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (hwnd == nullptr) {
        return 1;
    }

    ShowWindow(hwnd, cmd_show);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return static_cast<int>(msg.wParam);
}

