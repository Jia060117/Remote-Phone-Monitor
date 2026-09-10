#pragma once

// 判断当前默认路由是否走蜂窝网卡（移动数据）。
//
// 实现：解析 /proc/net/route（纯文本内核路由表，读取零开销、无需 fork），
// 取 metric 最小的默认路由（Destination == 00000000），
// 出口网卡名不以 wlan/eth/usb/tun/lo 开头即视为蜂窝（rmnet*/ccmni*/pdp*/wwan*）。
//
// 无默认路由（离线）时返回 false —— 离线时上传自会失败并入离线队列，
// 按非蜂窝处理不会造成误判。
bool net_is_cellular();
