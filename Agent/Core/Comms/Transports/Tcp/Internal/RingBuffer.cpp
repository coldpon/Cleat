#include "RingBuffer.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace cleat {

	RingBuffer::RingBuffer(size_t initial_capacity, size_t max_capacity)
		: max_capacity_(max_capacity) {
		if (initial_capacity > max_capacity_) {
			initial_capacity = max_capacity_;
		}
		if (initial_capacity == 0) {
			initial_capacity = 1024; // 最小容量保底
		}
		buffer_.resize(initial_capacity);
		read_pos_ = 0;
		write_pos_ = 0;
		size_ = 0;
	}

	size_t RingBuffer::Write(const uint8_t* data, size_t len) {
		if (len == 0 || data == nullptr) return 0;

		std::unique_lock<std::mutex> lock(mutex_);

		// 1. 确保有足够空间（尝试 compact + 扩容）
		if (!expand_unlocked(len)) {
			// 即使无法扩容，也可能有部分剩余空间，写入尽可能多的数据
			size_t avail = writable_size_unlocked();
			if (avail == 0) return 0;
			len = std::min(len, avail);
			// 但 expand_unlocked 失败时可能还未 compact，但我们已经调用过了，这里不再重复
		}

		// 2. 写入数据（可能 len 已被缩减）
		size_t capacity = buffer_.size();
		size_t first_chunk = std::min(len, capacity - write_pos_);
		if (first_chunk > 0) {
			std::memcpy(buffer_.data() + write_pos_, data, first_chunk);
		}
		size_t second_chunk = len - first_chunk;
		if (second_chunk > 0) {
			// 环绕到头部
			std::memcpy(buffer_.data(), data + first_chunk, second_chunk);
			write_pos_ = second_chunk; // 写指针回绕后指向 second_chunk 位置
		}
		else {
			write_pos_ += first_chunk;
			if (write_pos_ == capacity) write_pos_ = 0; // 刚好写到末尾，回绕
		}
		size_ += len;
		return len;
	}

	size_t RingBuffer::Read(uint8_t* out, size_t len) {
		if (len == 0 || out == nullptr) return 0;

		std::unique_lock<std::mutex> lock(mutex_);
		size_t readable = readable_size_unlocked();
		if (readable == 0) return 0;

		size_t to_read = std::min(len, readable);
		size_t capacity = buffer_.size();

		// 从 read_pos_ 开始读取
		size_t first_chunk = std::min(to_read, capacity - read_pos_);
		if (first_chunk > 0) {
			std::memcpy(out, buffer_.data() + read_pos_, first_chunk);
		}
		size_t second_chunk = to_read - first_chunk;
		if (second_chunk > 0) {
			std::memcpy(out + first_chunk, buffer_.data(), second_chunk);
			read_pos_ = second_chunk; // 回绕后读指针指向 second_chunk
		}
		else {
			read_pos_ += first_chunk;
			if (read_pos_ == capacity) read_pos_ = 0;
		}
		size_ -= to_read;

		// 可选优化：如果读空了，可以重置指针到0，方便后续写入
		if (size_ == 0) {
			read_pos_ = 0;
			write_pos_ = 0;
		}

		return to_read;
	}

	size_t RingBuffer::Peek(uint8_t* out, size_t len) const {
		if (len == 0 || out == nullptr) return 0;

		std::unique_lock<std::mutex> lock(mutex_);
		size_t readable = readable_size_unlocked();
		if (readable == 0) return 0;

		size_t to_peek = std::min(len, readable);
		size_t capacity = buffer_.size();

		size_t first_chunk = std::min(to_peek, capacity - read_pos_);
		if (first_chunk > 0) {
			std::memcpy(out, buffer_.data() + read_pos_, first_chunk);
		}
		size_t second_chunk = to_peek - first_chunk;
		if (second_chunk > 0) {
			std::memcpy(out + first_chunk, buffer_.data(), second_chunk);
		}
		return to_peek;
	}

	size_t RingBuffer::ReadableSize() const {
		std::unique_lock<std::mutex> lock(mutex_);
		return readable_size_unlocked();
	}

	size_t RingBuffer::WritableSize() const {
		std::unique_lock<std::mutex> lock(mutex_);
		return writable_size_unlocked();
	}

	size_t RingBuffer::Capacity() const {
		std::unique_lock<std::mutex> lock(mutex_);
		return buffer_.size();
	}

	void RingBuffer::Clear() {
		std::unique_lock<std::mutex> lock(mutex_);
		read_pos_ = 0;
		write_pos_ = 0;
		size_ = 0;
		// 不清除实际内存，仅重置逻辑
	}

	// ---------- 私有辅助方法（调用者已持有锁） ----------

	size_t RingBuffer::readable_size_unlocked() const {
		return size_;
	}

	size_t RingBuffer::writable_size_unlocked() const {
		return buffer_.size() - size_;
	}

	bool RingBuffer::compact_unlocked() {
		if (size_ == 0) {
			read_pos_ = 0;
			write_pos_ = 0;
			return true;
		}
		if (read_pos_ == 0) {
			
			write_pos_ = size_;
			return true;
		}

		// 将未读数据从 read_pos_ 开始，长度为 size_，移到缓冲区开头
		std::vector<uint8_t> temp(size_);
		size_t capacity = buffer_.size();
		size_t first = std::min(size_, capacity - read_pos_);
		if (first > 0) {
			std::memcpy(temp.data(), buffer_.data() + read_pos_, first);
		}
		size_t second = size_ - first;
		if (second > 0) {
			std::memcpy(temp.data() + first, buffer_.data(), second);
		}
		// 复制回原缓冲区头部
		std::memcpy(buffer_.data(), temp.data(), size_);
		read_pos_ = 0;
		write_pos_ = size_;
		return true;
	}

	bool RingBuffer::expand_unlocked(size_t needed) {
		// 先紧凑，腾出连续空间
		compact_unlocked();

		size_t capacity = buffer_.size();
		size_t avail = capacity - size_; // 可用空间（已紧凑后，可用空间在尾部连续）
		if (avail >= needed) {
			return true; // 空间充足
		}

		// 需要扩容
		//size_t missing = needed - avail;
		size_t new_capacity = capacity;
		// 扩容策略：至少增加 missing，但按指数增长，但不超过 max_capacity_
		if (capacity == 0) {
			new_capacity = 1024; // 保底
		}
		while (new_capacity - size_ < needed && new_capacity < max_capacity_) {
			new_capacity = std::min(max_capacity_, new_capacity * 2);
			// 防止溢出
			if (new_capacity <= capacity) break;
		}
		if (new_capacity - size_ < needed) {
			// 无法扩容到足够空间
			return false;
		}

		// 执行扩容，注意之前已经 compact，数据在开头 size_ 个字节
		buffer_.resize(new_capacity);
		// 扩容后，数据仍在前 size_ 个字节，read_pos_ 保持 0，write_pos_ 保持 size_
		// 因为 compact 已经将 write_pos_ 设为 size_
		// 但为了安全，重新设置
		read_pos_ = 0;
		write_pos_ = size_;
		return true;
	}

} // namespace cleat