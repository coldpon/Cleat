#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace cleat {

    /**
     * @brief 线程安全的环形缓冲区（多生产者单消费者风格）
     *
     * - 预分配内存，支持动态扩容至硬上限（MAX_PACKET_SIZE = 50MB）。
     * - 所有公共方法均加锁，保证多线程写入和单线程读取时的数据一致性。
     * - 扩容前自动执行紧凑（compact）操作，将未读数据移到缓冲区头部，以最大化连续空间。
     * - 遵循 RAII，所有资源由 std::vector 管理。
     */
    class RingBuffer {
    public:
        /**
         * @param initial_capacity 初始容量（字节），建议设为常见包大小（如 1MB）
         * @param max_capacity     硬上限（字节），默认 50MB，不可超过此值
         */
        explicit RingBuffer(size_t initial_capacity = 1 * 1024 * 1024,
            size_t max_capacity = 50 * 1024 * 1024);

        // 禁止拷贝和移动（或可根据需要实现移动）
        RingBuffer(const RingBuffer&) = delete;
        RingBuffer& operator=(const RingBuffer&) = delete;
        RingBuffer(RingBuffer&&) = delete;
        RingBuffer& operator=(RingBuffer&&) = delete;

        ~RingBuffer() = default;

        /**
         * @brief 写入数据
         * @param data 源数据指针
         * @param len  要写入的字节数
         * @return 实际写入的字节数（可能小于 len，当剩余空间不足且无法扩容时）
         * @note 若剩余空间不足，会先尝试紧凑内部数据，再尝试扩容（不超过 max_capacity_）
         */
        size_t Write(const uint8_t* data, size_t len);

        /**
         * @brief 读取数据（消费）
         * @param out 输出缓冲区
         * @param len 期望读取的字节数
         * @return 实际读取的字节数（可能小于 len，当可读数据不足时）
         */
        size_t Read(uint8_t* out, size_t len);

        /**
         * @brief 查看数据（不消费）
         * @param out 输出缓冲区
         * @param len 期望查看的字节数
         * @return 实际查看的字节数
         */
        size_t Peek(uint8_t* out, size_t len) const;

        /**
         * @brief 当前可读数据大小（字节）
         */
        size_t ReadableSize() const;

        /**
         * @brief 当前剩余可写空间（字节）
         */
        size_t WritableSize() const;

        /**
         * @brief 当前缓冲区总容量（字节）
         */
        size_t Capacity() const;

        /**
         * @brief 清空缓冲区（重置读/写指针，数据丢弃）
         */
        void Clear();

    private:
        mutable std::mutex mutex_;           // 保护所有成员变量
        std::vector<uint8_t> buffer_;        // 实际存储
        size_t read_pos_ = 0;                // 读指针（逻辑索引）
        size_t write_pos_ = 0;               // 写指针（逻辑索引）
        size_t size_ = 0;                    // 当前有效数据字节数
        size_t max_capacity_;                // 硬上限

        // 以下辅助方法假定调用者已持有锁
        size_t readable_size_unlocked() const;
        size_t writable_size_unlocked() const;
        bool compact_unlocked();             // 将未读数据移到头部，返回是否成功（总是成功）
        bool expand_unlocked(size_t needed); // 扩容至至少 needed 字节，返回是否成功（不超上限）
    };

} // namespace cleat