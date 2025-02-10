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
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "Ws2_32.lib")

#define DEFAULT_BUFFER_SIZE 1024

static SOCKET clientSocket = INVALID_SOCKET;
int myID = -1;
std::mutex clientDataMutex;
std::vector<std::string> groupChatHistory;
std::map<int, std::vector<std::string>> privateChatHistory;
std::vector<int> onlineUserList;

void MessageDecode(std::string& message) {

	// login
	if (message.find("has logged in!") != std::string::npos) {

		size_t pos1 = message.find("Client ");
		size_t pos2 = message.find(" has logged in!");

		if (pos1 != std::string::npos && pos2 != std::string::npos) {

			std::string idString = message.substr(pos1 + 7, pos2 - (pos1 + 7));
			int id = std::stoi(idString);

			std::lock_guard<std::mutex> lock(clientDataMutex);
			if (id != myID && std::find(onlineUserList.begin(), onlineUserList.end(), id) == onlineUserList.end())
				onlineUserList.push_back(id);
			groupChatHistory.push_back(message);
		}
		return;
	}

	// log out
	if (message.find("has logged out") != std::string::npos) {

		size_t pos1 = message.find("Client ");
		size_t pos2 = message.find(" has logged out");

		if (pos1 != std::string::npos && pos2 != std::string::npos) {

			std::string idStr = message.substr(pos1 + 7, pos2 - (pos1 + 7));
			int id = std::stoi(idStr);

			std::lock_guard<std::mutex> lock(clientDataMutex);
			onlineUserList.erase(std::remove(onlineUserList.begin(), onlineUserList.end(), id), onlineUserList.end());
			groupChatHistory.push_back(message);
		}
		return;
	}


	// DM
	if (message.substr(0, 2) == "P:") {
		size_t pos = message.find(':', 2);
		if (pos != std::string::npos) {
			int sender = std::stoi(message.substr(2, pos - 2));
			std::string content = message.substr(pos + 1);
			std::lock_guard<std::mutex> lock(clientDataMutex);
			privateChatHistory[sender].push_back("Client " + std::to_string(sender) + ": " + content);
		}
		return;
	}

	// rest for broadcast
	{
		std::lock_guard<std::mutex> lock(clientDataMutex);
		groupChatHistory.push_back(message);
	}
}

// loop in the back to listen from the server
void ServerListenLoop() {
	char buffer[DEFAULT_BUFFER_SIZE] = { 0 };
	while (true) {
		int bytes_received = recv(clientSocket, buffer, DEFAULT_BUFFER_SIZE - 1, 0);
		if (bytes_received > 0) {
			buffer[bytes_received] = '\0'; // Null-terminate the received data
			std::string received(buffer);
			MessageDecode(received);
		}
		else if (bytes_received == 0) {
			std::cout << "Connection closed by server." << std::endl;
			break;
		}
		else {
			std::cerr << "Receive failed with error: " << WSAGetLastError() << std::endl;
			break;
		}
	}
}

// Data
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);


void RightAlignedText(const char* text) {
	float windowWidth = ImGui::GetWindowWidth();
	float textWidth = ImGui::CalcTextSize(text).x;
	ImGui::SetCursorPosX(windowWidth - textWidth - 10);
	ImGui::Text("%s", text);
}

// Main code

int main(int, char**)
{
	const char* host = "127.0.0.1"; // Server IP address
	unsigned int port = 65432;

	// Initialize WinSock
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		std::cerr << "WSAStartup failed with error: " << WSAGetLastError() << std::endl;
		return 1;
	}

	// Create a socket
	clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (clientSocket == INVALID_SOCKET) {
		std::cerr << "Socket creation failed with error: " << WSAGetLastError() << std::endl;
		WSACleanup();
		return 1;
	}

	// Resolve the server address and port
	sockaddr_in server_address = {};
	server_address.sin_family = AF_INET;
	server_address.sin_port = htons(port);
	if (inet_pton(AF_INET, host, &server_address.sin_addr) <= 0) {
		std::cerr << "Invalid address/ Address not supported" << std::endl;
		closesocket(clientSocket);
		WSACleanup();
		return 1;
	}

	// Connect to the server
	if (connect(clientSocket, reinterpret_cast<sockaddr*>(&server_address), sizeof(server_address)) == SOCKET_ERROR) {
		std::cerr << "Connection failed with error: " << WSAGetLastError() << std::endl;
		closesocket(clientSocket);
		WSACleanup();
		return 1;
	}

	std::cout << "Connected to the server." << std::endl;

	// use port as ID
	sockaddr_in ClientAddress = {};
	int addressLength = sizeof(ClientAddress);
	getsockname(clientSocket, (sockaddr*)&ClientAddress, &addressLength);
	myID = ntohs(ClientAddress.sin_port);

	// loop to listen to server
	std::thread clientNetThread(ServerListenLoop);
	clientNetThread.detach();

	// Create application window
	//ImGui_ImplWin32_EnableDpiAwareness();
	WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ImGui Example", nullptr };
	::RegisterClassExW(&wc);
	HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Dear ImGui DirectX11 Example", WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);

	// Initialize Direct3D
	if (!CreateDeviceD3D(hwnd))
	{
		CleanupDeviceD3D();
		::UnregisterClassW(wc.lpszClassName, wc.hInstance);
		return 1;
	}

	// Show the window
	::ShowWindow(hwnd, SW_SHOWDEFAULT);
	::UpdateWindow(hwnd);

	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();
	//ImGui::StyleColorsLight();

	// Setup Platform/Renderer backends
	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

	// Our state
	ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

	bool showMenu = true;
	bool showGroupChat = true;
	bool showOnlineUsers = false;
	std::map<int, bool> showPrivateChat;

	std::string personalID = "ID: " + std::to_string(myID);

	// group input
	static char groupInput[256] = "";

	// private input
	std::map<int, std::string> privateInputMap;

	// Main loop
	bool done = false;
	while (!done) {
		// Poll and handle messages (inputs, window resize, etc.)
		// See the WndProc() function below for our to dispatch events to the Win32 backend.
		MSG msg;
		while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
		{
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
			if (msg.message == WM_QUIT)
				done = true;
		}
		if (done)
			break;

		// Handle window being minimized or screen locked
		if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
		{
			::Sleep(10);
			continue;
		}
		g_SwapChainOccluded = false;

		// Handle window resize (we don't resize directly in the WM_SIZE handler)
		if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
		{
			CleanupRenderTarget();
			g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
			g_ResizeWidth = g_ResizeHeight = 0;
			CreateRenderTarget();
		}

		// Start the Dear ImGui frame
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		// menu
		if (showMenu) {
			ImGui::Begin("Menu");
			ImGui::Text("%s", personalID.c_str());
			ImGui::Checkbox("Group Chat", &showGroupChat);
			ImGui::Checkbox("Online Users", &showOnlineUsers);
			ImGui::End();
		}

		// group chat
		if (showGroupChat) {
			ImGui::Begin("Group Chat", &showGroupChat);
			ImGui::BeginChild("GroupChatMessages", ImVec2(0, -50), true);
			{
				std::lock_guard<std::mutex> lock(clientDataMutex);
				for (const auto& message : groupChatHistory) {
					if (message.find("Me:") == 0) {
						RightAlignedText(message.c_str());
					}
					else {
						ImGui::TextWrapped("%s", message.c_str());
					}
				}
			}
			ImGui::EndChild();
			ImGui::InputText("##GroupInput", groupInput, IM_ARRAYSIZE(groupInput));
			ImGui::SameLine();
			if (ImGui::Button("Send Group")) {
				if (strlen(groupInput) > 0) {
					std::string Message = groupInput;
					send(clientSocket, Message.c_str(), (int)Message.size(), 0);
					{
						std::lock_guard<std::mutex> lock(clientDataMutex);
						groupChatHistory.push_back("Me: " + std::string(groupInput));
					}
					strcpy_s(groupInput, "");
				}
			}
			ImGui::End();
		}

		// online user list
		if (showOnlineUsers) {
			ImGui::Begin("Online Users", &showOnlineUsers);
			{
				std::lock_guard<std::mutex> lock(clientDataMutex);
				for (int id : onlineUserList) {
					std::string label = "Client " + std::to_string(id);
					if (ImGui::Selectable(label.c_str())) {
						showPrivateChat[id] = true;
						if (privateChatHistory.find(id) == privateChatHistory.end()) {
							privateChatHistory[id] = std::vector<std::string>();
						}
						if (privateInputMap.find(id) == privateInputMap.end()) {
							privateInputMap[id] = "";
						}
					}
				}
			}
			ImGui::End();
		}

		// private
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
						if (msg.find("Me:") == 0) {
							RightAlignedText(msg.c_str());
						}
						else {
							ImGui::TextWrapped("%s", msg.c_str());
						}
					}
				}
				ImGui::EndChild();
				std::string inputLabel = "##PrivateInput_" + std::to_string(targetId);
				char buffer[256] = "";
				if (privateInputMap.find(targetId) != privateInputMap.end()) {
					strcpy_s(buffer, privateInputMap[targetId].c_str());
				}
				ImGui::InputText(inputLabel.c_str(), buffer, IM_ARRAYSIZE(buffer));
				privateInputMap[targetId] = std::string(buffer);
				ImGui::SameLine();
				std::string buttonLabel = "Send##Private_" + std::to_string(targetId);
				if (ImGui::Button(buttonLabel.c_str())) {
					if (strlen(buffer) > 0) {
						// format"/dm targetID P:myID:message"
						std::string toSend = "/dm " + std::to_string(targetId) + " " + "P:" + std::to_string(myID) + ":" + buffer;
						send(clientSocket, toSend.c_str(), (int)toSend.size(), 0);
						{
							std::lock_guard<std::mutex> lock(clientDataMutex);
							privateChatHistory[targetId].push_back("Me: " + std::string(buffer));
						}
						strcpy_s(buffer, "");
						privateInputMap[targetId] = "";
					}
				}
				ImGui::End();
			}
		}

		// Rendering
		ImGui::Render();
		const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
		g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
		g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

		// Present
		HRESULT hr = g_pSwapChain->Present(1, 0);   // Present with vsync
		//HRESULT hr = g_pSwapChain->Present(0, 0); // Present without vsync
		g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
	}

	// Cleanup
	if (clientSocket != INVALID_SOCKET) {
		closesocket(clientSocket);
		clientSocket = INVALID_SOCKET;
	}

	WSACleanup();

	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	CleanupDeviceD3D();
	::DestroyWindow(hwnd);
	::UnregisterClassW(wc.lpszClassName, wc.hInstance);

	return 0;
}


// Helper functions

bool CreateDeviceD3D(HWND hWnd)
{
	// Setup swap chain
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
	//createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
	D3D_FEATURE_LEVEL featureLevel;
	const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
	HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
	if (res == DXGI_ERROR_UNSUPPORTED) // Try high-performance WARP software driver if hardware is not available.
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

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 message handler
// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return true;

	switch (msg)
	{
	case WM_SIZE:
		if (wParam == SIZE_MINIMIZED)
			return 0;
		g_ResizeWidth = (UINT)LOWORD(lParam); // Queue resize
		g_ResizeHeight = (UINT)HIWORD(lParam);
		return 0;
	case WM_SYSCOMMAND:
		if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
			return 0;
		break;
	case WM_DESTROY:
		::PostQuitMessage(0);
		return 0;
	}
	return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
