#include "DiffFrameEncoder.hpp"
#include <algorithm>
#include <cstring>
#include <cmath>

namespace cleat {

    DiffFrameEncoder::DiffFrameEncoder() : DiffFrameEncoder(Config{}) {}

    DiffFrameEncoder::DiffFrameEncoder(const Config& config) : config_(config) {
        block_size_ = config.block_size;
    }

    void DiffFrameEncoder::SetFrameSize(int width, int height) {
        if (width == width_ && height == height_) return;
        width_ = width;
        height_ = height;
        block_size_ = config_.block_size;
        cols_ = (width + block_size_ - 1) / block_size_;
        rows_ = (height + block_size_ - 1) / block_size_;
        // 确保至少 1 行 1 列
        if (cols_ == 0) cols_ = 1;
        if (rows_ == 0) rows_ = 1;
        prev_frame_.clear();
        frame_counter_ = 0;
    }

    void DiffFrameEncoder::ForceFullFrame() {
        frame_counter_ = config_.full_frame_interval; // 强制下一帧全帧
        prev_frame_.clear();  //防止关闭再打开之后误判
    }

    void DiffFrameEncoder::Reset() {
        prev_frame_.clear();
        frame_counter_ = 0;
    }

    uint64_t DiffFrameEncoder::computeBlockHash(const uint8_t* data, int stride, int x, int y, int w, int h) const {
        uint64_t hash = 0;
        for (int row = y; row < y + h; ++row) {
            const uint8_t* row_ptr = data + row * stride + x * 3;
            for (int col = 0; col < w; ++col) {
                hash ^= (uint64_t)row_ptr[0] << 0;
                hash ^= (uint64_t)row_ptr[1] << 8;
                hash ^= (uint64_t)row_ptr[2] << 16;
                hash = (hash << 1) | (hash >> 63);
                row_ptr += 3;
            }
        }
        return hash;
    }

    void DiffFrameEncoder::extractBlock(const uint8_t* data, int stride, int x, int y, int w, int h, std::vector<uint8_t>& out) const {
        out.resize(static_cast<std::vector<uint8_t, std::allocator<uint8_t>>::size_type>(w) * h * 3);
        for (int row = y; row < y + h; ++row) {
            const uint8_t* src = data + row * stride + x * 3;
            uint8_t* dst = out.data() + (row - y) * w * 3;
            memcpy(dst, src, static_cast<size_t>(w) * 3);
        }
    }

    DiffFrameEncoder::DiffResult DiffFrameEncoder::Encode(const std::vector<uint8_t>& current_frame) {
        DiffResult result;
        if (width_ == 0 || height_ == 0) {
            return result;
        }

        int expected_size = width_ * height_ * 3;
        if (current_frame.size() != expected_size) {
            result.is_full = true;
            return result;
        }

        ++frame_counter_;

        // 首次或强制全帧
        if (prev_frame_.empty() || frame_counter_ >= config_.full_frame_interval) {
            frame_counter_ = 0;
            prev_frame_ = current_frame;
            result.is_full = true;
            result.full_width = width_;
            result.full_height = height_;
            result.block_size = block_size_;
            return result;
        }

        int stride = width_ * 3;
        const uint8_t* cur_data = current_frame.data();
        const uint8_t* prev_data = prev_frame_.data();

        int changed_count = 0;
        int total_blocks = cols_ * rows_;
        result.blocks.reserve(total_blocks);

        for (int row = 0; row < rows_; ++row) {
            int y = row * block_size_;
            int h = std::min(block_size_, height_ - y);
            for (int col = 0; col < cols_; ++col) {
                int x = col * block_size_;
                int w = std::min(block_size_, width_ - x);

                uint64_t hash_cur = computeBlockHash(cur_data, stride, x, y, w, h);
                uint64_t hash_prev = computeBlockHash(prev_data, stride, x, y, w, h);

                if (hash_cur != hash_prev) {
                    changed_count++;
                    DiffBlock block;
                    block.x = col;
                    block.y = row;
                    block.w = w;
                    block.h = h;
                    extractBlock(cur_data, stride, x, y, w, h, block.data);
                    result.blocks.push_back(std::move(block));
                }
            }
        }

        result.changed_blocks = changed_count;
        result.total_blocks = total_blocks;
        result.change_ratio = (float)changed_count / total_blocks;

        // 如果变化比例超过阈值，降级为全帧
        if (result.change_ratio > config_.degrade_ratio) {
            prev_frame_ = current_frame;
            frame_counter_ = 0;
            result.is_full = true;
            result.full_width = width_;
            result.full_height = height_;
            result.block_size = block_size_;
            result.blocks.clear();
            return result;
        }

        result.is_full = false;
        result.full_width = width_;
        result.full_height = height_;
        result.block_size = block_size_;
        prev_frame_ = current_frame; // 更新参考帧
        return result;
    }

    BoundingBox DiffFrameEncoder::ComputeBoundingBox(const DiffResult& result) const {
        if (result.blocks.empty() || width_ == 0 || height_ == 0) {
            return { 0, 0, 0, 0 };
        }

        int minX = 999999, maxX = -1, minY = 999999, maxY = -1;
        for (const auto& block : result.blocks) {
            minX = std::min(minX, block.x);
            maxX = std::max(maxX, block.x);
            minY = std::min(minY, block.y);
            maxY = std::max(maxY, block.y);
        }

        BoundingBox bb;
        bb.x = minX * block_size_;
        bb.y = minY * block_size_;
        bb.w = (maxX - minX + 1) * block_size_;
        bb.h = (maxY - minY + 1) * block_size_;

        // 修正边界（不能超出图像边界）
        if (bb.x + bb.w > width_) bb.w = width_ - bb.x;
        if (bb.y + bb.h > height_) bb.h = height_ - bb.y;
        return bb;
    }

    std::vector<uint8_t> DiffFrameEncoder::ExtractRegion(const std::vector<uint8_t>& frame, const BoundingBox& bb) const {
        std::vector<uint8_t> region;
        if (bb.w <= 0 || bb.h <= 0 || frame.size() != (size_t)width_ * height_ * 3) {
            return region;
        }

        region.resize(static_cast<std::vector<uint8_t, std::allocator<uint8_t>>::size_type>(bb.w) * bb.h * 3);
        int stride = width_ * 3;
        for (int y = 0; y < bb.h; ++y) {
            const uint8_t* src = frame.data() + (bb.y + y) * stride + bb.x * 3;
            uint8_t* dst = region.data() + y * bb.w * 3;
            memcpy(dst, src, static_cast<size_t>(bb.w) * 3);
        }
        return region;
    }

} // namespace cleat