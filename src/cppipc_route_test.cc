#include <libipc/ipc.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <algorithm>

const char* REQ_ROUTE = "benchmark_req";   // 请求通道
const char* RESP_ROUTE = "benchmark_resp"; // 响应通道
const int MSG_SIZE = 32;
const int WARMUP_COUNT = 100;
const int TEST_COUNT = 1000;

void server_node() {
    // 创建请求接收端和响应发送端
    ipc::route rt_recv { REQ_ROUTE, ipc::receiver };
    ipc::route rt_send { RESP_ROUTE, ipc::sender };

    // 等待客户端连接
    rt_send.wait_for_recv(1);

    // 处理请求
    while (true) {
        // 接收请求
        auto recv_data = rt_recv.recv();
        if (recv_data.empty()) {
            continue;
        }

        // 将 buff_t 转换为字符串以检查内容
        std::string msg(static_cast<const char*>(recv_data.data()), recv_data.size());

        // 检查是否是退出消息
        if (!msg.empty() && msg[0] == 'Q') {
            break;
        }

        // 发送响应（直接回显）
        rt_send.send(recv_data);
    }
}

void client_node() {
    // 创建请求发送端和响应接收端
    ipc::route rt_send { REQ_ROUTE, ipc::sender };
    ipc::route rt_recv { RESP_ROUTE, ipc::receiver };
    
    // 等待服务端就绪
    std::cout << "Waiting for server...\n";
    rt_send.wait_for_recv(1);
    
    // 预分配缓冲区
    std::string send_buf(MSG_SIZE, 'A');
    
    // Warmup
    std::cout << "Starting warmup...\n";
    for (int i = 0; i < WARMUP_COUNT; i++) {
        rt_send.send(send_buf);
        auto recv_data = rt_recv.recv();
        while (recv_data.empty()) {
            std::this_thread::yield();
            recv_data = rt_recv.recv();
        }
    }
    std::cout << "Warmup completed\n";

    // Latency test
    std::vector<double> latencies;
    latencies.reserve(TEST_COUNT);

    std::cout << "Starting latency test...\n";
    for (int i = 0; i < TEST_COUNT; i++) {
        auto start = std::chrono::high_resolution_clock::now();
        
        // 发送请求
        rt_send.send(send_buf);
        
        // 等待响应
        auto recv_data = rt_recv.recv();
        while (recv_data.empty()) {
            std::this_thread::yield();
            recv_data = rt_recv.recv();
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        latencies.push_back(duration.count());

        if ((i + 1) % 100 == 0) {
            std::cout << "Completed " << (i + 1) << " iterations\n";
        }
    }

    // 发送退出消息
    send_buf[0] = 'Q';
    rt_send.send(send_buf);

    // Calculate statistics
    std::sort(latencies.begin(), latencies.end());
    double total = 0;
    for (double lat : latencies) {
        total += lat;
    }
    
    double avg_latency = total / TEST_COUNT / 1000.0;  // convert to microseconds
    double p50 = latencies[TEST_COUNT * 50 / 100] / 1000.0;
    double p90 = latencies[TEST_COUNT * 90 / 100] / 1000.0;
    double p99 = latencies[TEST_COUNT * 99 / 100] / 1000.0;
    double min_latency = latencies.front() / 1000.0;
    double max_latency = latencies.back() / 1000.0;
    
    double throughput = (1000000.0 / avg_latency) * 2;  // messages per second

    std::cout << "\nLatency Statistics (microseconds):\n"
              << "  Average: " << avg_latency << "\n"
              << "  P50:     " << p50 << "\n"
              << "  P90:     " << p90 << "\n"
              << "  P99:     " << p99 << "\n"
              << "  Min:     " << min_latency << "\n"
              << "  Max:     " << max_latency << "\n"
              << "\nThroughput: " << throughput << " messages/second\n";

    // 清理资源
    rt_send.disconnect();
    rt_recv.disconnect();
}

int main() {
    // 清理可能存在的旧 route
    ipc::route::clear_storage(REQ_ROUTE);
    ipc::route::clear_storage(RESP_ROUTE);

    std::thread server(server_node);
    std::thread client(client_node);

    client.join();
    server.join();

    // 清理 route
    ipc::route::clear_storage(REQ_ROUTE);
    ipc::route::clear_storage(RESP_ROUTE);

    return 0;
}