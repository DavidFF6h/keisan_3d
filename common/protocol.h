// common/protocol.h
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "mesh.h"

// ---- limits both sides agree on -------------------------------------------
// Never trust a number that arrived over the network. These caps stop a buggy or
// hostile peer from making us allocate gigabytes or burn minutes of CPU.
constexpr std::uint32_t MAX_PAYLOAD    = 32u * 1024 * 1024;   // largest message body we accept
constexpr std::uint32_t MAX_RESOLUTION = 400;                 // grid is at most 400 x 400

// ---- message types ----------------------------------------------------------
enum class message_type : std::uint8_t {
    scalar = 1,     // request: "3 + 4 * 2"      reply: one double
    graph  = 2,     // request: formula + domain reply: a mesh
    error  = 255,   // reply only: a human-readable explanation
};

struct graph_request {
    std::string   expr;
    float         xmin, xmax, ymin, ymax;
    std::uint32_t resolution;
};

// ---- socket plumbing (hides the Linux / macOS / Windows differences) -------
bool net_init();                                   // call once at startup
void close_socket(int fd);
bool set_socket_timeout(int fd, int milliseconds); // applies to recv and send

// ---- framing: every message is  [type: 1 byte][length: 4 bytes][payload] ----
bool send_message(int fd, message_type type, const std::vector<std::uint8_t> &payload);
bool recv_message(int fd, message_type &type, std::vector<std::uint8_t> &payload);

// ---- payload encodings. The decode_* functions throw std::runtime_error ----
// ---- if the bytes are malformed.                                         ----
std::vector<std::uint8_t> encode_text(const std::string &s);
std::string               decode_text(const std::vector<std::uint8_t> &b);

std::vector<std::uint8_t> encode_scalar(double value);
double                    decode_scalar(const std::vector<std::uint8_t> &b);

std::vector<std::uint8_t> encode_graph_request(const graph_request &req);
graph_request             decode_graph_request(const std::vector<std::uint8_t> &b);

std::vector<std::uint8_t> encode_vertices(const std::vector<gridvertex> &verts);
std::vector<gridvertex>   decode_vertices(const std::vector<std::uint8_t> &b);

// ---- client-side conveniences: one call = one request/response round trip ----
enum class net_status {
    ok,
    server_error,      // the server answered with an error message; connection is still fine
    connection_lost,   // the connection broke or timed out; close it and reconnect
};

net_status request_scalar(int fd, const std::string &expr, double &result, std::string &message);
net_status request_graph(int fd, const graph_request &req,
                         std::vector<gridvertex> &verts, std::string &message);
