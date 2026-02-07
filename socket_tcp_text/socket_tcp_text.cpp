#define WIN32_LEAN_AND_MEAN
#include <iostream>
#include <winsock2.h>    
#include <ws2tcpip.h>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <ctime>
#include <memory>
#include <queue>
#include <condition_variable>
#include <functional>
#pragma comment(lib, "ws2_32.lib")

class ThreadPool
{
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop = false;

public:
    ThreadPool(size_t threads = std::thread::hardware_concurrency())
    {
        for (size_t i = 0; i < threads; ++i)
        {
            workers.emplace_back([this]()
                {
                    while (true)
                    {
                        std::function<void()> task;
                        {
                            std::unique_lock<std::mutex> lock(queue_mutex);
                            condition.wait(lock, [this]
                                {
                                    return stop || !tasks.empty();
                                });
                            if (stop && tasks.empty()) return;
                            task = std::move(tasks.front());
                            tasks.pop();
                        }
                        task();
                    }
                });
        }
    }

    template<class F>
    void enqueue(F&& f)
    {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            tasks.emplace(std::forward<F>(f));
        }
        condition.notify_one();
    }

    ~ThreadPool()
    {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread& worker : workers)
        {
            worker.join();
        }
    }

    size_t queue_size()
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        return tasks.size();
    }

    size_t worker_count()
    {
        return workers.size();
    }
};

class PerformanceMonitor
{
private:
    std::atomic<long long> total_message{ 0 };
    std::atomic<long long> total_bytes{ 0 };
    std::atomic<int> peak_clients{ 0 };
    time_t start_time;

public:
    PerformanceMonitor()
    {
        start_time = time(nullptr);
    }

    void on_message(int bytes)
    {
        total_message.fetch_add(1);
        total_bytes.fetch_add(bytes);
    }

    void on_client_count(int count)
    {
        int current_peak = peak_clients.load();
        while (count > current_peak)
        {
            if (peak_clients.compare_exchange_weak(current_peak, count))
            {
                break;
            }
        }
    }

    void print_stats()
    {
        time_t now = time(nullptr);
        double uptime = difftime(now, start_time);
        std::cout << "\n=== 服务器统计 ===" << std::endl;
        std::cout << "运行时间: " << uptime << "秒" << std::endl;
        std::cout << "总消息数: " << total_message.load() << std::endl;
        std::cout << "总字节数: " << total_bytes.load() << " bytes" << std::endl;
        std::cout << "平均消息率: " << total_message.load() / uptime << " 消息/秒" << std::endl;
        std::cout << "峰值连接数: " << peak_clients.load() << std::endl;
    }
};

struct ClientInfo
{
    SOCKET socket;
    std::string ip;
    int port;
    int id;
    std::atomic<bool> connected;
    time_t connect_time;

    ClientInfo(SOCKET sock, std::string ip_addr, int p, int client_id)
        :socket(sock), ip(ip_addr), port(p), id(client_id),connected(true)
    {
        connect_time = time(nullptr);
    }
};

class ClientManager
{
private:
    std::mutex client_mutex;
    std::vector<std::shared_ptr<ClientInfo>> clients;
    std::atomic<int> next_client{ 1 };

public:
    int add_client(SOCKET socket, const std::string& ip, int port)
    {
        std::lock_guard<std::mutex> lock(client_mutex);
        int client_id = next_client++;
        auto client = std::make_shared<ClientInfo>(socket, ip, port, client_id);
        client->connected.store(true);
        clients.push_back(client);

        return client_id;
    } //添加客户端

    void remove_client(int client_id)
    {
        std::lock_guard<std::mutex> lock(client_mutex);
        for (auto it = clients.begin(); it != clients.end(); it++)
        {
            if ((*it)->id == client_id)
            {
                (*it)->connected.store(false);
                closesocket((*it)->socket);
                clients.erase(it);
                break;
            }
        }
    }  //移除客户端

    std::shared_ptr<ClientInfo> get_client(int client_id)
    {
        std::lock_guard<std::mutex> lock(client_mutex);

        for (auto& client : clients)
        {
            if (client->id == client_id)
            {
                return client;
            }
        }
        std::cout << "无法" << client_id << "找到客户端" << std::endl;
        return nullptr;
    }  //获取一个客户端信息

    std::vector<std::shared_ptr<ClientInfo>> get_all_clients()
    {
        std::lock_guard<std::mutex> lock(client_mutex);
        return clients;
    } //获取所有客户端

    int get_online_count()
    {
        std::lock_guard<std::mutex> lock(client_mutex);
        int count = 0;
        for (auto& client : clients)
        {
            if (client->connected.load())
            {
                count++;
            }
        }
        return count;
    }  // 获取当前在线客户端数量

    int get_total_count()
    {
        std::lock_guard<std::mutex> lock(client_mutex);
        return static_cast<int>(clients.size());
    }  //获取所有客户端数量

    bool send_to_client(int client_id, const std::string& message)
    {
        auto client = get_client(client_id);
        if (!client || !client->connected.load())
        {
            return false;
        }

        int sent = send(client->socket, message.c_str(), static_cast<int>(message.length()), 0);
        return sent > 0;
    }  //向一个客户端发送消息

    void broadcast(const std::string& message, int exclude_client_id = -1)
    {
        std::lock_guard<std::mutex> lock(client_mutex);
        for (auto& client : clients)
        {
            if (client->connected.load() && client->id != exclude_client_id)
            {
                send(client->socket, message.c_str(), static_cast<int>(message.length()), 0);
            }
        }
    }  //广播
};


std::mutex console_mutex;
std::atomic<bool> server_running(true);
ClientManager client_manager;
PerformanceMonitor perf_monitor;
ThreadPool thread_pool(4);


void initialize_WSA()
{
    std::cout << "初始化winsocket..." << std::endl;
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0)
    {
        std::cout << "网络库初始化失败！" << std::endl;
    }

    std::cout << "网络库初始化成功！" << std::endl;
}

SOCKET initializeSocketAndListen ()
{
    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == INVALID_SOCKET)
    {
        std::lock_guard<std::mutex> lock(console_mutex);
        std::cout << "创建socket失败！" << std::endl;
        WSACleanup();
        return INVALID_SOCKET;
    }
    else
    {
        std::lock_guard<std::mutex> lock(console_mutex);
        std::cout << "socket创建成功!" << std::endl;
    }


    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(8080);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    int bindResult = bind(serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr));
    if (bindResult == SOCKET_ERROR)
    {
        std::lock_guard<std::mutex> lock(console_mutex);
        std::cout << "bind创建错误！" << std::endl;
    }
    else
    {
        std::lock_guard<std::mutex> lock(console_mutex);
        std::cout << "bind创建成功!" << std::endl;
    }

    int listenResult = listen(serverSocket, 5);
    if (listenResult == SOCKET_ERROR)
    {
        std::lock_guard<std::mutex> lock(console_mutex);
        std::cout << "listen创建失败！" << std::endl;
        closesocket(serverSocket);
        WSACleanup();
        return INVALID_SOCKET;
    }
    else
    {
        std::lock_guard<std::mutex> lock(console_mutex);
        std::cout << "listen创建成功!" << std::endl;
    }

    return serverSocket;
}

void handleClient(SOCKET clientSocket, std::string clientIP,int clientPort, int clientId)
{
    {
        std::lock_guard<std::mutex> lock(console_mutex);
        std::cout << "开始接受客户端的消息..." << std::endl;
    }

    std::vector<char> buffer(1024);

    while (server_running.load())
    {
        int bytesRecevied = recv(clientSocket, buffer.data(), static_cast<int>(buffer.size()) - 1, 0);
        if (bytesRecevied > 0)
        {
            perf_monitor.on_message(bytesRecevied);

            buffer[bytesRecevied] = '\0';
            std::string message = buffer.data();

            {
                std::lock_guard<std::mutex> lock(console_mutex);
                std::cout << "ip： " << clientIP << " : " << buffer.data() << std::endl;
            }

            std::string response = "服务器收到消息！";
            send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);

            if (message == "/list") {
                std::string list_msg = "在线客户端 ("
                    + std::to_string(client_manager.get_online_count())
                    + "个):\n";
                auto clients = client_manager.get_all_clients();
                for (auto& client : clients) {
                    if (client->connected.load()) {
                        list_msg += "  [客户端" + std::to_string(client->id)
                            + "] " + client->ip + ":" + std::to_string(client->port)
                            + "\n";
                    }
                }
                send(clientSocket, list_msg.c_str(), static_cast<int>(list_msg.length()), 0);
            }
            else if (message.find("/sendto ") == 0) {
                // 格式: /sendto 客户端ID 消息
                size_t space1 = message.find(' ', 8);
                if (space1 != std::string::npos) {
                    int target_id = std::stoi(message.substr(8, space1 - 8));
                    std::string private_msg = message.substr(space1 + 1);

                    std::string private_msg_formatted = "[私聊] 客户端"
                        + std::to_string(clientId)
                        + " 对你说: " + private_msg;

                    if (client_manager.send_to_client(target_id, private_msg_formatted)) {
                        send(clientSocket, "私聊发送成功", 12, 0);
                    }
                    else {
                        send(clientSocket, "私聊发送失败，客户端可能已离线", 30, 0);
                    }
                }
              }

        }
        else if (bytesRecevied == 0)
        {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << "ip : " << clientIP << " 断开！" << std::endl;
            break;
        }
        else
        {
            int error = WSAGetLastError();
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << "接受错误！" << std::endl;
            break;
        }
    }

    closesocket(clientSocket);
    client_manager.remove_client(clientId);

    {
        std::lock_guard<std::mutex> lock(console_mutex);
        std::cout << "客户端：" << clientIP << " 断开！" << std::endl;
        std::cout << "剩余客户端：" << client_manager.get_online_count() << std::endl;
    }
}

void acceptClient(SOCKET serverSocket)
{
    while (server_running.load())
    {
        sockaddr_in clientAddr;
        int clientAddrSize = sizeof(clientAddr);

        SOCKET clientSocket = accept(serverSocket, (sockaddr*)&clientAddr, &clientAddrSize);
        if (clientSocket == INVALID_SOCKET)
        {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << "accept连接错误！" << std::endl;
            break;
        }
        else
        {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << "accept连接成功！" << std::endl;
        }

        char clientIP[16];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, sizeof(clientIP));
        int clientPort = ntohs(clientAddr.sin_port);

        int clientId = client_manager.add_client(clientSocket, clientIP, ntohs(clientAddr.sin_port));

        perf_monitor.on_client_count(client_manager.get_online_count());
        

        {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << "连接的客户端ip为： " << clientIP << std::endl;
            std::cout << "连接的客户端port为： " << clientPort << std::endl;
            std::cout << "当前客户端的数量为： " << client_manager.get_online_count() << std::endl;
        }

        std::string join_msg = "客户端" + std::to_string(clientId)
            + "(" + clientIP + ":" + std::to_string(clientPort)
            + ") 加入聊天室";

        client_manager.broadcast(join_msg, clientId);

        thread_pool.enqueue([clientSocket, clientIP, clientPort, clientId]()
            {
                handleClient(clientSocket, clientIP, clientPort, clientId);
                perf_monitor.on_client_count(client_manager.get_online_count());
            });

    }
}

int main()
{

    initialize_WSA();   //初始化网络库

    SOCKET serverSocket = initializeSocketAndListen();
    if (serverSocket == INVALID_SOCKET)
        return -1;

    std::thread acceptThread(acceptClient, serverSocket);
 
    std::string command;
    while (server_running)
    {
        std::getline(std::cin, command);
        if (command == "stats")
        {
            perf_monitor.print_stats();
        }
        else if (command == "stop")
        {
            server_running.store(false);
            closesocket(serverSocket);
            break;
        }
    }

    acceptThread.join();

    perf_monitor.print_stats();

    WSACleanup();
    std::cout << "关闭成功！" << std::endl;

    std::cout << "结束" << std::endl;
    return 0;
}
