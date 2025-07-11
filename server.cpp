#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>

#include <thread>
#include <vector>
#include <mutex>
#include <sstream>
#include <map>
#include <queue>

#include "nlohmann/json.hpp"

#pragma comment(lib, "ws2_32.lib")

using json = nlohmann::json;

struct Command{
    int client_id;
    std::string message;
};

struct State{
    float x;
    float y;
    float velocity;
    float direction;
    int r, g, b;
};

const int PORT = 55555;
const int BUFFER_SIZE = 1024;

int next_client_id = 0;

std::mutex clients_mutex;
std::mutex cout_mutex;
std::mutex queue_mutex;
std::mutex state_mutex;

std::condition_variable queue_cv;
std::queue<Command> command_queue;


std::map<int, SOCKET> clients;
std::map<int, State> client_states;




void print_state(const State& state) {
    // Clear screen and move cursor to top-left
    std::cout << "\033[2J\033[H";

    std::cout << "== CLIENT STATE ==\n";
    std::cout << "x:         " << state.x << "\n";
    std::cout << "y:         " << state.y << "\n";
    std::cout << "direction: " << state.direction << "\n";
    std::cout << "color:     (" << state.r << ", " << state.g << ", " << state.b << ")\n";
}

void client_listener(int client_id, SOCKET client_socket){
    char buffer[BUFFER_SIZE];

    while(true){
        int bytes_recieved = recv(client_socket, buffer, BUFFER_SIZE -1, 0);
        if(bytes_recieved <= 0){
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                // std::lock_guard<std::mutex> lock2(cout_mutex);
                // std::cout << "Client " << client_id << " disconnected.\n";
                closesocket(client_socket);
                clients.erase(client_id);
            }
            break;
        }

        buffer[bytes_recieved] = '\0';

        std::cout << buffer << "\n";

        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            command_queue.push(Command{client_id, buffer});
        }
        
        queue_cv.notify_one();
    }
}

void process_commands() {
    while (true) {
        Command cmd;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cv.wait(lock, [] { return !command_queue.empty(); });
            cmd = command_queue.front();
            command_queue.pop();
        }

        try {
            auto parsed = json::parse(cmd.message);
            std::string type = parsed.value("type", "");

            if (type == "CONNECT") {
                json response_json = {
                    {"type", "response"},
                    {"status", "SUCCESS"}
                };
                std::string response_str = response_json.dump() + "\n";
                {
                    std::lock_guard<std::mutex> lock(clients_mutex);
                    send(clients[cmd.client_id], response_str.c_str(), static_cast<int>(response_str.size()), 0);
                }

            } else if (type == "delta" || type == "snapshot") {
                json state_json = parsed["state"];
                std::lock_guard<std::mutex> lock(state_mutex);
                State &state = client_states[cmd.client_id];

                if (state_json.contains("x")) state.x = state_json["x"];
                if (state_json.contains("y")) state.y = state_json["y"];
                if (state_json.contains("velocity")) state.velocity = state_json["velocity"];
                if (state_json.contains("direction")) state.direction = state_json["direction"];

                if (state_json.contains("color") && state_json["color"].is_array()) {
                    auto c = state_json["color"];
                    if (c.size() == 3) {
                        state.r = c[0];
                        state.g = c[1];
                        state.b = c[2];
                    }
                }

            } else {
                json fail_resp = {
                    {"type", "response"},
                    {"status", "FAIL"}
                };
                std::string fail_str = fail_resp.dump() + "\n";
                send(clients[cmd.client_id], fail_str.c_str(), static_cast<int>(fail_str.size()), 0);
            }

        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cerr << "Failed to parse client message: " << e.what() << "\n";
        }
    }
}


void accept_clients(SOCKET server_socket){
    while(true){
        sockaddr_in client_addr;
        int addr_len = sizeof(client_addr);
        SOCKET client_socket = accept(server_socket, (sockaddr*)&client_addr, &addr_len);
        if(client_socket == INVALID_SOCKET){
            std::cerr << "Failed to accept client\n";
            continue;
        }

        std::lock_guard<std::mutex> lock(clients_mutex);
        int client_id = next_client_id++;
        clients[client_id] = client_socket;
        
        // std::lock_guard<std::mutex> lock2(cout_mutex); 
        // std::cout << "Client " << client_id << " connected!\n";
        
        std::thread listener_thread(client_listener, client_id, client_socket);
        listener_thread.detach();
    }
}


int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }


    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(server_socket == INVALID_SOCKET){
        std::cerr << "Socket creation failed\n";
        WSACleanup();
        return 1;
    }



    // Setup address
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    
    // Bind socket
    if (bind(server_socket, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Bind failed\n";
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    // Listen
    if (listen(server_socket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Listen failed\n";
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }
    
    // {
    //     std::lock_guard<std::mutex> lock(cout_mutex);  
    //     std::cout << "Server listening on port " << PORT << "...\n";
    // }

    std::thread command_processor(process_commands);
    command_processor.detach();

    std::thread accept_thread(accept_clients, server_socket);
    accept_thread.detach();

    //Main input loop
    std::string line;
    while(true){
        std::getline(std::cin, line);
        if(line == "exit") break;

        std::istringstream iss(line);
        std::string cmd;
        int id;
        std::string message;

        iss >> cmd >> id;
        std::getline(iss, message);

        if (!message.empty() && message[0] == ' ') message.erase(0, 1);
        if (!message.empty() && message[0] == '"') message.erase(0, 1);
        if (!message.empty() && message.back() == '"') message.pop_back();  

        std::lock_guard<std::mutex> lock(clients_mutex);
        // std::lock_guard<std::mutex> lock2(cout_mutex);
        if (cmd == "send"){
           
            if(clients.count(id)){
                send(clients[id], message.c_str(), static_cast<int>(message.length()), 0);
                // std::cout << "Sent to client " << id <<" "<< message << "\n";
            } else {
                // std::cout << "Client ID " << id << " not found.\n";
            }
        } else {
            // std::cout << "Unknown command.\n";
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        for(auto& [id, sock] : clients){
            closesocket(sock);
        }
    }
    
    closesocket(server_socket);
    WSACleanup();

    std::cout << "Server shutdown. \n";
    return 0;

}
