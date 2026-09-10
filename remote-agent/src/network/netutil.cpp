#include "netutil.h"
#include <cstdio>
#include <cstring>

bool net_is_cellular() {
    std::FILE* fp = std::fopen("/proc/net/route", "r");
    if (!fp) return false;

    char line[512];
    if (!std::fgets(line, sizeof(line), fp)) {  // 跳过表头
        std::fclose(fp);
        return false;
    }

    bool   have_default  = false;
    unsigned long best_metric = 0;
    char   best_iface[64] = {0};

    while (std::fgets(line, sizeof(line), fp)) {
        char iface[64] = {0};
        unsigned long dest = 1, gateway = 0, flags = 0, refcnt = 0, use = 0, metric = 0;
        if (std::sscanf(line, "%63s %lx %lx %lx %lx %lx %lx",
                        iface, &dest, &gateway, &flags, &refcnt, &use, &metric) < 7)
            continue;
        if (dest != 0 || !iface[0]) continue;          // 只看默认路由
        if (!have_default || metric < best_metric) {    // metric 小者优先级高
            have_default  = true;
            best_metric   = metric;
            std::strncpy(best_iface, iface, sizeof(best_iface) - 1);
        }
    }
    std::fclose(fp);

    if (!have_default) return false;
    return std::strncmp(best_iface, "wlan", 4) != 0
        && std::strncmp(best_iface, "eth",  3) != 0
        && std::strncmp(best_iface, "usb",  3) != 0
        && std::strncmp(best_iface, "tun",  3) != 0
        && std::strncmp(best_iface, "lo",   2) != 0;
}
