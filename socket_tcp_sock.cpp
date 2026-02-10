// client.cpp - 测试服务器的客户端
#define WIN32_LEAN_AND_MEAN
#include <iostream>
#include <string>  
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#pragma comment(lib, "ws2_32.lib")

std::atomic<bool> connected(true);

void initialize_WSA()
{
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0)
    {
        std::cout << "初始化Winsock失败！" << std::endl;
    }
}

SOCKET connectToServer()
{
    SOCKET clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket == INVALID_SOCKET)
    {
        std::cout << "创建socket失败" << std::endl;
        WSACleanup();
        return 1;
    }
    std::cout << "创建socket成功" << std::endl;

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(8080);  
    if (inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr) <= 0)
    {
        std::cout << "IP地址转换失败！" << std::endl;
        closesocket(clientSocket);
        WSACleanup();
        return 1;
    }

    // 4. 连接服务器
    std::cout << "正在连接服务器..." << std::endl;
    int connectResult = connect(clientSocket, (sockaddr*)&serverAddr, sizeof(serverAddr));

    if (connectResult == SOCKET_ERROR)
    {
        std::cout << "连接服务器失败！错误码: " << WSAGetLastError() << std::endl;
        std::cout << "请确保服务器已经启动" << std::endl;
        closesocket(clientSocket);
        WSACleanup();
        return 1;
    }

    std::cout << "已连接到服务器" << std::endl;
    std::cout << "==================================" << std::endl;
    return clientSocket;
}

void sendToServer(SOCKET clientSocket)
{
    while (connected)
    {
        std::string message;
        std::cout << "请输入要发送的消息: ";
        std::getline(std::cin, message);

		uint32_t length = htonl(static_cast<uint32_t>(message.length()));
		std::vector<char> send_buffer(4 + message.length());
		memcpy(send_buffer.data(), &length, 4);
		memcpy(send_buffer.data() + 4, message.c_str(), message.length());

		int total_sent = 0;
		int to_send = static_cast<int>(send_buffer.size());

        while (total_sent < to_send)
        {
            int sent = send(clientSocket, send_buffer.data() + total_sent, to_send - total_sent, 0);
            if (sent <= 0)
            {
                std::cout << "发送消息失败！错误码: " << WSAGetLastError() << std::endl;
                return;
            }
            total_sent += sent;
        }

		if (total_sent == to_send)
		{
			std::cout << "消息发送成功！ " << message.length() << "字节" << std::endl;
		}
    }

}

void receiveFromServer(SOCKET clientSocket)
{
    std::vector<char> buffer;
    while (connected)
    {
        char temp_buffer[2000];
        int bytes_received = recv(clientSocket, temp_buffer, sizeof(temp_buffer), 0);

        if (bytes_received > 0)
        {
            buffer.insert(buffer.end(), temp_buffer, temp_buffer + bytes_received);

            while (buffer.size() >= 4)
            {
                uint32_t length = 0;
                memcpy(&length, buffer.data(), 4);
                length = ntohl(length);

                if (buffer.size() < 4 + length)
                {
                    break; // 等待更多数据
                }

                std::string message(buffer.begin() + 4, buffer.begin() + 4 + length);
                buffer.erase(buffer.begin(), buffer.begin() + 4 + length);

                std::cout << "收到服务器消息: " << message << std::endl;

            }
        }
        else if (bytes_received == 0)
        {
            std::cout << "服务器已关闭连接" << std::endl;
            connected = false;
            break;
        }
        else
        {
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK && error != WSAEINTR)
            {
                std::cout << "接收消息失败！错误码: " << error << std::endl;
                connected = false;
                break;
            }
        }
    }
}

int main()
{
    initialize_WSA();

    SOCKET clientSocket = connectToServer();

    std::thread send(sendToServer, clientSocket);
    std::thread receive(receiveFromServer, clientSocket);

    send.join();
    receive.join();

    // 关闭连接
    std::cout << "正在关闭连接..." << std::endl;

    connected = false;
    closesocket(clientSocket);
    WSACleanup();

    return 0;
}