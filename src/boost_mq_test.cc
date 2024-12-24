#include <boost/interprocess/ipc/message_queue.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <algorithm>

using namespace boost::interprocess;

const char* MQ_NAME = "benchmark_mq";
const int MSG_SIZE = 32;
const int WARMUP_COUNT = 100;
const int TEST_COUNT = 1000;
const int MAX_MESSAGES = 100;  // 队列容量

// 消息类型
struct Message {
    enum Type { DATA, EXIT } type;
    char data[MSG_SIZE - sizeof(Type)];
};

void server_node() {
    try {
        // 创建消息队列
        message_queue::remove(MQ_NAME);  // 清理可能存在的旧队列
        message_queue mq_recv(create_only, MQ_NAME, MAX_MESSAGES, sizeof(Message));

        // 预分配缓冲区
        Message msg;
        
        // 处理请求
        while (true) {
            // 接收请求
            size_t recvd_size;
            unsigned int priority;
            mq_recv.receive(&msg, sizeof(Message), recvd_size, priority);

            // 检查是否是退出消息
            if (msg.type == Message::EXIT) {
                break;
            }
            
            // 发送响应（直接回显）
            mq_recv.send(&msg, sizeof(Message), 0);  // 优先级为0
        }
        
        // 清理
        message_queue::remove(MQ_NAME);
    }
    catch (interprocess_exception& ex) {
        std::cerr << "Server error: " << ex.what() << std::endl;
        message_queue::remove(MQ_NAME);
    }
}

void client_node() {
    try {
        // 打开消息队列
        message_queue mq_send(open_only, MQ_NAME);
        
        // 预分配缓冲区
        Message send_msg;
        Message recv_msg;
        send_msg.type = Message::DATA;
        std::fill_n(send_msg.data, sizeof(send_msg.data), 'A');
        
        // Warmup
        std::cout << "Starting warmup...\n";
        for (int i = 0; i < WARMUP_COUNT; i++) {
            mq_send.send(&send_msg, sizeof(Message), 0);
            
            size_t recvd_size;
            unsigned int priority;
            mq_send.receive(&recv_msg, sizeof(Message), recvd_size, priority);
        }
        std::cout << "Warmup completed\n";

        // Latency test
        std::vector<double> latencies;
        latencies.reserve(TEST_COUNT);

        std::cout << "Starting latency test...\n";
        for (int i = 0; i < TEST_COUNT; i++) {
            auto start = std::chrono::high_resolution_clock::now();
            
            // 发送请求
            mq_send.send(&send_msg, sizeof(Message), 0);
            
            // 接收响应
            size_t recvd_size;
            unsigned int priority;
            mq_send.receive(&recv_msg, sizeof(Message), recvd_size, priority);
            
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
            latencies.push_back(duration.count());

            if ((i + 1) % 100 == 0) {
                std::cout << "Completed " << (i + 1) << " iterations\n";
            }
        }

        // 发送退出消息
        send_msg.type = Message::EXIT;
        mq_send.send(&send_msg, sizeof(Message), 0);

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
    }
    catch (interprocess_exception& ex) {
        std::cerr << "Client error: " << ex.what() << std::endl;
        message_queue::remove(MQ_NAME);
    }
}

int main() {
    message_queue::remove(MQ_NAME);  // 确保开始时队列是干净的
    
    std::thread server(server_node);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // Let server start
    std::thread client(client_node);

    client.join();
    server.join();
    
    message_queue::remove(MQ_NAME);  // 最后清理
    return 0;
} 