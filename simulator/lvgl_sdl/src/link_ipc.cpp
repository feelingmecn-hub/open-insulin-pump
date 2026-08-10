/* link_ipc.cpp — 模拟器联调模式控制通道 (实现见 link_ipc.h)
 *
 * ⚠️ 实验项目 / 教学原型, 严禁用于任何人体。
 */
#include "link_ipc.h"
#include "link_session.h"
#include "ui_screen.h"   // ui_screen_key (手动按键注入固件 FSM)
#include "ui_hal.h"      // key_event_t / KEY_*
extern void link_set_board_temp_c(float c);   // P3-13: 联调板温注入 (定义于 ui_hal_link.cpp)

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <mutex>
#include <thread>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
#endif

namespace linkipc {

// 跨平台 socket 抽象: macOS/Linux 用 POSIX (int fd + close), Windows 用 Winsock (SOCKET + closesocket + WSAStartup)
#ifdef _WIN32
    typedef SOCKET  sock_t;
    #define SOCK_INVALID INVALID_SOCKET
    #define SOCK_CLOSE(s) closesocket(s)
#else
    typedef int     sock_t;
    #define SOCK_INVALID (-1)
    #define SOCK_CLOSE(s) ::close(s)
#endif

static const int LINK_PORT = 18923;

static sock_t            g_listen = SOCK_INVALID;
static std::vector<sock_t> g_clients;
static std::mutex        g_mtx;
static bool              g_run = true;

static void broadcast(const char *line)
{
    size_t n = strlen(line);
    std::lock_guard<std::mutex> lk(g_mtx);
    for (size_t i = 0; i < g_clients.size(); ) {
        int fd = g_clients[i];
        int w = ::send(fd, line, n, 0);
        if (w <= 0) { SOCK_CLOSE(fd); g_clients.erase(g_clients.begin() + (long)i); }
        else i++;
    }
}

static void build_and_broadcast(void)
{
    char steps[8192];
    char state[2048];
    char trace[12000];
    linksess::all_steps_json(steps, sizeof(steps));
    linksess::snapshot_json(state, sizeof(state));
    linksess::trace_json(trace, sizeof(trace));
    char buf[30000];
    snprintf(buf, sizeof(buf),
             "{\"t\":\"status\",\"idx\":%d,\"total\":%d,\"playing\":%s,%s,%s,\"state\":{%s}}\n",
             linksess::index(), linksess::total(),
             linksess::playing() ? "true" : "false",
             steps, trace, state);
    broadcast(buf);
}

void broadcast_status(void) { build_and_broadcast(); }

void broadcast_reset(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"t\":\"reset\",\"total\":%d}\n", linksess::total());
    broadcast(buf);
    build_and_broadcast();
}

static void handle_cmd(const char *raw)
{
    char line[70000];
    strncpy(line, raw, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    size_t L = strlen(line);
    while (L > 0 && (line[L - 1] == '\n' || line[L - 1] == '\r' || line[L - 1] == ' '))
        line[--L] = '\0';
    if (line[0] == '\0') return;

    if (strcmp(line, "play") == 0) {
        linksess::set_playing(true);
        build_and_broadcast();
    } else if (strcmp(line, "pause") == 0) {
        linksess::set_playing(false);
        build_and_broadcast();
    } else if (strcmp(line, "step") == 0) {
        linksess::step();
        build_and_broadcast();
    } else if (strcmp(line, "reset") == 0) {
        linksess::reset();
        broadcast_reset();
    } else if (strncmp(line, "delay ", 6) == 0) {
        int d = atoi(line + 6);
        if (d >= 0) { linksess::set_delay(d); build_and_broadcast(); }
    } else if (strncmp(line, "mode ", 5) == 0) {
        const char *m = line + 5;
        if (strcmp(m, "replay") == 0)      linksess::set_mode(1);
        else if (strcmp(m, "script") == 0) linksess::set_mode(0);
        build_and_broadcast();
    } else if (strncmp(line, "data ", 5) == 0) {
        linksess::load_dataset(line + 5);   // 载入血糖曲线 + 基础率档案
        build_and_broadcast();
    } else if (strncmp(line, "key ", 4) == 0) {
        // 手动控制: 把泵屏 4 个物理按键(上/下/确认/返回)注入固件 FSM
        const char *k = line + 4;
        if (strcmp(k, "release") == 0) {
            ui_screen_release();     // 物理按键释放 -> 停止"长按自动重复"
        } else {
            key_event_t ke; bool ok = false;
            if      (strcmp(k, "up")   == 0) { ke = KEY_UP;   ok = true; }
            else if (strcmp(k, "down") == 0) { ke = KEY_DOWN; ok = true; }
            else if (strcmp(k, "set")  == 0) { ke = KEY_SET;  ok = true; }
            else if (strcmp(k, "esc")  == 0) { ke = KEY_ESC;  ok = true; }
            if (ok) {
                ui_screen_key(ke);
                build_and_broadcast();   // 按键后立即推送新界面状态给控制面板
            }
        }
    } else if (strncmp(line, "thermal ", 8) == 0) {
        // P3-13: 注入板温(°C)以演示过温预警/报警状态机, 如 "thermal 62.5"
        float c = (float)atof(line + 8);
        link_set_board_temp_c(c);
        build_and_broadcast();
    }
}

static void client_loop(sock_t fd)
{
    char buf[70000];
    while (g_run) {
        int r = ::recv(fd, buf, sizeof(buf) - 1, 0);
        if (r <= 0) break;
        buf[r] = '\0';
        char *p = buf;
        while (*p) {
            char *nl = strchr(p, '\n');
            if (!nl) { handle_cmd(p); break; }
            *nl = '\0';
            handle_cmd(p);
            p = nl + 1;
        }
    }
    SOCK_CLOSE(fd);
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto it = g_clients.begin(); it != g_clients.end(); ++it) {
        if (*it == fd) { g_clients.erase(it); break; }
    }
}

static void server_loop(void)
{
    while (g_run) {
        sock_t fd = ::accept(g_listen, nullptr, nullptr);
        if (fd == SOCK_INVALID) { if (!g_run) break; continue; }
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_clients.push_back(fd);
        }
        build_and_broadcast();   // 新连接立即推送当前状态
        std::thread(client_loop, fd).detach();
    }
}

void start(void)
{
#ifdef _WIN32
    // Windows 必须先初始化 Winsock, 否则任何 socket 调用都失败
    { WSADATA wsa; if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "[link_ipc] WSAStartup failed\n"); return; } }
#endif
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen == SOCK_INVALID) { perror("link_ipc socket"); return; }
    int opt = 1;
    ::setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(LINK_PORT);

    if (bind(g_listen, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("link_ipc bind"); SOCK_CLOSE(g_listen); g_listen = SOCK_INVALID; return;
    }
    if (listen(g_listen, 8) < 0) {
        perror("link_ipc listen"); SOCK_CLOSE(g_listen); g_listen = SOCK_INVALID; return;
    }
    printf("[link_ipc] 控制通道已启动 127.0.0.1:%d\n", LINK_PORT);
    fflush(stdout);
    std::thread(server_loop).detach();
}

} // namespace linkipc
