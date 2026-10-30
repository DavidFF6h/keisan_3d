// common/protocol.cpp
#include "protocol.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <csignal>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
#endif

// ------------------------------------------------------------ socket plumbing --

bool net_init() {
#if defined(_WIN32)
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    // Writing to a socket the other side has closed would normally kill the whole
    // process with SIGPIPE. Ignore it; send() will just return an error instead.
    std::signal(SIGPIPE, SIG_IGN);
    return true;
#endif
}

void close_socket(int fd) {
#if defined(_WIN32)
    closesocket(fd);
#else
    close(fd);
#endif
}

bool set_socket_timeout(int fd, int milliseconds) {
#if defined(_WIN32)
    DWORD ms = (DWORD)milliseconds;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms)) == 0;
#else
    timeval tv;
    tv.tv_sec  = milliseconds / 1000;
    tv.tv_usec = (milliseconds % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

// ------------------------------------------------------------------- framing --

namespace {

// send() and recv() may move FEWER bytes than asked for. These loops keep going
// until everything has been transferred, or the connection fails.
bool send_all(int fd, const std::uint8_t *data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        int chunk = (int)std::min<std::size_t>(size - sent, 1 << 20);
        auto n = send(fd, (const char *)data + sent, chunk, 0);
        if (n <= 0) return false;
        sent += (std::size_t)n;
    }
    return true;
}

bool recv_all(int fd, std::uint8_t *data, std::size_t size) {
    std::size_t got = 0;
    while (got < size) {
        int chunk = (int)std::min<std::size_t>(size - got, 1 << 20);
        auto n = recv(fd, (char *)data + got, chunk, 0);
        if (n <= 0) return false;      // 0 = the peer closed; <0 = error or timeout
        got += (std::size_t)n;
    }
    return true;
}

// Integers are sent most-significant byte first ("big-endian", a.k.a. network
// byte order), written out by hand so it works the same on every CPU.
void put_u32(std::vector<std::uint8_t> &b, std::uint32_t v) {
    b.push_back((std::uint8_t)(v >> 24));
    b.push_back((std::uint8_t)(v >> 16));
    b.push_back((std::uint8_t)(v >> 8));
    b.push_back((std::uint8_t)v);
}

void put_u64(std::vector<std::uint8_t> &b, std::uint64_t v) {
    put_u32(b, (std::uint32_t)(v >> 32));
    put_u32(b, (std::uint32_t)v);
}

// Floats travel as their raw IEEE-754 bits. memcpy is the well-defined way to
// view a float's bytes as an integer.
void put_f32(std::vector<std::uint8_t> &b, float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    put_u32(b, u);
}

// Reads values back out of a received buffer, throwing instead of running off the end.
class byte_reader {
public:
    explicit byte_reader(const std::vector<std::uint8_t> &buf) : buf_(buf) {}

    std::uint32_t u32() {
        need(4);
        std::uint32_t v = ((std::uint32_t)buf_[pos_] << 24) | ((std::uint32_t)buf_[pos_ + 1] << 16) |
                          ((std::uint32_t)buf_[pos_ + 2] << 8) | (std::uint32_t)buf_[pos_ + 3];
        pos_ += 4;
        return v;
    }
    std::uint64_t u64() {
        std::uint64_t hi = u32();
        return (hi << 32) | u32();
    }
    float f32() {
        std::uint32_t u = u32();
        float f;
        std::memcpy(&f, &u, sizeof(f));
        return f;
    }
    std::string text(std::size_t n) {
        need(n);
        std::string s(buf_.begin() + pos_, buf_.begin() + pos_ + n);
        pos_ += n;
        return s;
    }
    std::size_t remaining() const { return buf_.size() - pos_; }

private:
    void need(std::size_t n) const {
        if (buf_.size() - pos_ < n) throw std::runtime_error("message is truncated");
    }
    const std::vector<std::uint8_t> &buf_;
    std::size_t pos_ = 0;
};

} // namespace

bool send_message(int fd, message_type type, const std::vector<std::uint8_t> &payload) {
    if (payload.size() > MAX_PAYLOAD) return false;

    std::vector<std::uint8_t> frame;
    frame.reserve(5 + payload.size());
    frame.push_back((std::uint8_t)type);
    put_u32(frame, (std::uint32_t)payload.size());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return send_all(fd, frame.data(), frame.size());
}

bool recv_message(int fd, message_type &type, std::vector<std::uint8_t> &payload) {
    std::uint8_t header[5];
    if (!recv_all(fd, header, sizeof(header))) return false;

    std::uint32_t length = ((std::uint32_t)header[1] << 24) | ((std::uint32_t)header[2] << 16) |
                           ((std::uint32_t)header[3] << 8) | (std::uint32_t)header[4];
    if (length > MAX_PAYLOAD) return false;   // refuse before allocating anything

    type = (message_type)header[0];
    payload.resize(length);
    return length == 0 || recv_all(fd, payload.data(), length);
}

// ----------------------------------------------------------------- payloads --

std::vector<std::uint8_t> encode_text(const std::string &s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

std::string decode_text(const std::vector<std::uint8_t> &b) {
    return std::string(b.begin(), b.end());
}

std::vector<std::uint8_t> encode_scalar(double value) {
    std::uint64_t u;
    std::memcpy(&u, &value, sizeof(u));
    std::vector<std::uint8_t> b;
    put_u64(b, u);
    return b;
}

double decode_scalar(const std::vector<std::uint8_t> &b) {
    byte_reader r(b);
    std::uint64_t u = r.u64();
    if (r.remaining() != 0) throw std::runtime_error("scalar reply has extra bytes");
    double d;
    std::memcpy(&d, &u, sizeof(d));
    return d;
}

std::vector<std::uint8_t> encode_graph_request(const graph_request &req) {
    std::vector<std::uint8_t> b;
    put_u32(b, (std::uint32_t)req.expr.size());
    b.insert(b.end(), req.expr.begin(), req.expr.end());
    put_f32(b, req.xmin);
    put_f32(b, req.xmax);
    put_f32(b, req.ymin);
    put_f32(b, req.ymax);
    put_u32(b, req.resolution);
    return b;
}

graph_request decode_graph_request(const std::vector<std::uint8_t> &b) {
    byte_reader r(b);
    graph_request req;

    std::uint32_t length = r.u32();
    if (length > r.remaining()) throw std::runtime_error("message is truncated");
    req.expr = r.text(length);

    req.xmin = r.f32();
    req.xmax = r.f32();
    req.ymin = r.f32();
    req.ymax = r.f32();
    req.resolution = r.u32();

    if (r.remaining() != 0) throw std::runtime_error("graph request has extra bytes");
    return req;
}

std::vector<std::uint8_t> encode_vertices(const std::vector<gridvertex> &verts) {
    std::vector<std::uint8_t> b;
    b.reserve(4 + verts.size() * 24);
    put_u32(b, (std::uint32_t)verts.size());
    for (const gridvertex &v : verts) {
        put_f32(b, v.pos.x);    put_f32(b, v.pos.y);    put_f32(b, v.pos.z);
        put_f32(b, v.normal.x); put_f32(b, v.normal.y); put_f32(b, v.normal.z);
    }
    return b;
}

std::vector<gridvertex> decode_vertices(const std::vector<std::uint8_t> &b) {
    byte_reader r(b);
    std::uint32_t count = r.u32();

    // Check the claimed count against the bytes actually present BEFORE allocating.
    if ((std::uint64_t)count * 24 != r.remaining())
        throw std::runtime_error("vertex count does not match the message size");

    std::vector<gridvertex> verts(count);
    for (gridvertex &v : verts) {
        v.pos.x    = r.f32(); v.pos.y    = r.f32(); v.pos.z    = r.f32();
        v.normal.x = r.f32(); v.normal.y = r.f32(); v.normal.z = r.f32();
    }
    return verts;
}

// ------------------------------------------------------------- round trips --

net_status request_scalar(int fd, const std::string &expr, double &result, std::string &message) {
    if (!send_message(fd, message_type::scalar, encode_text(expr))) {
        message = "lost the connection while sending";
        return net_status::connection_lost;
    }

    message_type type;
    std::vector<std::uint8_t> payload;
    if (!recv_message(fd, type, payload)) {
        message = "lost the connection (or timed out) waiting for the answer";
        return net_status::connection_lost;
    }
    if (type == message_type::error) {
        message = decode_text(payload);
        return net_status::server_error;
    }
    if (type != message_type::scalar) {
        message = "the server sent a reply we didn't expect";
        return net_status::connection_lost;
    }
    try {
        result = decode_scalar(payload);
    } catch (const std::exception &e) {
        message = e.what();
        return net_status::connection_lost;
    }
    return net_status::ok;
}

net_status request_graph(int fd, const graph_request &req,
                         std::vector<gridvertex> &verts, std::string &message)
{
    if (!send_message(fd, message_type::graph, encode_graph_request(req))) {
        message = "lost the connection while sending";
        return net_status::connection_lost;
    }

    message_type type;
    std::vector<std::uint8_t> payload;
    if (!recv_message(fd, type, payload)) {
        message = "lost the connection (or timed out) waiting for the mesh";
        return net_status::connection_lost;
    }
    if (type == message_type::error) {
        message = decode_text(payload);
        return net_status::server_error;
    }
    if (type != message_type::graph) {
        message = "the server sent a reply we didn't expect";
        return net_status::connection_lost;
    }
    try {
        verts = decode_vertices(payload);
    } catch (const std::exception &e) {
        message = e.what();
        return net_status::connection_lost;
    }
    if (verts.size() != (std::size_t)req.resolution * req.resolution) {
        message = "the server sent the wrong number of vertices";
        return net_status::connection_lost;
    }
    return net_status::ok;
}
