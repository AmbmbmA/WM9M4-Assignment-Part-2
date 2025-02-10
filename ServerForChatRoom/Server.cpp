#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

std::map<int, SOCKET> clientSocketsIP;  // all client sockets with key using their IP
std::mutex clientMutex;

std::vector<std::string> groupChatHistory;


// broadcast the message
void MessageBroadcast(std::string& message, SOCKET SoketBan) {

	std::lock_guard<std::mutex> lock(clientMutex); // lock the map access
	groupChatHistory.push_back(message);
	for (auto soket : clientSocketsIP) {
		if (soket.second != SoketBan) {
			send(soket.second, message.c_str(), (int)message.size(), 0);
		}
	}
}


// specifier for DM
const std::string DMsp = "/dm";

// DM fomat: "/dm id message"

void client(SOCKET clientSocket, int clientID) {
	{
		std::lock_guard<std::mutex> lock(clientMutex); // lock the map access
		clientSocketsIP[clientID] = clientSocket;

		// resend all history 
		for (auto& message : groupChatHistory) {
			send(clientSocket, message.c_str(), (int)message.size(), 0);
			std::string temp = "\n"; // manual gap cause too fast
			send(clientSocket, temp.c_str(), (int)temp.size(), 0);
		}
	}

	char buffer[1024];
	bool stop = false;
	while (!stop) {
		memset(buffer, 0, sizeof(buffer));
		int bytes = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
		if (bytes > 0) {
			buffer[bytes] = '\0';
			std::string message(buffer);
			// detect if it is DM
			if (message.substr(0, DMsp.size()) == DMsp) {

				bool check = true;
				std::string wrong = "Wrong Format";

				size_t gap1 = message.find(' ');
				if (gap1 == std::string::npos) {
					send(clientSocket, wrong.c_str(), (int)wrong.size(), 0);
					check = false;
				}
				size_t gap2 = message.find(' ', gap1 + 1);
				if (gap2 == std::string::npos) {
					send(clientSocket, wrong.c_str(), (int)wrong.size(), 0);
					check = false;
				}

				if (check) {
					std::string idString = message.substr(gap1 + 1, gap2 - gap1 - 1);
					int targetID = std::stoi(idString);
					if (clientSocketsIP.find(targetID) == clientSocketsIP.end()) {
						std::string error = "There is no Client " + idString;
						send(clientSocket, error.c_str(), (int)error.size(), 0);
					}
					else {
						std::string dmMessage = message.substr(gap2 + 1);
						send(clientSocketsIP[targetID], dmMessage.c_str(), (int)dmMessage.size(), 0);
						std::cout << "DM from " << clientID << " to " << targetID << ": " << dmMessage << std::endl;
					}
				}
			}
			else {

				std::string bcMessage = "Client " + std::to_string(clientID) + ": " + message;
				std::cout << "Broadcast from " << clientID << ": " << message << std::endl;
				MessageBroadcast(bcMessage, clientSocket);
			}
		}
		else {
			stop = true;
		}
	}

	{
		std::lock_guard<std::mutex> lock(clientMutex);
		clientSocketsIP.erase(clientID);
	}
	std::string endMessage = "Client " + std::to_string(clientID) + " has logged out.";
	MessageBroadcast(endMessage, clientSocket);
	closesocket(clientSocket);
}

int server() {
	// Step 1: Initialize WinSock
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		std::cerr << "WSAStartup failed with error: " << WSAGetLastError() << std::endl;
		return 1;
	}

	// Step 2: Create a socket
	SOCKET server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server_socket == INVALID_SOCKET) {
		std::cerr << "Socket creation failed with error: " << WSAGetLastError() << std::endl;
		WSACleanup();
		return 1;
	}

	// Step 3: Bind the socket
	sockaddr_in server_address = {};
	server_address.sin_family = AF_INET;
	server_address.sin_port = htons(65432);  // Server port
	server_address.sin_addr.s_addr = INADDR_ANY; // Accept connections on any IP address

	if (bind(server_socket, (sockaddr*)&server_address, sizeof(server_address)) == SOCKET_ERROR) {
		std::cerr << "Bind failed with error: " << WSAGetLastError() << std::endl;
		closesocket(server_socket);
		WSACleanup();
		return 1;
	}

	// Step 4: Listen for incoming connections
	if (listen(server_socket, SOMAXCONN) == SOCKET_ERROR) {
		std::cerr << "Listen failed with error: " << WSAGetLastError() << std::endl;
		closesocket(server_socket);
		WSACleanup();
		return 1;
	}

	std::cout << "Server is listening on port 65432..." << std::endl;

	while (true) {
		sockaddr_in client_address = {};
		int client_address_len = sizeof(client_address);
		SOCKET client_socket = accept(server_socket, (sockaddr*)&client_address, &client_address_len);
		if (client_socket == INVALID_SOCKET) {
			std::cerr << "Accept failed: " << WSAGetLastError() << std::endl;
			closesocket(server_socket);
			WSACleanup();
			return 1;
		}

		char client_ip[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &client_address.sin_addr, client_ip, INET_ADDRSTRLEN);
		std::cout << "Accepted connection from " << client_ip << ":" << ntohs(client_address.sin_port) << std::endl;

		std::string loginMessage = "Client " + std::to_string(ntohs(client_address.sin_port)) + " has logged in!";
		MessageBroadcast(loginMessage, -1);

		std::thread t(client, client_socket, ntohs(client_address.sin_port)); // port as id
		t.detach();
	}
	closesocket(server_socket);
	WSACleanup();
	return 0;
}

int main() {
	return server();
}
