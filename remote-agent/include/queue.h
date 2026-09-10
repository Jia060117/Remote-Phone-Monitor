#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <optional>

struct QueuedItem {
    std::vector<uint8_t> data;
    int64_t ts = 0;
    std::string path;  // 内部用：.sending 文件路径，confirm 时删除
};

class OfflineQueue {
public:
    OfflineQueue(const std::string& dir, int max_mb = 200);

    // 保存到离线队列（原子写入：先写 .tmp 再 rename）
    void save(const std::vector<uint8_t>& data, int64_t ts = 0);

    // 弹出最旧的一帧（rename 为 .sending，崩溃不丢数据）
    std::optional<QueuedItem> pop();

    // 确认上传成功，删除 .sending 文件
    void confirm(const QueuedItem& item);

    // 当前队列大小（不含 .sending）
    size_t size() const;

    void cleanup();

private:
    void recover_sending();  // 启动时回收 .sending 文件
    std::string dir_;
    int max_mb_;
};
