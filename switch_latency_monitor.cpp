#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <csignal>
#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <random>
#include <stdexcept>
#include <poll.h>
#include <endian.h>

#include <unistd.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <linux/if_packet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <linux/errqueue.h>

std::atomic<bool> g_running{true};
constexpr uint64_t MAGIC_TAG = 0x5357495443485453ULL;

void signal_handler(int) {
    g_running = false;
}

struct PacketPayload {
    uint64_t magic;
    uint64_t seq_id;
};

constexpr size_t UDP_PAYLOAD_OFFSET = sizeof(ether_header) + sizeof(iphdr) + sizeof(udphdr);
static_assert(UDP_PAYLOAD_OFFSET + sizeof(PacketPayload) <= 64 - 4,
              "Measurement payload must fit in a 64-byte Ethernet frame without the FCS");

struct PendingPacket {
    uint64_t tx_ns = 0;
    uint16_t frame_size = 0;
    double elapsed_sec = 0.0;
};

struct FrameStats {
    uint64_t sent = 0;
    uint64_t received = 0;
    std::vector<double> latencies_us;
};

inline uint64_t timespec_to_ns(const struct timespec& ts) {
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

uint16_t ipv4_checksum(const void* data, size_t len) {
    const auto* ptr = reinterpret_cast<const uint16_t*>(data);
    uint32_t sum = 0;
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len > 0) {
        sum += *reinterpret_cast<const uint8_t*>(ptr);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum);
}

void get_iface_info(int sock, const char* iface_name, int& ifindex, uint8_t mac[6]) {
    struct ifreq ifr {};
    std::strncpy(ifr.ifr_name, iface_name, IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
        perror("SIOCGIFINDEX");
        exit(EXIT_FAILURE);
    }
    ifindex = ifr.ifr_ifindex;

    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0) {
        perror("SIOCGIFHWADDR");
        exit(EXIT_FAILURE);
    }
    std::memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
}

void enable_hw_timestamping(int sock, const char* iface_name) {
    struct ifreq ifr {};
    struct hwtstamp_config hwconfig {};

    std::strncpy(ifr.ifr_name, iface_name, IFNAMSIZ - 1);
    hwconfig.tx_type = HWTSTAMP_TX_ON;
    hwconfig.rx_filter = HWTSTAMP_FILTER_ALL;

    ifr.ifr_data = reinterpret_cast<char*>(&hwconfig);
    if (ioctl(sock, SIOCSHWTSTAMP, &ifr) < 0) {
        hwconfig.rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_EVENT;
        if (ioctl(sock, SIOCSHWTSTAMP, &ifr) < 0) {
            perror("SIOCSHWTSTAMP (verifique sudo/CAP_NET_ADMIN)");
        }
    }
}

uint64_t extract_hw_timestamp(struct msghdr* msg) {
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMPING &&
            cmsg->cmsg_len >= CMSG_LEN(3 * sizeof(struct timespec))) {
            auto* ts = reinterpret_cast<struct timespec*>(CMSG_DATA(cmsg));
            return timespec_to_ns(ts[2]);
        }
    }
    return 0;
}

void print_dashboard(const std::vector<size_t>& frame_sizes,
                     std::unordered_map<size_t, FrameStats>& stats_map,
                     std::mutex& mtx,
                     double elapsed_sec,
                     const std::string& csv_path,
                     bool clear_screen) {
    std::lock_guard<std::mutex> lock(mtx);

    if (clear_screen) {
        std::cout << "\033[2J\033[H";
    }

    std::cout << "=========================================================================================================\n";
    std::cout << " MONITOR DE LATENCIA DE SWITCH (10GbE HW TIMESTAMP) | Tempo: "
              << std::fixed << std::setprecision(1) << elapsed_sec << "s | Arquivo: " << csv_path << "\n";
    std::cout << " Pressione Ctrl+C para encerrar e salvar o relatorio final.\n";
    std::cout << "=========================================================================================================\n";
    std::cout << std::left
              << std::setw(10) << "Quadro"
              << std::setw(14) << "Env / Rec"
              << std::setw(10) << "Perda"
              << std::setw(13) << "Min (us)"
              << std::setw(13) << "Media (us)"
              << std::setw(13) << "P50 (us)"
              << std::setw(13) << "P99 (us)"
              << std::setw(13) << "Max (us)"
              << std::setw(12) << "Jitter(us)" << "\n";
    std::cout << "---------------------------------------------------------------------------------------------------------\n";

    for (size_t fsize : frame_sizes) {
        auto& st = stats_map[fsize];
        double loss_pct = (st.sent > 0) ? (100.0 * (st.sent - st.received) / st.sent) : 0.0;

        std::cout << std::left << std::setw(10) << (std::to_string(fsize) + "B")
                  << std::setw(14) << (std::to_string(st.sent) + "/" + std::to_string(st.received))
                  << std::setw(10) << (std::to_string(static_cast<int>(loss_pct)) + "%");

        if (!st.latencies_us.empty()) {
            std::vector<double> sorted = st.latencies_us;
            std::sort(sorted.begin(), sorted.end());

            double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
            double mean = sum / sorted.size();

            double sq_sum = 0.0;
            for (double v : sorted) {
                sq_sum += (v - mean) * (v - mean);
            }
            double jitter = std::sqrt(sq_sum / sorted.size());

            double min_v = sorted.front();
            double max_v = sorted.back();
            double p50 = sorted[static_cast<size_t>(sorted.size() * 0.50)];
            double p99 = sorted[static_cast<size_t>(sorted.size() * 0.99)];

            std::cout << std::fixed << std::setprecision(3)
                      << std::setw(13) << min_v
                      << std::setw(13) << mean
                      << std::setw(13) << p50
                      << std::setw(13) << p99
                      << std::setw(13) << max_v
                      << std::setw(12) << jitter << "\n";
        } else {
            std::cout << std::setw(13) << "-"
                      << std::setw(13) << "-"
                      << std::setw(13) << "-"
                      << std::setw(13) << "-"
                      << std::setw(13) << "-"
                      << std::setw(12) << "-" << "\n";
        }
    }
    std::cout << "=========================================================================================================\n";
    std::cout.flush();
}

std::vector<std::vector<uint8_t>> build_frames(const std::vector<size_t>& frame_sizes,
                                               const uint8_t* tx_mac, const uint8_t* rx_mac,
                                               uint16_t udp_port) {
    std::vector<std::vector<uint8_t>> prebuilt_frames;
    for (size_t frame_size : frame_sizes) {
        if (frame_size < 64) throw std::invalid_argument("Ethernet frame shorter than 64 bytes");
        size_t wire_buf_len = frame_size - 4;
        std::vector<uint8_t> frame(wire_buf_len, 0x00);

        auto* eth = reinterpret_cast<ether_header*>(frame.data());
        std::memcpy(eth->ether_dhost, rx_mac, 6);
        std::memcpy(eth->ether_shost, tx_mac, 6);
        eth->ether_type = htons(ETH_P_IP);

        auto* ip = reinterpret_cast<iphdr*>(frame.data() + sizeof(ether_header));
        ip->ihl = 5;
        ip->version = 4;
        ip->tos = 0;
        ip->tot_len = htons(static_cast<uint16_t>(wire_buf_len - sizeof(ether_header)));
        ip->id = htons(1234);
        ip->frag_off = 0;
        ip->ttl = 64;
        ip->protocol = IPPROTO_UDP;
        inet_pton(AF_INET, "10.255.255.1", &ip->saddr);
        inet_pton(AF_INET, "10.255.255.2", &ip->daddr);
        ip->check = 0;
        ip->check = ipv4_checksum(ip, sizeof(iphdr));

        auto* udp = reinterpret_cast<udphdr*>(frame.data() + sizeof(ether_header) + sizeof(iphdr));
        udp->source = htons(40000);
        udp->dest = htons(udp_port);
        udp->len = htons(static_cast<uint16_t>(wire_buf_len - sizeof(ether_header) - sizeof(iphdr)));
        udp->check = 0;

        prebuilt_frames.push_back(std::move(frame));
    }

    return prebuilt_frames;
}

int run_local(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Uso: sudo " << argv[0] << " <iface_tx> <iface_rx> [arquivo_saida.csv]\n";
        std::cerr << "Exemplo: sudo " << argv[0] << " eth0 eth1 medidas_switch.csv\n";
        return EXIT_FAILURE;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    const char* tx_iface = argv[1];
    const char* rx_iface = argv[2];
    const std::string csv_filename = (argc >= 4) ? argv[3] : "latency_results.csv";


    int tx_sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    int rx_sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (tx_sock < 0 || rx_sock < 0) {
        perror("socket AF_PACKET (execute com sudo)");
        return EXIT_FAILURE;
    }

    int tx_ifindex = 0, rx_ifindex = 0;
    uint8_t tx_mac[6] {}, rx_mac[6] {};
    get_iface_info(tx_sock, tx_iface, tx_ifindex, tx_mac);
    get_iface_info(rx_sock, rx_iface, rx_ifindex, rx_mac);

    enable_hw_timestamping(tx_sock, tx_iface);
    enable_hw_timestamping(rx_sock, rx_iface);

    int tx_flags = SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE | SOF_TIMESTAMPING_OPT_TSONLY;
    setsockopt(tx_sock, SOL_SOCKET, SO_TIMESTAMPING, &tx_flags, sizeof(tx_flags));

    int rx_flags = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE;
    setsockopt(rx_sock, SOL_SOCKET, SO_TIMESTAMPING, &rx_flags, sizeof(rx_flags));

    sockaddr_ll sll_tx {};
    sll_tx.sll_family = AF_PACKET;
    sll_tx.sll_protocol = htons(ETH_P_ALL);
    sll_tx.sll_ifindex = tx_ifindex;
    if (bind(tx_sock, reinterpret_cast<sockaddr*>(&sll_tx), sizeof(sll_tx)) < 0) {
        perror("bind tx_sock");
        return EXIT_FAILURE;
    }

    sockaddr_ll sll_rx {};
    sll_rx.sll_family = AF_PACKET;
    sll_rx.sll_protocol = htons(ETH_P_ALL);
    sll_rx.sll_ifindex = rx_ifindex;
    if (bind(rx_sock, reinterpret_cast<sockaddr*>(&sll_rx), sizeof(sll_rx)) < 0) {
        perror("bind rx_sock");
        return EXIT_FAILURE;
    }

    std::ofstream csv_file(csv_filename);
    if (!csv_file.is_open()) {
        std::cerr << "Erro ao abrir arquivo CSV: " << csv_filename << "\n";
        return EXIT_FAILURE;
    }
    csv_file << "elapsed_sec,seq_id,frame_size_bytes,latency_us\n";

    {
        uint8_t warmup[60] {};
        auto* eth = reinterpret_cast<ether_header*>(warmup);
        std::memcpy(eth->ether_dhost, tx_mac, 6);
        std::memcpy(eth->ether_shost, rx_mac, 6);
        eth->ether_type = htons(0x88B5);
        send(rx_sock, warmup, sizeof(warmup), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::vector<size_t> frame_sizes = {64, 128, 256, 512, 1024, 1518};
    std::unordered_map<size_t, FrameStats> stats_map;
    std::unordered_map<uint64_t, PendingPacket> pending_packets;
    std::mutex mtx;

    auto start_time = std::chrono::steady_clock::now();

    std::thread rx_thread([&]() {
        uint8_t buf[2048];
        char ctrl[1024];
        const size_t min_frame_len = UDP_PAYLOAD_OFFSET + sizeof(PacketPayload);

        while (g_running) {
            iovec iov { buf, sizeof(buf) };
            msghdr msg {};
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = ctrl;
            msg.msg_controllen = sizeof(ctrl);

            ssize_t len = recvmsg(rx_sock, &msg, MSG_DONTWAIT);
            if (len >= static_cast<ssize_t>(min_frame_len)) {
                auto* eth = reinterpret_cast<ether_header*>(buf);
                if (ntohs(eth->ether_type) == ETH_P_IP && std::memcmp(eth->ether_dhost, rx_mac, 6) == 0) {
                    auto* ip = reinterpret_cast<iphdr*>(buf + sizeof(ether_header));
                    if (ip->protocol == IPPROTO_UDP) {
                        PacketPayload payload;
                        std::memcpy(&payload, buf + UDP_PAYLOAD_OFFSET, sizeof(payload));
                        if (payload.magic == MAGIC_TAG) {
                            uint64_t rx_ns_ts = extract_hw_timestamp(&msg);
                            if (rx_ns_ts > 0) {
                                std::lock_guard<std::mutex> lock(mtx);
                                auto it = pending_packets.find(payload.seq_id);
                                if (it != pending_packets.end() && rx_ns_ts > it->second.tx_ns) {
                                    double diff_us = static_cast<double>(rx_ns_ts - it->second.tx_ns) / 1000.0;
                                    size_t fsize = it->second.frame_size;

                                    stats_map[fsize].received++;
                                    stats_map[fsize].latencies_us.push_back(diff_us);

                                    csv_file << std::fixed << std::setprecision(4)
                                             << it->second.elapsed_sec << ","
                                             << payload.seq_id << ","
                                             << fsize << ","
                                             << diff_us << "\n";

                                    pending_packets.erase(it);
                                }
                            }
                        }
                    }
                }
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
        }
    });

    std::thread ui_thread([&]() {
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!g_running) break;
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - start_time).count();
            print_dashboard(frame_sizes, stats_map, mtx, elapsed, csv_filename, true);
        }
    });

    auto prebuilt_frames = build_frames(frame_sizes, tx_mac, rx_mac, 319);

    uint64_t seq_counter = 1;
    size_t size_idx = 0;

    while (g_running) {
        size_t current_frame_size = frame_sizes[size_idx];
        auto& frame = prebuilt_frames[size_idx];
        uint64_t current_seq = seq_counter++;
        const PacketPayload payload{MAGIC_TAG, current_seq};
        std::memcpy(frame.data() + UDP_PAYLOAD_OFFSET, &payload, sizeof(payload));

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time).count();

        if (send(tx_sock, frame.data(), frame.size(), 0) > 0) {
            char ctrl[1024];
            msghdr err_msg {};
            err_msg.msg_control = ctrl;
            err_msg.msg_controllen = sizeof(ctrl);

            int retries = 0;
            while (g_running && recvmsg(tx_sock, &err_msg, MSG_ERRQUEUE | MSG_DONTWAIT) < 0) {
                if (++retries > 100000) break;
            }

            uint64_t tx_ns_ts = extract_hw_timestamp(&err_msg);
            if (tx_ns_ts > 0) {
                std::lock_guard<std::mutex> lock(mtx);
                stats_map[current_frame_size].sent++;
                pending_packets[current_seq] = { tx_ns_ts, static_cast<uint16_t>(current_frame_size), elapsed };

                if (current_seq > 2000) {
                    pending_packets.erase(current_seq - 2000);
                }
            }
        }

        size_idx = (size_idx + 1) % frame_sizes.size();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    rx_thread.join();
    ui_thread.join();

    csv_file.flush();
    csv_file.close();

    auto end_time = std::chrono::steady_clock::now();
    double total_elapsed = std::chrono::duration<double>(end_time - start_time).count();
    print_dashboard(frame_sizes, stats_map, mtx, total_elapsed, csv_filename, true);
    std::cout << "Dados salvos com sucesso em: " << csv_filename << "\n";

    close(tx_sock);
    close(rx_sock);
    return 0;
}

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr uint16_t CONTROL_PORT = 46000;
constexpr uint16_t MEASUREMENT_PORT = 49152;
constexpr uint64_t MAX_BATCH_SIZE = 10000;

bool resolve_control_interface(const char* name, in_addr& address) {
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) < 0) {
        perror("getifaddrs");
        return false;
    }
    bool found = false;
    bool ambiguous = false;
    for (auto* entry = interfaces; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET ||
            std::strcmp(entry->ifa_name, name) != 0) continue;

        const auto current = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)->sin_addr;
        if (found && current.s_addr != address.s_addr) {
            ambiguous = true;
            break;
        }
        address = current;
        found = true;
    }
    freeifaddrs(interfaces);
    if (ambiguous) {
        std::cerr << "Interface " << name << " tem mais de um IPv4. Informe o endereco desejado.\n";
    } else if (!found) {
        std::cerr << "Nenhum IPv4 encontrado na interface de controle " << name << "\n";
    }
    return found && !ambiguous;
}

// TCP messages use network byte order. The initial handshake exchanges the
// protocol identifier and the MAC addresses of the measurement interfaces.
constexpr uint64_t CONTROL_VERSION = 0x53574c4154000001ULL;
using BatchHeader = std::array<uint64_t, 2>;
using RxRecord = std::array<uint64_t, 2>;

struct Socket {
    int fd;
    explicit Socket(int descriptor) : fd(descriptor) {}
    ~Socket() { if (fd >= 0) close(fd); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};

bool wait_socket(int fd, short events, Clock::time_point deadline) {
    while (g_running && Clock::now() < deadline) {
        pollfd item{fd, events, 0};
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
        const int timeout = static_cast<int>(std::clamp(remaining, 0ms, 20ms).count());
        const int ready = poll(&item, 1, timeout);
        if (ready > 0) return (item.revents & (events | POLLHUP | POLLERR)) != 0;
        if (ready < 0 && errno != EINTR) return false;
    }
    return false;
}

bool transfer(int fd, void* data, size_t size, bool sending, Clock::time_point deadline) {
    auto* bytes = static_cast<uint8_t*>(data);
    while (size > 0 && wait_socket(fd, sending ? POLLOUT : POLLIN, deadline)) {
        const ssize_t n = sending ? send(fd, bytes, size, MSG_NOSIGNAL | MSG_DONTWAIT)
                                  : recv(fd, bytes, size, MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) return false;
        bytes += n;
        size -= static_cast<size_t>(n);
    }
    return size == 0;
}

bool configure_measurement_socket(int fd, const char* iface, bool tx, uint8_t mac[6]) {
    if (fd < 0) {
        perror("socket AF_PACKET");
        return false;
    }

    int ifindex = 0;
    get_iface_info(fd, iface, ifindex, mac);
    sockaddr_ll address{};
    address.sll_family = AF_PACKET;
    address.sll_ifindex = ifindex;
    address.sll_protocol = tx ? 0 : htons(ETH_P_IP);
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        perror("bind AF_PACKET");
        return false;
    }

    // Request the filter needed to timestamp arbitrary measurement frames.
    ifreq request{};
    std::strncpy(request.ifr_name, iface, IFNAMSIZ - 1);
    hwtstamp_config config{};
    config.tx_type = HWTSTAMP_TX_ON;
    config.rx_filter = HWTSTAMP_FILTER_ALL;
    request.ifr_data = reinterpret_cast<char*>(&config);
    if (ioctl(fd, SIOCSHWTSTAMP, &request) < 0) {
        perror("SIOCSHWTSTAMP");
        return false;
    }
    if ((tx && config.tx_type != HWTSTAMP_TX_ON) ||
        (!tx && config.rx_filter != HWTSTAMP_FILTER_ALL)) {
        std::cerr << "Driver nao ativou o timestamp de hardware solicitado em " << iface << "\n";
        return false;
    }

    const int flags = SOF_TIMESTAMPING_RAW_HARDWARE |
        (tx ? SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_OPT_TSONLY
            : SOF_TIMESTAMPING_RX_HARDWARE);
    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) < 0) {
        perror("SO_TIMESTAMPING");
        return false;
    }
    return true;
}

bool exchange_mac(int fd, bool tx, uint8_t local[6], uint8_t remote[6]) {
    auto deadline = Clock::now() + 5s;
    uint64_t version = htobe64(CONTROL_VERSION);
    uint64_t peer_version = 0;
    if (tx) {
        return transfer(fd, &version, sizeof(version), true, deadline) &&
               transfer(fd, local, 6, true, deadline) &&
               transfer(fd, &peer_version, sizeof(peer_version), false, deadline) &&
               peer_version == version && transfer(fd, remote, 6, false, deadline);
    }
    return transfer(fd, &peer_version, sizeof(peer_version), false, deadline) &&
           peer_version == version && transfer(fd, remote, 6, false, deadline) &&
           transfer(fd, &version, sizeof(version), true, deadline) &&
           transfer(fd, local, 6, true, deadline);
}

void collect_frame(int fd, const uint8_t local[6], const uint8_t remote[6],
                   std::unordered_map<uint64_t, uint64_t>& timestamps) {
    uint8_t buf[2048];
    alignas(cmsghdr) char ctrl[1024];
    iovec iov{buf, sizeof(buf)};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof(ctrl);
    const ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT);
    if (n < static_cast<ssize_t>(UDP_PAYLOAD_OFFSET + sizeof(PacketPayload))) return;

    ether_header eth;
    iphdr ip;
    udphdr udp;
    PacketPayload payload;
    std::memcpy(&eth, buf, sizeof(eth));
    std::memcpy(&ip, buf + sizeof(eth), sizeof(ip));
    std::memcpy(&udp, buf + sizeof(eth) + sizeof(ip), sizeof(udp));
    std::memcpy(&payload, buf + UDP_PAYLOAD_OFFSET, sizeof(payload));
    if (ntohs(eth.ether_type) != ETH_P_IP || std::memcmp(eth.ether_dhost, local, 6) != 0 ||
        std::memcmp(eth.ether_shost, remote, 6) != 0 || ip.version != 4 || ip.ihl != 5 ||
        ip.protocol != IPPROTO_UDP || ntohs(udp.dest) != MEASUREMENT_PORT ||
        be64toh(payload.magic) != MAGIC_TAG) return;

    const auto seq = be64toh(payload.seq_id);
    const auto timestamp = extract_hw_timestamp(&msg);
    if (timestamps.size() < 2 * MAX_BATCH_SIZE) {
        auto [it, inserted] = timestamps.emplace(seq, timestamp);
        if (!inserted && it->second == 0) it->second = timestamp;
    }
}

int run_remote_rx(const char* iface, in_addr control_ip) {
    const Socket listener(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(CONTROL_PORT);
    address.sin_addr = control_ip;
    const int reuse = 1;
    if (listener.fd < 0 ||
        setsockopt(listener.fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
        bind(listener.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(listener.fd, 1) < 0) {
        perror("TCP RX");
        return EXIT_FAILURE;
    }

    const Socket packet(socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_IP)));
    uint8_t local[6], remote[6];
    if (!configure_measurement_socket(packet.fd, iface, false, local)) return EXIT_FAILURE;
    char control_address[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &control_ip, control_address, sizeof(control_address));
    std::cout << "RX aguardando Host A em " << control_address << ':' << CONTROL_PORT << " (TCP)\n";
    if (control_ip.s_addr != htonl(INADDR_ANY)) {
        std::cout << "No Host A: sudo ./switch_latency_monitor --tx <iface_tx> "
                  << control_address << "\n";
    }
    std::cout << std::flush;
    if (!wait_socket(listener.fd, POLLIN, Clock::time_point::max())) return g_running ? EXIT_FAILURE : 0;
    const Socket control(accept(listener.fd, nullptr, nullptr));
    if (control.fd < 0 || !exchange_mac(control.fd, false, local, remote)) {
        std::cerr << "Falha ao iniciar sessao de controle\n";
        return g_running ? EXIT_FAILURE : 0;
    }

    std::unordered_map<uint64_t, uint64_t> timestamps;
    while (g_running) {
        pollfd ready[]{{packet.fd, POLLIN, 0}, {control.fd, POLLIN, 0}};
        if (poll(ready, 2, 20) < 0) {
            if (errno == EINTR) continue;
            perror("poll RX");
            return EXIT_FAILURE;
        }
        if (ready[1].revents == 0) {
            if (ready[0].revents & POLLIN) collect_frame(packet.fd, local, remote, timestamps);
            continue;
        }

        BatchHeader request{};
        auto deadline = Clock::now() + 5s;
        if (!transfer(control.fd, request.data(), sizeof(request), false, deadline)) break;
        const uint64_t first = be64toh(request[0]);
        const uint64_t count = be64toh(request[1]);
        if (count == 0 || count > MAX_BATCH_SIZE || first > UINT64_MAX - count) {
            std::cerr << "Intervalo de sequencias invalido\n";
            return EXIT_FAILURE;
        }

        const auto end_window = Clock::now() + 10ms;
        while (wait_socket(packet.fd, POLLIN, end_window)) {
            collect_frame(packet.fd, local, remote, timestamps);
        }

        std::vector<RxRecord> records;
        for (uint64_t seq = first; seq < first + count; ++seq) {
            const auto it = timestamps.find(seq);
            if (it != timestamps.end()) records.push_back({htobe64(seq), htobe64(it->second)});
        }
        timestamps.clear();

        BatchHeader response{htobe64(first), htobe64(records.size())};
        deadline = Clock::now() + 5s;
        if (!transfer(control.fd, response.data(), sizeof(response), true, deadline) ||
            !transfer(control.fd, records.data(), records.size() * sizeof(RxRecord), true, deadline)) {
            return g_running ? EXIT_FAILURE : 0;
        }
    }
    return 0;
}

uint64_t read_tx_timestamp(int fd) {
    const auto deadline = Clock::now() + 100ms;
    while (wait_socket(fd, POLLERR, deadline)) {
        alignas(cmsghdr) char ctrl[1024];
        msghdr msg{};
        msg.msg_control = ctrl;
        msg.msg_controllen = sizeof(ctrl);
        if (recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT) >= 0) {
            const auto timestamp = extract_hw_timestamp(&msg);
            if (timestamp) return timestamp;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
    }
    return 0;
}

int run_remote_tx(const char* iface, in_addr control_ip, size_t batch_size,
                  const std::string& csv_path) {
    const Socket control(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(CONTROL_PORT);
    peer.sin_addr = control_ip;
    if (control.fd < 0) {
        perror("socket TCP");
        return EXIT_FAILURE;
    }
    if (connect(control.fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) < 0) {
        if (errno != EINPROGRESS || !wait_socket(control.fd, POLLOUT, Clock::now() + 5s)) {
            std::cerr << "Falha ao conectar ao Host B\n";
            return g_running ? EXIT_FAILURE : 0;
        }
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(control.fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error) {
            if (error) errno = error;
            perror("TCP TX");
            return EXIT_FAILURE;
        }
    }

    const Socket packet(socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, 0));
    uint8_t local[6], remote[6];
    if (!configure_measurement_socket(packet.fd, iface, true, local)) return EXIT_FAILURE;
    sockaddr_ll destination{};
    socklen_t destination_len = sizeof(destination);
    if (getsockname(packet.fd, reinterpret_cast<sockaddr*>(&destination), &destination_len) < 0) {
        perror("getsockname AF_PACKET");
        return EXIT_FAILURE;
    }
    destination.sll_protocol = htons(ETH_P_IP);
    destination.sll_halen = ETH_ALEN;
    if (!exchange_mac(control.fd, true, local, remote)) {
        std::cerr << "Falha ao iniciar sessao de controle\n";
        return g_running ? EXIT_FAILURE : 0;
    }
    std::memcpy(destination.sll_addr, remote, ETH_ALEN);

    std::ofstream csv(csv_path);
    if (!csv) {
        std::cerr << "Erro ao abrir arquivo CSV: " << csv_path << "\n";
        return EXIT_FAILURE;
    }
    csv << "elapsed_sec,seq_id,frame_size_bytes,latency_us\n" << std::fixed << std::setprecision(4);

    const std::vector<size_t> sizes{64, 128, 256, 512, 1024, 1518};
    auto frames = build_frames(sizes, local, remote, MEASUREMENT_PORT);
    std::unordered_map<size_t, FrameStats> stats;
    std::mutex dashboard_mutex;
    std::mt19937_64 sequence_seed(std::random_device{}());
    uint64_t first = sequence_seed() & (UINT64_MAX >> 1);
    size_t size_index = 0;
    size_t batches = 0, invalid_rx = 0;
    const auto start = Clock::now();
    const char* failure = nullptr;

    while (g_running) {
        std::vector<PendingPacket> sent;
        sent.reserve(batch_size);
        for (size_t i = 0; i < batch_size && g_running; ++i) {
            auto& frame = frames[size_index];
            const PacketPayload payload{htobe64(MAGIC_TAG), htobe64(first + i)};
            std::memcpy(frame.data() + UDP_PAYLOAD_OFFSET, &payload, sizeof(payload));
            const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
            if (sendto(packet.fd, frame.data(), frame.size(), MSG_DONTWAIT,
                       reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) !=
                static_cast<ssize_t>(frame.size())) {
                failure = "falha ao enviar quadro";
                break;
            }
            const auto tx_ns = read_tx_timestamp(packet.fd);
            if (!tx_ns) {
                failure = "timestamp de hardware TX ausente";
                break;
            }
            sent.push_back({tx_ns, static_cast<uint16_t>(sizes[size_index]), elapsed});
            size_index = (size_index + 1) % sizes.size();
            std::this_thread::sleep_for(200us);
        }
        if (failure || !g_running) break;

        BatchHeader request{htobe64(first), htobe64(sent.size())};
        BatchHeader response{};
        const auto deadline = Clock::now() + 5s;
        if (!transfer(control.fd, request.data(), sizeof(request), true, deadline) ||
            !transfer(control.fd, response.data(), sizeof(response), false, deadline) ||
            be64toh(response[0]) != first || be64toh(response[1]) > sent.size()) {
            failure = "resposta de controle ausente ou invalida";
            break;
        }
        std::vector<RxRecord> records(be64toh(response[1]));
        if (!transfer(control.fd, records.data(), records.size() * sizeof(RxRecord), false, deadline)) {
            failure = "resposta de controle incompleta";
            break;
        }

        std::vector<bool> received(sent.size());
        for (const auto& record : records) {
            const auto seq = be64toh(record[0]);
            if (seq < first || seq - first >= sent.size() || received[seq - first]) {
                failure = "sequencia de recepcao invalida ou duplicada";
                break;
            }
            received[seq - first] = true;
        }
        if (failure) break;

        for (const auto& item : sent) ++stats[item.frame_size].sent;
        for (const auto& record : records) {
            const auto seq = be64toh(record[0]);
            const auto rx_ns = be64toh(record[1]);
            const auto& tx = sent[seq - first];
            auto& group = stats[tx.frame_size];
            ++group.received;
            if (rx_ns < tx.tx_ns) {
                ++invalid_rx;
                continue;
            }
            const double latency = static_cast<double>(rx_ns - tx.tx_ns) / 1000.0;
            group.latencies_us.push_back(latency);
            csv << tx.elapsed_sec << ',' << seq << ',' << tx.frame_size << ',' << latency << '\n';
        }
        csv.flush();
        if (!csv) {
            std::cerr << "Falha ao gravar " << csv_path << "\n";
            return EXIT_FAILURE;
        }

        ++batches;
        first += sent.size();
        print_dashboard(sizes, stats, dashboard_mutex,
                        std::chrono::duration<double>(Clock::now() - start).count(),
                        csv_path, isatty(STDOUT_FILENO));
        std::cout << "Lotes concluidos: " << batches
                  << " | Quadros recebidos sem latencia valida: " << invalid_rx << "\n" << std::flush;
    }

    if (failure && g_running) {
        std::cerr << "Lote inconclusivo: " << failure << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "Sessao encerrada. CSV com os lotes concluidos: " << csv_path << "\n";
    return 0;
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2 || (std::strcmp(argv[1], "--tx") != 0 && std::strcmp(argv[1], "--rx") != 0)) {
        if (argc >= 3) return run_local(argc, argv);
    } else {
        const bool tx = std::strcmp(argv[1], "--tx") == 0;
        if ((!tx && argc == 4) || (tx && argc >= 4 && argc <= 6)) {
            in_addr ip{};
            if (inet_pton(AF_INET, argv[3], &ip) != 1) {
                if (tx) {
                    std::cerr << "Informe o endereco IPv4 do Host B para o controle\n";
                    return EXIT_FAILURE;
                }
                if (!resolve_control_interface(argv[3], ip)) return EXIT_FAILURE;
            }

            size_t batch_size = 600;
            if (tx && argc == 6) {
                char* end = nullptr;
                const auto count = std::strtoul(argv[5], &end, 10);
                if (end == argv[5] || *end != '\0' || count == 0 || count > MAX_BATCH_SIZE) {
                    std::cerr << "O tamanho do lote deve estar entre 1 e " << MAX_BATCH_SIZE << "\n";
                    return EXIT_FAILURE;
                }
                batch_size = count;
            }
            std::signal(SIGINT, signal_handler);
            std::signal(SIGTERM, signal_handler);
            return tx ? run_remote_tx(argv[2], ip, batch_size, argc >= 5 ? argv[4] : "latency_results.csv")
                      : run_remote_rx(argv[2], ip);
        }
    }
    std::cerr << "Uso:\n"
              << "  " << argv[0] << " <iface_tx> <iface_rx> [saida.csv]\n"
              << "  " << argv[0] << " --rx <iface_rx> <iface_controle|ip_local_controle>\n"
              << "  " << argv[0] << " --tx <iface_tx> <ip_host_B> [saida.csv] [quadros_por_lote]\n";
    return EXIT_FAILURE;
}
