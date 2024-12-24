#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <zmq.hpp>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cmath>
#include <nng/nng.h>
#include <nng/protocol/reqrep0/req.h>
#include <nng/protocol/reqrep0/rep.h>

class IPCBase {
public:
    virtual ~IPCBase() = default;
    virtual void init(bool is_server) = 0;
    virtual void send(const void* data, size_t size) = 0;
    virtual bool recv(void* data, size_t size) = 0;
};

class ZmqIPC : public IPCBase {
public:
    ZmqIPC() : context_(1), socket_(context_, zmq::socket_type::dealer) {}
    
    void init(bool is_server) override {
        if (is_server) {
            socket_.bind("ipc:///dev/shm/zmq.socket");
        } else {
            socket_.connect("ipc:///dev/shm/zmq.socket");
        }
    }
    
    void send(const void* data, size_t size) override {
        socket_.send(zmq::buffer(data, size), zmq::send_flags::none);
    }
    
    bool recv(void* data, size_t size) override {
        zmq::message_t msg;
        auto result = socket_.recv(msg);
        if (result) {
            std::memcpy(data, msg.data(), size);
            return true;
        }
        return false;
    }

private:
    zmq::context_t context_;
    zmq::socket_t socket_;
};

class SharedMemoryIPC : public IPCBase {
public:
    SharedMemoryIPC(size_t size) : shm_size_(size) {
        data_size_ = size;
        total_size_ = sizeof(Header) + size * 2;  // 双缓冲区
    }
    
    void init(bool is_server) override {
        is_server_ = is_server;
        
        // 创建/打开同步信号量
        if (is_server) {
            sem_unlink("/shm_server_ready");
            sem_unlink("/shm_client_ready");
            server_sem_ = sem_open("/shm_server_ready", O_CREAT, 0666, 0);
            client_sem_ = sem_open("/shm_client_ready", O_CREAT, 0666, 0);
        } else {
            while ((server_sem_ = sem_open("/shm_server_ready", 0)) == SEM_FAILED) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            while ((client_sem_ = sem_open("/shm_client_ready", 0)) == SEM_FAILED) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        // 创建/打开共享内存
        int fd;
        if (is_server) {
            fd = shm_open("/my_shm", O_CREAT | O_RDWR, 0666);
            ftruncate(fd, total_size_);
            
            shared_mem_ = mmap(NULL, total_size_, PROT_READ | PROT_WRITE, 
                             MAP_SHARED, fd, 0);
            close(fd);
            
            // 初始化共享内存
            header_ = static_cast<Header*>(shared_mem_);
            header_->server_write_index.store(0);
            header_->client_read_index.store(0);
            header_->client_write_index.store(0);
            header_->server_read_index.store(0);
            
            // 通知client服务器已准备好
            sem_post(server_sem_);
            
            // 等待client准备好
            sem_wait(client_sem_);
        } else {
            // 等待server准备好
            sem_wait(server_sem_);
            
            fd = shm_open("/my_shm", O_RDWR, 0666);
            shared_mem_ = mmap(NULL, total_size_, PROT_READ | PROT_WRITE, 
                             MAP_SHARED, fd, 0);
            close(fd);
            
            header_ = static_cast<Header*>(shared_mem_);
            
            // 通知server客户端已准备好
            sem_post(client_sem_);
        }
        
        data_server_ = static_cast<uint8_t*>(shared_mem_) + sizeof(Header);
        data_client_ = data_server_ + data_size_;
    }
    
    void send(const void* data, size_t size) override {
        if (is_server_) {
            // Server: 非阻塞写入
            uint8_t* write_buf = data_server_;
            std::memcpy(write_buf, data, size);
            header_->server_write_index.fetch_add(1, std::memory_order_release);
        } else {
            // Client: 写入自己的缓冲区
            uint8_t* write_buf = data_client_;
            std::memcpy(write_buf, data, size);
            header_->client_write_index.fetch_add(1, std::memory_order_release);
        }
    }
    
    bool recv(void* data, size_t size) override {
        if (is_server_) {
            // Server: 检查client的写入
            uint32_t client_write = header_->client_write_index.load(std::memory_order_acquire);
            uint32_t server_read = header_->server_read_index.load(std::memory_order_relaxed);
            
            if (client_write > server_read) {
                std::memcpy(data, data_client_, size);
                header_->server_read_index.fetch_add(1, std::memory_order_release);
                return true;
            }
        } else {
            // Client: 检查server的写入
            uint32_t server_write = header_->server_write_index.load(std::memory_order_acquire);
            uint32_t client_read = header_->client_read_index.load(std::memory_order_relaxed);
            
            if (server_write > client_read) {
                std::memcpy(data, data_server_, size);
                header_->client_read_index.fetch_add(1, std::memory_order_release);
                return true;
            }
        }
        return false;
    }
    
    ~SharedMemoryIPC() {
        if (shared_mem_) {
            munmap(shared_mem_, total_size_);
        }
        
        if (server_sem_) {
            sem_close(server_sem_);
        }
        if (client_sem_) {
            sem_close(client_sem_);
        }
        
        if (is_server_) {
            shm_unlink("/my_shm");
            sem_unlink("/shm_server_ready");
            sem_unlink("/shm_client_ready");
        }
    }

private:
    struct Header {
        std::atomic<uint32_t> server_write_index;
        std::atomic<uint32_t> client_read_index;
        std::atomic<uint32_t> client_write_index;
        std::atomic<uint32_t> server_read_index;
    };
    
    sem_t* server_sem_ = nullptr;
    sem_t* client_sem_ = nullptr;
    void* shared_mem_ = nullptr;
    Header* header_ = nullptr;
    uint8_t* data_server_ = nullptr;  // 服务器写入缓冲区
    uint8_t* data_client_ = nullptr;  // 客户端写入缓冲区
    size_t shm_size_;
    size_t data_size_;
    size_t total_size_;
    bool is_server_;
};

class UnixDomainSocketIPC : public IPCBase {
public:
    UnixDomainSocketIPC() {
        sock_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    }
    
    ~UnixDomainSocketIPC() {
        if (sock_fd_ >= 0) {
            close(sock_fd_);
        }
        if (is_server_) {
            unlink("/tmp/bench.sock");
        }
    }
    
    void init(bool is_server) override {
        is_server_ = is_server;
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, "/tmp/bench.sock", sizeof(addr.sun_path)-1);
        
        if (is_server) {
            unlink("/tmp/bench.sock");
            bind(sock_fd_, (struct sockaddr*)&addr, sizeof(addr));
            listen(sock_fd_, 1);
            client_fd_ = accept(sock_fd_, NULL, NULL);
        } else {
            connect(sock_fd_, (struct sockaddr*)&addr, sizeof(addr));
            client_fd_ = sock_fd_;
        }
    }
    
    void send(const void* data, size_t size) override {
        write(client_fd_, data, size);
    }
    
    bool recv(void* data, size_t size) override {
        return read(client_fd_, data, size) == size;
    }

private:
    int sock_fd_ = -1;
    int client_fd_ = -1;
    bool is_server_ = false;
};

class NngIPC : public IPCBase {
public:
    NngIPC() {
        socket_.id = 0;  // Initialize socket id to invalid value
    }
    
    ~NngIPC() {
        if (socket_.id != 0) {
            nng_close(socket_);
        }
    }

    void init(bool is_server) override {
        // from nng v2.x, we need nng_init() before using nng_socket
        int rv;
        if (is_server) {
            if ((rv = nng_rep0_open(&socket_)) != 0) {
                throw std::runtime_error("nng_rep0_open: " + std::string(nng_strerror(rv)));
            }
            if ((rv = nng_listen(socket_, "ipc:///dev/shm/bench.ipc", nullptr, 0)) != 0) {
                nng_close(socket_);
                throw std::runtime_error("nng_listen: " + std::string(nng_strerror(rv)));
            }
        } else {
            if ((rv = nng_req0_open(&socket_)) != 0) {
                throw std::runtime_error("nng_req0_open: " + std::string(nng_strerror(rv)));
            }
            if ((rv = nng_dial(socket_, "ipc:///dev/shm/bench.ipc", nullptr, 0)) != 0) {
                nng_close(socket_);
                throw std::runtime_error("nng_dial: " + std::string(nng_strerror(rv)));
            }
        }

        // Set send timeout (10 seconds)
        if ((rv = nng_socket_set_ms(socket_, NNG_OPT_SENDTIMEO, 10000)) != 0) {
            nng_close(socket_);
            throw std::runtime_error("nng_socket_set_ms (sendtimeo): " + std::string(nng_strerror(rv)));
        }

        // Set receive timeout (10 seconds)
        if ((rv = nng_socket_set_ms(socket_, NNG_OPT_RECVTIMEO, 10000)) != 0) {
            nng_close(socket_);
            throw std::runtime_error("nng_socket_set_ms (recvtimeo): " + std::string(nng_strerror(rv)));
        }
    }

    void send(const void* data, size_t size) override {
        int rv;
        if ((rv = nng_send(socket_, const_cast<void*>(data), size, 0)) != 0) {
            throw std::runtime_error("nng_send: " + std::string(nng_strerror(rv)));
        }
    }

    bool recv(void* data, size_t size) override {
        size_t sz = size;
        int rv = nng_recv(socket_, data, &sz, NNG_FLAG_NONBLOCK);
        if (rv == NNG_EAGAIN) {
            return false;
        }
        if (rv != 0) {
            throw std::runtime_error("nng_recv: " + std::string(nng_strerror(rv)));
        }
        if (sz != size) {
            throw std::runtime_error("nng_recv: Size mismatch, expected " + 
                                   std::to_string(size) + " got " + std::to_string(sz));
        }
        return true;
    }

private:
    nng_socket socket_;
};

void run_benchmark(IPCBase& ipc, bool is_server, size_t data_size, int iterations) {
    std::vector<uint8_t> send_buf(data_size, 0x55);
    std::vector<uint8_t> recv_buf(data_size);
    std::vector<double> latencies;
    latencies.reserve(iterations);
    
    ipc.init(is_server);
    
    double total_latency = 0;
    double min_latency = std::numeric_limits<double>::max();
    double max_latency = 0;
    
    for (int i = 0; i < iterations; i++) {
        auto start = std::chrono::high_resolution_clock::now();
        
        if (is_server) {
            ipc.send(send_buf.data(), data_size);
            ipc.recv(recv_buf.data(), data_size);
        } else {
            ipc.recv(recv_buf.data(), data_size);
            ipc.send(send_buf.data(), data_size);
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration_cast<std::chrono::nanoseconds>
            (end - start).count();
        
        latencies.push_back(latency);
        total_latency += latency;
        min_latency = std::min(min_latency, latency);
        max_latency = std::max(max_latency, latency);
    }
    
    // 计算统计数据
    double avg_latency = total_latency / iterations;
    
    // 计算标准差
    double variance = 0;
    for (double lat : latencies) {
        variance += (lat - avg_latency) * (lat - avg_latency);
    }
    double stddev = std::sqrt(variance / iterations);
    
    // 计算中位数
    std::sort(latencies.begin(), latencies.end());
    double median = latencies[iterations/2];
    
    std::cout << "Performance Statistics (nanoseconds):\n"
              << "  Average latency: " << avg_latency << "\n"
              << "  Median latency:  " << median << "\n"
              << "  Min latency:     " << min_latency << "\n"
              << "  Max latency:     " << max_latency << "\n"
              << "  Stddev:          " << stddev << "\n"
              << "  Throughput:      " << (data_size * iterations * 1000000000.0 / total_latency) 
              << " bytes/sec\n";
}

int main(int argc, char* argv[]) {
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0] 
                  << " <zmq|shm> <server|client> <data_size> <iterations>\n";
        return 1;
    }
    
    std::string type = argv[1];
    bool is_server = std::string(argv[2]) == "server";
    size_t data_size = std::stoul(argv[3]);
    int iterations = std::stoi(argv[4]);
    
    if (type == "zmq") {
        ZmqIPC ipc;
        run_benchmark(ipc, is_server, data_size, iterations);
    } else if (type == "shm") {
        SharedMemoryIPC ipc(data_size);
        run_benchmark(ipc, is_server, data_size, iterations);
    } else if (type == "uds") {
        UnixDomainSocketIPC ipc;
        run_benchmark(ipc, is_server, data_size, iterations);
    } else if (type == "nng") {
        NngIPC ipc;
        run_benchmark(ipc, is_server, data_size, iterations);
    } else {
        std::cerr << "Invalid type. Use 'zmq', 'shm', 'uds', or 'nng'\n";
        return 1;
    }
    
    return 0;
}