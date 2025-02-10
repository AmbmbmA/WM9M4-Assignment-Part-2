// ChatClient_ImGui.cpp
// 定义 _CRT_SECURE_NO_WARNINGS 以避免安全函数警告
#define _CRT_SECURE_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <d3d11.h>
#include <tchar.h>
#include <thread>
#include <mutex>
#include <vector>
#include <map>
#include <string>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <chrono>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "Ws2_32.lib")

#define DEFAULT_BUFFER_SIZE 1024

// --------------------- 客户端网络与聊天全局变量 ---------------------
static SOCKET clientSocket = INVALID_SOCKET;  // 客户端 Socket
int myID = -1;  // 本客户端ID（采用本地端口号）
std::mutex clientDataMutex;
std::vector<std::string> groupChatHistory;                     // 群聊历史记录
std::map<int, std::vector<std::string>> privateChatHistory;      // 私聊记录，key 为对方ID
std::vector<int> onlineUserList;                                 // 在线用户列表（不包含自己）

// 协议说明：
// 群聊消息由服务器转发，格式："G:<sender>:<message>"
// 私聊消息由服务器转发，格式："P:<sender>:<message>"
// 当有客户端登录或退出时，服务器会广播类似 "Client <id> has logged in!" 或 "Client <id> has logged out!"
// 我们通过解析这些系统消息来更新在线用户列表及群聊历史
void MessageDecode(const std::string& msg) {
    // 登录系统消息
    if (msg.find("has logged in!") != std::string::npos) {
        size_t pos1 = msg.find("Client ");
        size_t pos2 = msg.find(" has logged in!");
        if (pos1 != std::string::npos && pos2 != std::string::npos) {
            std::string idStr = msg.substr(pos1 + 7, pos2 - (pos1 + 7));
            int id = std::stoi(idStr);
            std::lock_guard<std::mutex> lock(clientDataMutex);
            if (id != myID && std::find(onlineUserList.begin(), onlineUserList.end(), id) == onlineUserList.end())
                onlineUserList.push_back(id);
            groupChatHistory.push_back(msg);
        }
        return;
    }
    // 退出系统消息
    if (msg.find("has logged out") != std::string::npos) {
        size_t pos1 = msg.find("Client ");
        size_t pos2 = msg.find(" has logged out");
        if (pos1 != std::string::npos && pos2 != std::string::npos) {
            std::string idStr = msg.substr(pos1 + 7, pos2 - (pos1 + 7));
            int id = std::stoi(idStr);
            std::lock_guard<std::mutex> lock(clientDataMutex);
            onlineUserList.erase(std::remove(onlineUserList.begin(), onlineUserList.end(), id), onlineUserList.end());
            groupChatHistory.push_back(msg);
        }
        return;
    }
    // 群聊消息
    if (msg.substr(0, 2) == "G:") {
        size_t pos = msg.find(':', 2);
        if (pos != std::string::npos) {
            int sender = std::stoi(msg.substr(2, pos - 2));
            std::string content = msg.substr(pos + 1);
            std::lock_guard<std::mutex> lock(clientDataMutex);
            groupChatHistory.push_back("Client " + std::to_string(sender) + ": " + content);
        }
        return;
    }
    // 私聊消息
    if (msg.substr(0, 2) == "P:") {
        size_t pos = msg.find(':', 2);
        if (pos != std::string::npos) {
            int sender = std::stoi(msg.substr(2, pos - 2));
            std::string content = msg.substr(pos + 1);
            std::lock_guard<std::mutex> lock(clientDataMutex);
            privateChatHistory[sender].push_back("Client " + std::to_string(sender) + ": " + content);
        }
        return;
    }
    // 其他消息直接添加到群聊历史
    {
        std::lock_guard<std::mutex> lock(clientDataMutex);
        groupChatHistory.push_back(msg);
    }
}

// 客户端网络接收线程，不断接收服务器消息
void ClientNetworkThread() {
    char buffer[DEFAULT_BUFFER_SIZE];
    while (true) {
        int bytes = recv(clientSocket, buffer, DEFAULT_BUFFER_SIZE - 1, 0);
        if (bytes <= 0)
            break;
        buffer[bytes] = '\0';
        std::string received(buffer);
        MessageDecode(received);
    }
}

// --------------------- DirectX 11 全局变量（客户端） ---------------------
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static bool                    g_SwapChainOccluded = false;
static UINT                    g_ResizeWidth = 0, g_ResizeHeight = 0;

// --------------------- DirectX 辅助函数（客户端） ---------------------
bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
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
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
        &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (!pBackBuffer)
        return false;
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
    return true;
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

// --------------------- Win32 消息处理 ---------------------
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
        g_ResizeWidth = LOWORD(lParam);
        g_ResizeHeight = HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// 辅助函数：右对齐文本（用于显示自己发送的消息）
void RightAlignedText(const char* text) {
    float windowWidth = ImGui::GetWindowWidth();
    float textWidth = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(windowWidth - textWidth - 10);
    ImGui::Text("%s", text);
}

// --------------------- 主函数（客户端 GUI） ---------------------
int main(int, char**)
{
    // --------------------- 初始化客户端 Winsock ---------------------
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "Client WSAStartup failed" << std::endl;
        return 1;
    }
    clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (clientSocket == INVALID_SOCKET) {
        std::cerr << "Client socket creation failed" << std::endl;
        WSACleanup();
        return 1;
    }
    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(65432);
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);
    if (connect(clientSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Client connect failed" << std::endl;
        closesocket(clientSocket);
        WSACleanup();
        return 1;
    }
    std::cout << "Client connected to server" << std::endl;

    // 获取本地端口作为客户端ID
    sockaddr_in localAddr = {};
    int addrLen = sizeof(localAddr);
    getsockname(clientSocket, (sockaddr*)&localAddr, &addrLen);
    myID = ntohs(localAddr.sin_port);

    // 启动客户端网络接收线程
    std::thread clientNetThread(ClientNetworkThread);
    clientNetThread.detach();

    // --------------------- 初始化 ImGui 与 DirectX ---------------------
    // 创建 Win32 窗口
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L,
                      GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
                      _T("ImGui Chat Client"), nullptr };
    RegisterClassEx(&wc);
    HWND hwnd = CreateWindow(wc.lpszClassName, _T("ImGui Chat Client"),
        WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // 界面控制变量
    bool showMenu = true;
    bool showPersonal = false;
    bool showGroupChat = false;
    bool showOnlineUsers = false;
    std::map<int, bool> showPrivateChat; // key: 对方ID, value: 窗口显示状态
    // 为每个私聊窗口保存输入内容（key: 对方ID）
    std::map<int, std::string> privateInputMap;
    // 初始个人信息
    std::string personalInfo = "Name: UserX\nID: " + std::to_string(myID);
    // 群聊输入缓冲区
    static char groupInput[256] = "";

    // 主消息循环
    bool done = false;
    while (!done) {
        // 处理 Windows 消息
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // 处理窗口尺寸变化
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            ID3D11Texture2D* pBackBuffer = nullptr;
            g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
            if (pBackBuffer) {
                g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
                pBackBuffer->Release();
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // -------- 菜单窗口 --------
        if (showMenu) {
            ImGui::Begin("Menu", &showMenu);
            ImGui::Checkbox("Personal Info", &showPersonal);
            ImGui::Checkbox("Group Chat", &showGroupChat);
            ImGui::Checkbox("Online Users", &showOnlineUsers);
            ImGui::End();
        }

        // -------- 个人信息窗口 --------
        if (showPersonal) {
            ImGui::Begin("Personal Info", &showPersonal);
            ImGui::Text("%s", personalInfo.c_str());
            ImGui::End();
        }

        // -------- 群聊窗口 --------
        if (showGroupChat) {
            ImGui::Begin("Group Chat", &showGroupChat);
            ImGui::BeginChild("GroupChatMessages", ImVec2(0, -50), true);
            {
                std::lock_guard<std::mutex> lock(clientDataMutex);
                for (const auto& msg : groupChatHistory) {
                    if (msg.find("Me:") == 0)
                        RightAlignedText(msg.c_str());
                    else
                        ImGui::TextWrapped("%s", msg.c_str());
                }
            }
            ImGui::EndChild();
            ImGui::InputText("##GroupInput", groupInput, IM_ARRAYSIZE(groupInput));
            ImGui::SameLine();
            if (ImGui::Button("Send Group")) {
                if (strlen(groupInput) > 0) {
                    // 构造群聊消息格式 "G:<myID>:<message>"
                    std::string toSend = "G:" + std::to_string(myID) + ":" + groupInput;
                    send(clientSocket, toSend.c_str(), (int)toSend.size(), 0);
                    {
                        std::lock_guard<std::mutex> lock(clientDataMutex);
                        groupChatHistory.push_back("Me: " + std::string(groupInput));
                    }
                    strcpy_s(groupInput, "");
                }
            }
            ImGui::End();
        }

        // -------- 在线用户窗口 --------
        if (showOnlineUsers) {
            ImGui::Begin("Online Users", &showOnlineUsers);
            {
                std::lock_guard<std::mutex> lock(clientDataMutex);
                for (int id : onlineUserList) {
                    std::string label = "Client " + std::to_string(id);
                    if (ImGui::Selectable(label.c_str())) {
                        showPrivateChat[id] = true;
                        if (privateChatHistory.find(id) == privateChatHistory.end())
                            privateChatHistory[id] = std::vector<std::string>();
                        if (privateInputMap.find(id) == privateInputMap.end())
                            privateInputMap[id] = "";
                    }
                }
            }
            ImGui::End();
        }

        // -------- 私聊窗口 --------
        for (auto& pair : showPrivateChat) {
            int targetId = pair.first;
            bool& open = pair.second;
            std::string windowTitle = "Private Chat with Client " + std::to_string(targetId);
            if (open) {
                ImGui::Begin(windowTitle.c_str(), &open);
                std::string childName = "PrivateChatMessages_" + std::to_string(targetId);
                ImGui::BeginChild(childName.c_str(), ImVec2(0, -50), true);
                {
                    std::lock_guard<std::mutex> lock(clientDataMutex);
                    for (const auto& msg : privateChatHistory[targetId]) {
                        if (msg.find("Me:") == 0)
                            RightAlignedText(msg.c_str());
                        else
                            ImGui::TextWrapped("%s", msg.c_str());
                    }
                }
                ImGui::EndChild();
                // 每个私聊窗口使用唯一的输入控件
                std::string inputLabel = "##PrivateInput_" + std::to_string(targetId);
                char buf[256] = "";
                if (privateInputMap.find(targetId) != privateInputMap.end()) {
                    strcpy_s(buf, privateInputMap[targetId].c_str());
                }
                ImGui::InputText(inputLabel.c_str(), buf, IM_ARRAYSIZE(buf));
                privateInputMap[targetId] = std::string(buf);
                ImGui::SameLine();
                std::string btnLabel = "Send##Private_" + std::to_string(targetId);
                if (ImGui::Button(btnLabel.c_str())) {
                    if (strlen(buf) > 0) {
                        // 构造私聊消息格式: "/dm <target> P:<myID>:<message>"
                        std::string toSend = "/dm " + std::to_string(targetId) + " " + "P:" + std::to_string(myID) + ":" + buf;
                        send(clientSocket, toSend.c_str(), (int)toSend.size(), 0);
                        {
                            std::lock_guard<std::mutex> lock(clientDataMutex);
                            privateChatHistory[targetId].push_back("Me: " + std::string(buf));
                        }
                        strcpy_s(buf, "");
                        privateInputMap[targetId] = "";
                    }
                }
                ImGui::End();
            }
        }

        // -------- 渲染 ImGui --------
        ImGui::Render();
        const float clearColorWithAlpha[4] = { 0.45f, 0.55f, 0.60f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColorWithAlpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    // -------- 清理退出 --------
    if (clientSocket != INVALID_SOCKET) {
        closesocket(clientSocket);
        clientSocket = INVALID_SOCKET;
    }
    WSACleanup();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
    return 0;
}
