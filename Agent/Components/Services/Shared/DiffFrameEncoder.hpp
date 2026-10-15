#pragma once
#include <cstdint>
#include <vector>
#include "Common/ThirdParty/json.hpp"
#include <memory>

namespace cleat {

    // BoundingBox 定义在 cleat 命名空间
    struct BoundingBox {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
    };

    // 差异算法枚举
    enum class DiffAlgorithm {
        BoundingBox,   // 包围盒
        BlockBased     // 块列表
    };

    class DiffFrameEncoder {
    public:
        struct Config {
            int block_size = 64;                // 块大小（像素）
            int full_frame_interval = 30;       // 每多少帧强制发送一次全帧
            float diff_threshold = 0.01f;       // 块变化阈值（像素差异百分比）
            float degrade_ratio = 0.5f;         // 变化块超过此比例则降级为全帧
            DiffAlgorithm algorithm = DiffAlgorithm::BoundingBox;
        };

        struct DiffBlock {
            int x{};          // 块索引（列）
            int y{};          // 块索引（行）
            int w{};          // 实际块宽（可能小于 block_size，用于边缘）
            int h{};          // 实际块高
            std::vector<uint8_t> data;  // RGB 数据（大小 = w * h * 3）
        };

        struct DiffResult {
            bool is_full = false;               // 是否应发送全帧
            int total_blocks = 0;               // 总块数
            int changed_blocks = 0;             // 变化块数
            float change_ratio = 0.0f;          // 变化比例
            std::vector<DiffBlock> blocks;      // 变化的块
            int full_width = 0;
            int full_height = 0;
            int block_size = 0;
        };

        DiffFrameEncoder();
        explicit DiffFrameEncoder(const Config& config);
        ~DiffFrameEncoder() = default;

        // 设置图像尺寸（在首次捕获时调用）
        void SetFrameSize(int width, int height);

        // 编码差异帧
        // 返回 DiffResult，包含是否全帧、变化块列表等
        DiffResult Encode(const std::vector<uint8_t>& current_frame);

        // 强制下一帧为全帧
        void ForceFullFrame();

        // 重置编码器状态（清除参考帧）
        void Reset();

        // 计算包围盒（基于 DiffResult）
        BoundingBox ComputeBoundingBox(const DiffResult& result) const;

        // 从帧中提取包围盒区域（用于包围盒算法）
        std::vector<uint8_t> ExtractRegion(const std::vector<uint8_t>& frame, const BoundingBox& bb) const;

        // 获取当前配置
        const Config& GetConfig() const { return config_; }

    private:
        Config config_;
        int width_ = 0;
        int height_ = 0;
        int cols_ = 0;
        int rows_ = 0;
        int block_size_ = 0;

        std::vector<uint8_t> prev_frame_;   // 前一帧数据（RGB24）
        int frame_counter_ = 0;

        uint64_t computeBlockHash(const uint8_t* data, int stride, int x, int y, int w, int h) const;
        void extractBlock(const uint8_t* data, int stride, int x, int y, int w, int h, std::vector<uint8_t>& out) const;
    };

} // namespace cleat