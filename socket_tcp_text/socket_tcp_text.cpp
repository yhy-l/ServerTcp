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
#include <unordered_map>
#include <algorithm>
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

	std::vector<char> read_buffer;

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
	std::unordered_map<SOCKET, std::shared_ptr<ClientInfo>> socket_to_client;
	std::unordered_map<int, std::shared_ptr<ClientInfo>> id_to_client;
    std::atomic<int> next_client{ 1 };

public:
    int add_client(SOCKET socket, const std::string& ip, int port)
    {
        std::lock_guard<std::mutex> lock(client_mutex);
        int client_id = next_client++;
        auto client = std::make_shared<ClientInfo>(socket, ip, port, client_id);
        client->connected.store(true);
        
		socket_to_client[socket] = client;
		id_to_client[client_id] = client;

        return client_id;
    } //添加客户端

    void remove_client(int client_id)
    {
        std::lock_guard<std::mutex> lock(client_mutex);
		auto it = id_to_client.find(client_id);
		if (it != id_to_client.end())
		{
			auto client = it->second;
			client->connected.store(false);
			closesocket(client->socket);
			socket_to_client.erase(client->socket);
			id_to_client.erase(it);
		}
    }  //移除客户端

	std::shared_ptr<ClientInfo> get_client_by_socket(SOCKET socket)
	{
		std::lock_guard<std::mutex> lock(client_mutex);
		auto it = socket_to_client.find(socket);
		if (it != socket_to_client.end())
		{
			return it->second;
		}
		return nullptr;
	} //通过socket获取客户端信息

    std::shared_ptr<ClientInfo> get_client(int client_id)
    {
        std::lock_guard<std::mutex> lock(client_mutex);
        auto it = id_to_client.find(client_id);
        if (it != id_to_client.end())
        {
            return it->second;
        }
        return nullptr;
	}  //通过id获取客户端信息

    std::vector<std::shared_ptr<ClientInfo>> get_all_clients()
    {
        std::lock_guard<std::mutex> lock(client_mutex);
        std::vector<std::shared_ptr<ClientInfo>> result;
        for (auto& pair : id_to_client)
        {
            if (pair.second->connected.load())
            {
                result.push_back(pair.second);
            }
        }
        return result;
	}  //获取所有在线客户端信息

    std::vector<SOCKET> get_all_sockets()
    {
        std::lock_guard<std::mutex> lock(client_mutex);
        std::vector<SOCKET> result;
        for (auto& pair : socket_to_client)
        {
            if (pair.second->connected.load())
            {
                result.push_back(pair.first);
            }
        }
        return result;
	}  //获取所有在线客户端socket

    int get_online_count()
    {
        std::lock_guard<std::mutex> lock(client_mutex);
        int count = 0;
        for (auto& client : id_to_client)
        {
			if (client.second->connected.load())
			{
				count++;
			}
        }
        return count;
    }  // 获取当前在线客户端数量

    int get_total_count()
    {
		std::lock_guard<std::mutex> lock(client_mutex);
		return static_cast<int>(id_to_client.size());
    }

	std::vector<char> format_message(const std::string& message)
	{
		uint32_t length = htonl(static_cast<uint32_t>(message.size()));
		std::vector<char> buffer(4 + message.size());
		memcpy(buffer.data(), &length, 4);
		memcpy(buffer.data() + 4, message.data(), message.size());
		return buffer;
	}  //格式化消息，添加长度前缀

    bool send_to_client(int client_id, const std::string& message)
    {
        auto client = get_client(client_id);
        if (!client || !client->connected.load())
        {
            return false;
        }

		std::vector<char> buffer = format_message(message);
		int total_sent = 0;
		int remaining = static_cast<int>(buffer.size());
        while (remaining > 0)
        {
            int sent = send(client->socket, buffer.data() + total_sent, remaining, 0);
            if (sent <= 0)
            {
                return false;
            }
            total_sent += sent;
            remaining -= sent;
        }
        return true;
    }  //向一个客户端发送消息

    void broadcast(const std::string& message, int exclude_client_id = -1)
    {
        std::vector<int> client_id;
        {
			std::lock_guard<std::mutex> lock(client_mutex);
            for (auto& client : id_to_client)
            {
                if (client.second->connected.load() && client.second->id != exclude_client_id)
                {
                    client_id.push_back(client.second->id);
                }
            }
        }

        for (auto& n : client_id)
        {
			send_to_client(n, message);
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

    int opt = 1;
	setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

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

void process_message(int client_id, const std::string& message, const std::string& client_ip, int client_port)
{
	perf_monitor.on_message(static_cast<int>(message.size()));
	{
		std::lock_guard<std::mutex> lock(console_mutex);
        std::cout << "[客户端" << client_id << "(" << client_ip << ":" << client_port << ")] 说: "
            << message << std::endl;
	}

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

        auto client = client_manager.get_client(client_id);
        if (client && client->connected.load()) {
			client_manager.send_to_client(client_id, list_msg);
        }
    }
    else if (message.find("/sendto ") == 0) {
        // 格式: /sendto 客户端ID 消息
        size_t space1 = message.find(' ', 8);
        if (space1 != std::string::npos) {
            int target_id = std::stoi(message.substr(8, space1 - 8));
            std::string private_msg = message.substr(space1 + 1);

            std::string private_msg_formatted = "[私聊] 客户端"
                + std::to_string(client_id)
                + " 对你说: " + private_msg;

            if (client_manager.send_to_client(target_id, private_msg_formatted)) {
            }
            else {
                auto client = client_manager.get_client(client_id);
                if (client && client->connected.load()) {
					client_manager.send_to_client(client_id, "发送失败，目标客户端可能不存在或已断开连接");
                }
            }
        }
    }
    else {
        // 普通消息，广播给所有人
        std::string broadcast_msg = "客户端" + std::to_string(client_id)
            + "(" + client_ip + ":" + std::to_string(client_port)
            + ") 说: " + message;

        client_manager.broadcast(broadcast_msg, client_id);

        // 给发送者回复确认
        auto client = client_manager.get_client(client_id);
        if (client && client->connected.load()) {
            std::string response = "服务器收到消息！";
			client_manager.send_to_client(client_id, response);
        }
    }
}

void handle_client_data(SOCKET client_socket)
{
	auto client = client_manager.get_client_by_socket(client_socket);
	if (!client || !client->connected.load())
	{
		return;
	}

    char temp_buffer[2000];
	int bytes_received = recv(client_socket, temp_buffer, sizeof(temp_buffer), 0);

    const int MAX_BUFFER_SIZE = 8 * 1024 * 1024;     // 8MB，防止内存耗尽

    if (bytes_received > 0)
    {
        if (client->read_buffer.size() + bytes_received > MAX_BUFFER_SIZE) {
            std::cout << "客户端" << client->id << "缓冲区过大，断开连接" << std::endl;
            client_manager.remove_client(client->id);
            return;
        }

        client->read_buffer.insert(client->read_buffer.end(), temp_buffer, temp_buffer + bytes_received);

        while (client->read_buffer.size() >= 4)
        {
            uint32_t length = 0;
            memcpy(&length, client->read_buffer.data(), 4);
            length = ntohl(length);

            if (client->read_buffer.size() < 4 + length)
            {
                break; // 等待更多数据
            }

            std::string message(client->read_buffer.begin() + 4, client->read_buffer.begin() + 4 + length);
            client->read_buffer.erase(client->read_buffer.begin(), client->read_buffer.begin() + 4 + length);

            int client_id = client->id;
            std::string client_ip = client->ip;
            int client_port = client->port;

            if (!client->connected.load(std::memory_order_acquire))
                continue; // 已断开，跳过处理

            thread_pool.enqueue([client_id, message, client_ip, client_port]()
                {
                    process_message(client_id, message, client_ip, client_port);
                });
        }
    }
    else if (bytes_received == 0)
    {
		std::lock_guard<std::mutex> lock(console_mutex);
		std::cout << "客户端 [" << client->id << "] " << client->ip << ":" << client->port << " 已断开连接" << std::endl;
		client_manager.remove_client(client->id);
		perf_monitor.on_client_count(client_manager.get_online_count());
	}
	else
	{
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK)
        {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << "recv错误，客户端" << client->id << "(" << client->ip << ":" << client->port
                << ") 断开连接，错误码: " << error << std::endl;
            client_manager.remove_client(client->id);
            perf_monitor.on_client_count(client_manager.get_online_count());
        }
    }
}

void run_select_server(SOCKET serverSocket)
{
	fd_set read_fds;
    int max_fd = 0;

	std::cout << "select服务器已启动，等待客户端连接..." << std::endl;

    while (server_running.load())
    {
        FD_ZERO(&read_fds);

        FD_SET(serverSocket, &read_fds);
        if (serverSocket > max_fd)
        {
            max_fd = serverSocket;
        }

        auto client_sockets = client_manager.get_all_sockets();
        for (SOCKET client_socket : client_sockets)
        {
            FD_SET(client_socket, &read_fds);
            if (client_socket > max_fd)
            {
                max_fd = client_socket;
            }
        }

        if (max_fd == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; // 100ms

        int result = select(0, &read_fds, nullptr, nullptr, &timeout);

        if (result > 0)
        {
            if (FD_ISSET(serverSocket, &read_fds))
            {
                sockaddr_in clientAddr;
                int clientAddrSize = sizeof(clientAddr);
                SOCKET clientSocket = accept(serverSocket, (sockaddr*)&clientAddr, &clientAddrSize);

                if (clientSocket != INVALID_SOCKET)
                {
                    char client_ip[16];
                    inet_ntop(AF_INET, &clientAddr.sin_addr, client_ip, sizeof(client_ip));
                    int client_port = ntohs(clientAddr.sin_port);

                    int client_id = client_manager.add_client(clientSocket, client_ip, client_port);
                    perf_monitor.on_client_count(client_manager.get_online_count());

                    {
                        std::lock_guard<std::mutex> lock(console_mutex);
                        std::cout << "新客户端 [" << client_id << "] "
                            << client_ip << ":" << client_port << std::endl;
                        std::cout << "当前在线客户端: " << client_manager.get_online_count() << std::endl;
                    }
                }
            }

            for (SOCKET client_socket : client_sockets)
            {
                if (FD_ISSET(client_socket, &read_fds))
                {
                    handle_client_data(client_socket);
                }
            }
        }
        else if (result == 0)
        {
            // 超时，继续循环
        }
        else
        {
            int error = WSAGetLastError();
            if (error != WSAEINTR)
            {
                std::cout << "select错误，错误码: " << error << std::endl;
            }
        }
    }

	auto all_clients = client_manager.get_all_clients();
	for (auto& client : all_clients)
	{
		if (client->connected.load())
		{
			closesocket(client->socket);
			client->connected.store(false);
		}
	}

    {
		std::lock_guard<std::mutex> lock(console_mutex);
		std::cout << "服务器关闭..." << std::endl;
    }
}

int main()
{

    initialize_WSA();   //初始化网络库

    SOCKET serverSocket = initializeSocketAndListen();
    if (serverSocket == INVALID_SOCKET)
        return -1;

    std::thread select_thread(run_select_server,serverSocket);
 
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

    select_thread.join();

    perf_monitor.print_stats();

    WSACleanup();
    std::cout << "关闭成功！" << std::endl;

    std::cout << "结束" << std::endl;
    return 0;
}
