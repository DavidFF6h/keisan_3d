// server/main.cpp
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
#endif

#include "mesh.h"
#include "parser.h"
#include "protocol.h"
#include "simd_eval.h"

// ------------------------------------------------------------------ logging --
// Many threads print at once; the mutex keeps their lines from interleaving.
static std::mutex g_log_mutex;

static void log_line(const std::string &peer, const std::string &text) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::printf("[%s] %s\n", peer.c_str(), text.c_str());
    std::fflush(stdout);
}

// ---------------------------------------------------------- request handlers --
// Each handler turns a request payload into a reply payload. If anything is wrong
// it throws std::runtime_error, and handle_client turns that into an error reply.

static std::vector<std::uint8_t> handle_scalar(const std::vector<std::uint8_t> &payload,
                                               std::string &log)
{
    std::string text = decode_text(payload);
    node_ptr ast = parse_expression(text);
    if (uses_variables(ast.get()))
        throw std::runtime_error("x and y only make sense when graphing; use a graph request");

    double result = eval(ast.get(), 0.0, 0.0);
    log = "calc  \"" + text + "\" = " + std::to_string(result);
    return encode_scalar(result);
}

static std::vector<std::uint8_t> handle_graph(const std::vector<std::uint8_t> &payload,
                                              std::string &log)
{
    graph_request req = decode_graph_request(payload);

    // Validate everything the client told us before acting on any of it.
    if (req.resolution < 2 || req.resolution > MAX_RESOLUTION)
        throw std::runtime_error("resolution must be between 2 and " + std::to_string(MAX_RESOLUTION));
    if (!std::isfinite(req.xmin) || !std::isfinite(req.xmax) ||
        !std::isfinite(req.ymin) || !std::isfinite(req.ymax) ||
        req.xmin >= req.xmax || req.ymin >= req.ymax)
        throw std::runtime_error("the domain needs xmin < xmax and ymin < ymax");

    node_ptr ast = parse_expression(req.expr);
    int res = (int)req.resolution;

    // 1. The grid: res*res points with x and y filled in.
    std::vector<gridvertex> verts = build_grid(req.xmin, req.xmax, req.ymin, req.ymax, res);

    // 2. Copy the coordinates into two flat arrays. SIMD wants to load 8 x-values
    //    in one go, so they must sit side by side in memory, not interleaved with y and z.
    std::size_t n = verts.size();
    std::vector<float> xs(n), ys(n), zs(n);
    for (std::size_t i = 0; i < n; i++) {
        xs[i] = verts[i].pos.x;
        ys[i] = verts[i].pos.y;
    }

    // 3. The heavy part: evaluate the formula at every point, 8 (or 4) at a time.
    auto t0 = std::chrono::steady_clock::now();
    evaluate_grid_simd(ast.get(), xs.data(), ys.data(), zs.data(), n);
    auto t1 = std::chrono::steady_clock::now();
    double eval_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // 4. Store the heights, then work out which way the surface faces everywhere.
    for (std::size_t i = 0; i < n; i++) verts[i].pos.z = zs[i];
    compute_normals(verts, build_indices(res));

    char buf[64];
    std::snprintf(buf, sizeof(buf), " -> %zu points, evaluated in %.3f ms", n, eval_ms);
    log = "graph \"" + req.expr + "\" res=" + std::to_string(res) + buf;
    return encode_vertices(verts);
}

// ------------------------------------------------------------ one connection --
// Runs on its own thread, so a slow or stuck client never blocks anyone else.

static void handle_client(int client_fd, std::string peer) {
    log_line(peer, "connected");

    for (;;) {
        message_type type;
        std::vector<std::uint8_t> payload;
        if (!recv_message(client_fd, type, payload)) break;   // disconnected, or sent garbage

        message_type reply_type = type;
        std::vector<std::uint8_t> reply;
        std::string log;
        try {
            switch (type) {
                case message_type::scalar: reply = handle_scalar(payload, log); break;
                case message_type::graph:  reply = handle_graph(payload, log);  break;
                default: throw std::runtime_error("unknown message type");
            }
            log_line(peer, log);
        } catch (const std::exception &e) {
            // A bad request must never take the server down: report it and carry on.
            reply_type = message_type::error;
            reply = encode_text(e.what());
            log_line(peer, std::string("rejected request: ") + e.what());
        }

        if (!send_message(client_fd, reply_type, reply)) break;
    }

    close_socket(client_fd);
    log_line(peer, "disconnected");
}

// ----------------------------------------------------------------- the server --

static int make_server_socket(int port) {
    // 1. socket(): ask the OS for a TCP endpoint.
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    // Let us restart the server immediately instead of waiting for the old port to time out.
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    // 2. bind(): attach it to a port on all of this machine's network interfaces.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(fd, (sockaddr *)&addr, sizeof(addr)) != 0) { close_socket(fd); return -1; }

    // 3. listen(): start queueing incoming connections (up to 16 waiting).
    if (listen(fd, 16) != 0) { close_socket(fd); return -1; }
    return fd;
}

int main(int argc, char **argv) {
    int port = argc > 1 ? std::atoi(argv[1]) : 5000;
    if (port < 1 || port > 65535) {
        std::fprintf(stderr, "usage: %s [port]\n", argv[0]);
        return 1;
    }
    if (!net_init()) { std::fprintf(stderr, "could not initialise networking\n"); return 1; }

    int server_fd = make_server_socket(port);
    if (server_fd < 0) {
        std::fprintf(stderr, "could not listen on port %d (is it already in use?)\n", port);
        return 1;
    }
    std::printf("server: listening on port %d, SIMD backend: %s\n", port, simd_backend_name());
    std::fflush(stdout);

    for (;;) {
        // 4. accept(): wait for a client. Returns a NEW socket for that one client;
        //    server_fd stays free to accept the next.
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int client_fd = (int)accept(server_fd, (sockaddr *)&client_addr, &len);
        if (client_fd < 0) continue;

        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        std::string peer = std::string(ip) + ":" + std::to_string(ntohs(client_addr.sin_port));

        // A client that goes silent shouldn't hold a thread forever.
        set_socket_timeout(client_fd, 5 * 60 * 1000);

        // One thread per client. detach(): nobody will join it; it cleans up
        // after itself when handle_client returns.
        std::thread(handle_client, client_fd, peer).detach();
    }
}
