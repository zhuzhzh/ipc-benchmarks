#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <libipc/ipc.h>
#include <libipc/shm.h>
#include <cstring>

const char* REQ_CHANNEL = "benchmark_req";
const char* RESP_CHANNEL = "benchmark_resp";
const int MSG_SIZE = 32;
const int WARMUP_COUNT = 100;
const int TEST_COUNT = 1000;

void server_node() {
    // Create channels for receiving requests and sending responses
    ipc::channel req_channel{REQ_CHANNEL, ipc::receiver};
    ipc::channel resp_channel{RESP_CHANNEL, ipc::sender};
    
    // Echo back messages
    for (int i = 0; i < WARMUP_COUNT + TEST_COUNT; i++) {
        // Receive request
        auto recv_data = req_channel.recv();
        
        if (recv_data.empty() || recv_data.size() != MSG_SIZE) {
            std::cerr << "Server receive failed\n";
            return;
        }
        
        // Send response
        if (!resp_channel.send(recv_data.data(), MSG_SIZE)) {
            std::cerr << "Server send failed\n";
            return;
        }
    }
}

void client_node() {
    // Create channels for sending requests and receiving responses
    ipc::channel req_channel{REQ_CHANNEL, ipc::sender};
    ipc::channel resp_channel{RESP_CHANNEL, ipc::receiver};
    
    // Add a small delay before starting
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Pre-allocate buffers
    std::vector<char> send_buf(MSG_SIZE, 'A');
    
    // Wait for server to be ready
    std::cout << "Waiting for server...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Warmup
    std::cout << "Starting warmup...\n";
    for (int i = 0; i < WARMUP_COUNT; i++) {
        if (!req_channel.send(send_buf.data(), MSG_SIZE)) {
            std::cerr << "Warmup send failed\n";
            return;
        }
        
        auto recv_data = resp_channel.recv();
        
        if (recv_data.empty() || recv_data.size() != MSG_SIZE) {
            std::cerr << "Warmup receive failed\n";
            return;
        }
    }
    std::cout << "Warmup completed\n";

    // Latency test
    std::vector<double> latencies;
    latencies.reserve(TEST_COUNT);

    std::cout << "Starting latency test...\n";
    for (int i = 0; i < TEST_COUNT; i++) {
        auto start = std::chrono::high_resolution_clock::now();
        
        if (!req_channel.send(send_buf.data(), MSG_SIZE)) {
            std::cerr << "Send failed at iteration " << i << "\n";
            return;
        }
        
        auto recv_data = resp_channel.recv();
        
        if (recv_data.empty() || recv_data.size() != MSG_SIZE) {
            std::cerr << "Receive failed at iteration " << i << "\n";
            return;
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        latencies.push_back(duration.count());

        //if ((i + 1) % 100 == 0) {
        //    std::cout << "Completed " << (i + 1) << " iterations\n";
        //}
    }

    // Calculate statistics
    std::sort(latencies.begin(), latencies.end());
    double total = 0;
    for (double lat : latencies) {
        total += lat;
    }
    
    double avg_latency = total / TEST_COUNT / 1000.0; // convert to microseconds
    double p50 = latencies[TEST_COUNT * 50 / 100] / 1000.0;
    double p90 = latencies[TEST_COUNT * 90 / 100] / 1000.0;
    double p99 = latencies[TEST_COUNT * 99 / 100] / 1000.0;
    double min_latency = latencies.front() / 1000.0;
    double max_latency = latencies.back() / 1000.0;
    
    double throughput = (1000000.0 / avg_latency) * 2; // messages per second (multiply by 2 for roundtrip)

    std::cout << "\nLatency Statistics (microseconds):\n"
              << "  Average: " << avg_latency << "\n"
              << "  P50:     " << p50 << "\n"
              << "  P90:     " << p90 << "\n"
              << "  P99:     " << p99 << "\n"
              << "  Min:     " << min_latency << "\n"
              << "  Max:     " << max_latency << "\n"
              << "\nThroughput: " << throughput << " messages/second\n";
              
    // Clean up
    req_channel.disconnect();
    resp_channel.disconnect();
}

int main() {
    std::thread server(server_node);
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Let server start
    std::thread client(client_node);

    client.join();
    server.join();
    
    return 0;
}
