#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <zmq.hpp>

const char* URL = "ipc:///dev/shm/zmq_test";
const int MSG_SIZE = 32;
const int WARMUP_COUNT = 100;
const int TEST_COUNT = 1000;

void server_node() {
    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::rep);
    
    // Configure socket
    int rcvbuf = 8192;
    socket.set(zmq::sockopt::rcvbuf, rcvbuf);
    int sndbuf = 8192;
    socket.set(zmq::sockopt::sndbuf, sndbuf);
    int timeout = 1000;  // 1 second
    socket.set(zmq::sockopt::rcvtimeo, timeout);
    socket.set(zmq::sockopt::sndtimeo, timeout);
    
    socket.bind(URL);

    // Pre-allocate receive buffer
    zmq::message_t recv_msg(MSG_SIZE);
    zmq::message_t send_msg(MSG_SIZE);
    
    // Echo back messages
    for (int i = 0; i < WARMUP_COUNT + TEST_COUNT; i++) {
        auto result = socket.recv(recv_msg, zmq::recv_flags::none);
        if (!result) {
            std::cerr << "Server receive failed\n";
            return;
        }
        
        // Copy received data to send buffer
        std::memcpy(send_msg.data(), recv_msg.data(), MSG_SIZE);
        
        result = socket.send(send_msg, zmq::send_flags::none);
        if (!result) {
            std::cerr << "Server send failed\n";
            return;
        }
    }
}

void client_node() {
    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::req);
    
    // Configure socket
    int rcvbuf = 8192;
    socket.set(zmq::sockopt::rcvbuf, rcvbuf);
    int sndbuf = 8192;
    socket.set(zmq::sockopt::sndbuf, sndbuf);
    int timeout = 1000;  // 1 second
    socket.set(zmq::sockopt::rcvtimeo, timeout);
    socket.set(zmq::sockopt::sndtimeo, timeout);
    
    // Add a small delay before connecting
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    socket.connect(URL);
    
    // Add a small delay after connecting
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Pre-allocate buffers
    zmq::message_t send_msg(MSG_SIZE);
    std::memset(send_msg.data(), 'A', MSG_SIZE);
    zmq::message_t recv_msg(MSG_SIZE);
    
    // Warmup
    std::cout << "Starting warmup...\n";
    for (int i = 0; i < WARMUP_COUNT; i++) {
        auto result = socket.send(send_msg, zmq::send_flags::none);
        if (!result) {
            std::cerr << "Warmup send failed\n";
            return;
        }
        
        result = socket.recv(recv_msg, zmq::recv_flags::none);
        if (!result) {
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
        
        auto result = socket.send(send_msg, zmq::send_flags::none);
        if (!result) {
            std::cerr << "Send failed at iteration " << i << "\n";
            return;
        }
        
        result = socket.recv(recv_msg, zmq::recv_flags::none);
        if (!result) {
            std::cerr << "Receive failed at iteration " << i << "\n";
            return;
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        latencies.push_back(duration.count());

        if ((i + 1) % 100 == 0) {
            std::cout << "Completed " << (i + 1) << " iterations\n";
        }
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
}

int main() {
    std::thread server(server_node);
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Let server start
    std::thread client(client_node);

    server.join();
    client.join();
    
    return 0;
}
