// client.cpp - 测试服务器的客户端
#define WIN32_LEAN_AND_MEAN
#include <iostream>
#include <string>  
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <mutex>
#include <atomic>
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

        int bytesSent = send(clientSocket, message.c_str(), static_cast<int>(message.length()), 0);

        if (bytesSent > 0)
        {
            std::cout << "已发送消息: " << message << std::endl;
        }
        else
        {
            std::cout << "发送消息失败" << std::endl;
        }
    }

}

void receiveFromServer(SOCKET clientSocket)
{
    while (connected)
    {
        char buffer[1024];
        int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

        if (bytesReceived > 0)
        {
            buffer[bytesReceived] = '\0';
            std::cout << "服务器回复: " << buffer << std::endl;
        }
        else if (bytesReceived == 0)
        {
            std::cout << "服务器关闭了连接" << std::endl;
        }
        else
        {
            std::cout << "接收回复失败" << std::endl;
            break;
        }
    }
}

int main()
{
    initialize_WSA();

    SOCKET clientSocket = connectToServer();

    std::thread send(sendToServer, clientSocket);
    std::thread receive(receiveFromServer, clientSocket);


    // 关闭连接
    std::cout << "正在关闭连接..." << std::endl;
    send.join();
    receive.join();
    connected = false;
    closesocket(clientSocket);
    WSACleanup();

    return 0;
}