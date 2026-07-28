/* 链接模式核心冒烟测试 (不依赖 LVGL/SDL)
 * 直接驱动 linksess::init/step/reset, 验证 g_pump_state 单定义 + 桩完整性 + 17 步逻辑。
 * 编译见 test/build_link_smoke.sh
 */
#include "link_session.h"
#include "link_ipc.h"
#include <cstdio>

int main(void)
{
    linksess::init();
    linkipc::start();

    int pass = 0, fail = 0;
    while (linksess::step()) {
        // 累积检查 (从 all_steps_json 解析太重, 这里只打印进度)
    }
    int done = linksess::index();
    printf(">>> 链接模式会话执行完成: 步数=%d (期望 17)\n", done);

    char steps[9000];
    linksess::all_steps_json(steps, sizeof(steps));
    // 粗略统计 ok:true / ok:false 出现次数
    const char *p = steps;
    while ((p = strstr(p, "\"ok\":"))) {
        p += 5;
        if (strncmp(p, "true", 4) == 0) pass++;
        else if (strncmp(p, "false", 5) == 0) fail++;
    }
    printf(">>> 检查项: PASS=%d FAIL=%d\n", pass, fail);

    char state[2048];
    linksess::snapshot_json(state, sizeof(state));
    printf(">>> 末态快照: %s\n", state);

    if (done == 17 && fail == 0) {
        printf(">>> ✅ 链接模式核心 OK\n");
        return 0;
    }
    printf(">>> ❌ 链接模式核心异常\n");
    return 1;
}
