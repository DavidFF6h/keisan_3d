// client/main.cpp
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <netdb.h>
  #include <sys/socket.h>
#endif

#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include "mesh.h"
#include "protocol.h"
#include "rasterizer.h"

// ------------------------------------------------------------------ networking --

// Opens a TCP connection to host:port. Returns the socket, or -1 on failure.
static int connect_to_server(const std::string &host, int port) {
    // getaddrinfo turns "127.0.0.1" or "localhost" or "myserver.local" into an address.
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *found = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0) return -1;

    int fd = -1;
    for (addrinfo *a = found; a; a = a->ai_next) {
        int s = (int)socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s < 0) continue;
        if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) { fd = s; break; }
        close_socket(s);
    }
    freeaddrinfo(found);

    // Never wait forever for a reply: after 10 s a request fails instead of freezing the window.
    if (fd >= 0) set_socket_timeout(fd, 10 * 1000);
    return fd;
}

// ------------------------------------------------------------------ app state --

struct app_state {
    std::string host;
    int         port = 5000;
    int         fd   = -1;                       // -1 = not connected

    // calculator panel
    char        calc_input[128] = "3 + 4 * 2";
    std::string calc_output;

    // graph panel
    char  formula[128]  = "sin(x) * cos(y)";
    float domain[4]     = { -3.14159f, 3.14159f, -3.14159f, 3.14159f };   // xmin xmax ymin ymax
    int   resolution    = 60;

    std::vector<gridvertex> verts;               // the mesh we last received
    std::vector<int>        indices;
    orbit_camera            camera;
    bool                    needs_render = true; // redraw only when something changed

    std::string status;
};

static void reconnect(app_state &s) {
    if (s.fd >= 0) { close_socket(s.fd); s.fd = -1; }
    s.fd = connect_to_server(s.host, s.port);
    s.status = s.fd >= 0 ? "Connected to " + s.host + ":" + std::to_string(s.port)
                         : "Could not connect to " + s.host + ":" + std::to_string(s.port) +
                           ". Is the server running?";
}

// Handles a failed round trip the same way everywhere.
static void handle_failure(app_state &s, net_status st, const std::string &message) {
    if (st == net_status::connection_lost) {
        close_socket(s.fd);
        s.fd = -1;
        s.status = "Disconnected: " + message;
    } else {
        s.status = "Server says: " + message;
    }
}

static void do_calculate(app_state &s) {
    double result = 0;
    std::string message;
    net_status st = request_scalar(s.fd, s.calc_input, result, message);
    if (st == net_status::ok) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "= %.12g", result);
        s.calc_output = buf;
        s.status = "Calculation OK";
    } else {
        s.calc_output.clear();
        handle_failure(s, st, message);
    }
}

static void do_graph(app_state &s) {
    graph_request req = { s.formula, s.domain[0], s.domain[1], s.domain[2], s.domain[3],
                          (std::uint32_t)s.resolution };
    std::vector<gridvertex> verts;
    std::string message;

    Uint32 started = SDL_GetTicks();
    net_status st = request_graph(s.fd, req, verts, message);
    Uint32 elapsed = SDL_GetTicks() - started;

    if (st == net_status::ok) {
        s.verts = std::move(verts);
        s.indices = build_indices(s.resolution);
        s.needs_render = true;
        s.status = "Received " + std::to_string(s.verts.size()) + " points in " +
                   std::to_string(elapsed) + " ms (network + server compute)";
    } else {
        handle_failure(s, st, message);
    }
}

// --------------------------------------------------------------------- window --

static SDL_Texture *g_texture = nullptr;

// Copies the CPU-rendered pixels into a texture the GPU (or SDL) can put on screen.
static void upload_framebuffer(SDL_Renderer *renderer, const framebuffer &fb) {
    if (!g_texture) {
        g_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STREAMING, fb.width, fb.height);
    }
    SDL_UpdateTexture(g_texture, nullptr, fb.pixels.data(), fb.width * (int)sizeof(color));
}

static int run_gui(app_state &s) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow("3D Graphing Calculator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1100, 640, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!window || !renderer) {
        std::fprintf(stderr, "could not create the window: %s\n", SDL_GetError());
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    framebuffer fb(640, 480);
    zbuffer     zb(640, 480);

    // Draw something straight away so the window doesn't start out empty.
    if (s.fd >= 0) do_graph(s);

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // ---- left panel: connection, calculator, graph settings ----------
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(380, 600), ImGuiCond_FirstUseEver);
        ImGui::Begin("Controls");

        ImGui::Text("Server: %s:%d  [%s]", s.host.c_str(), s.port,
                    s.fd >= 0 ? "connected" : "not connected");
        if (ImGui::Button("Reconnect")) reconnect(s);
        ImGui::Separator();

        ImGui::BeginDisabled(s.fd < 0);   // grey everything out while disconnected

        ImGui::Text("Calculator");
        bool enter = ImGui::InputText("##calc", s.calc_input, sizeof(s.calc_input),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button("=") || enter) && s.fd >= 0) do_calculate(s);
        ImGui::TextUnformatted(s.calc_output.c_str());
        ImGui::Separator();

        ImGui::Text("3D graph: z = f(x, y)");
        ImGui::InputText("f(x, y)", s.formula, sizeof(s.formula));
        ImGui::InputFloat2("x range", &s.domain[0]);   // xmin, xmax
        ImGui::InputFloat2("y range", &s.domain[2]);   // ymin, ymax
        ImGui::SliderInt("resolution", &s.resolution, 10, 200);
        if (ImGui::Button("Graph") && s.fd >= 0) do_graph(s);

        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::TextWrapped("%s", s.status.c_str());
        ImGui::Separator();
        ImGui::TextWrapped("Drag in the graph to orbit. Mouse wheel to zoom.");
        ImGui::TextWrapped("Functions: sin cos tan sqrt abs exp log");
        ImGui::TextWrapped("Operators: + - * / ^     Constants: pi e");
        ImGui::End();

        // ---- right panel: the rendered graph --------------------------------
        ImGui::SetNextWindowPos(ImVec2(400, 10), ImGuiCond_FirstUseEver);
        ImGui::Begin("Graph", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        // Redraw only when the mesh or the camera changed.
        if (s.needs_render) {
            render_scene(fb, zb, s.camera, s.verts, s.indices);
            upload_framebuffer(renderer, fb);
            s.needs_render = false;
        }

        ImVec2 size((float)fb.width, (float)fb.height);
        ImVec2 top_left = ImGui::GetCursorScreenPos();
        ImGui::Image((ImTextureID)(intptr_t)g_texture, size);

        // An invisible button laid over the image captures the mouse, so dragging
        // orbits the camera instead of dragging the whole window around.
        ImGui::SetCursorScreenPos(top_left);
        ImGui::InvisibleButton("view", size);
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f)) {
            ImVec2 d = ImGui::GetIO().MouseDelta;
            s.camera.yaw   -= d.x * 0.01f;
            s.camera.pitch += d.y * 0.01f;
            s.camera.clamp();
            s.needs_render = true;
        }
        if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.f) {
            s.camera.distance *= std::pow(0.9f, ImGui::GetIO().MouseWheel);
            s.camera.clamp();
            s.needs_render = true;
        }
        ImGui::End();

        // ---- put it all on screen -------------------------------------------
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 36, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    if (g_texture) SDL_DestroyTexture(g_texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

// ----------------------------------------------------------------------- main --

static void print_usage(const char *program) {
    std::fprintf(stderr,
        "usage:\n"
        "  %s [host] [port]                    open the graphing window\n"
        "  %s --calc \"expression\" [host] [port]  print one result and exit\n"
        "defaults: host 127.0.0.1, port 5000\n", program, program);
}

int main(int argc, char **argv) {
    app_state s;
    s.host = "127.0.0.1";

    bool calc_mode = false;
    std::string calc_expression;
    int next = 1;
    if (argc > 1 && std::strcmp(argv[1], "--calc") == 0) {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        calc_mode = true;
        calc_expression = argv[2];
        next = 3;
    }
    if (argc > next)     s.host = argv[next];
    if (argc > next + 1) s.port = std::atoi(argv[next + 1]);

    if (!net_init()) { std::fprintf(stderr, "could not initialise networking\n"); return 1; }

    if (calc_mode) {
        // Text-only mode: connect, ask, print, quit. No window needed.
        int fd = connect_to_server(s.host, s.port);
        if (fd < 0) { std::fprintf(stderr, "could not connect to %s:%d\n", s.host.c_str(), s.port); return 1; }

        double result = 0;
        std::string message;
        net_status st = request_scalar(fd, calc_expression, result, message);
        close_socket(fd);
        if (st != net_status::ok) { std::fprintf(stderr, "error: %s\n", message.c_str()); return 1; }
        std::printf("%.12g\n", result);
        return 0;
    }

    reconnect(s);
    return run_gui(s);
}
