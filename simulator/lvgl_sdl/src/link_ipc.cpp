/* link_ipc.cpp — 模拟器联调模式控制通道 (实现见 link_ipc.h)
 *
 * ⚠️ 实验项目 / 教学原型, 严禁用于任何人体。
 */
#include "link_ipc.h"
#include "link_session.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <mutex>
#include <thread>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace linkipc {

static const int LINK_PORT = 18923;

static int               g_listen = -1;
static std::vector<int>  g_clients;
static std::mutex        g_mtx;
static bool              g_run = true;

static void broadcast(const char *line)
{
    size_t n = strlen(line);
    std::lock_guard<std::mutex> lk(g_mtx);
    for (size_t i = 0; i < g_clients.size(); ) {
        int fd = g_clients[i];
        ssize_t w = ::send(fd, line, n, 0);
        if (w <= 0) { ::close(fd); g_clients.erase(g_clients.begin() + (long)i); }
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
    char line[256];
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
    }
}

static void client_loop(int fd)
{
    char buf[512];
    while (g_run) {
        ssize_t r = ::recv(fd, buf, sizeof(buf) - 1, 0);
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
    ::close(fd);
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto it = g_clients.begin(); it != g_clients.end(); ++it) {
        if (*it == fd) { g_clients.erase(it); break; }
    }
}

static void server_loop(void)
{
    while (g_run) {
        int fd = ::accept(g_listen, nullptr, nullptr);
        if (fd < 0) { if (!g_run) break; continue; }
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
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen < 0) { perror("link_ipc socket"); return; }
    int opt = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(LINK_PORT);

    if (bind(g_listen, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("link_ipc bind"); ::close(g_listen); g_listen = -1; return;
    }
    if (listen(g_listen, 8) < 0) {
        perror("link_ipc listen"); ::close(g_listen); g_listen = -1; return;
    }
    printf("[link_ipc] 控制通道已启动 127.0.0.1:%d\n", LINK_PORT);
    fflush(stdout);
    std::thread(server_loop).detach();
}

} // namespace linkipc
