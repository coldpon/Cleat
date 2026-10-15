/**
 * @file SecureWipe.hpp
 * @brief 敏感缓冲统一擦除口(敏感数据保护 Phase D)
 * @details
 * 所有含秘密的缓冲(模块 bin 体、凭据明文、密钥中间量)离开使用点即调用 ——
 * 目的: freed heap 不等于擦除, 内容在被复用前仍可被转储/扫描还原。
 * 实现走 libsodium sodium_memzero(防编译器把"写后释放"优化掉)。
 * 线程安全: 仅操作调用方拥有的缓冲, 无共享状态。
 */
#pragma once

#include <sodium.h>

#include <cstddef>
#include <string>
#include <vector>

namespace cleat {

    /// 原始缓冲擦除
    inline void SecureWipe(void* p, size_t n) {
        if (p && n != 0) {
            sodium_memzero(p, n);
        }
    }

    /// 字符串擦除(值清零 + 释放)
    inline void SecureWipe(std::string& s) {
        if (!s.empty()) {
            sodium_memzero(s.data(), s.size());
        }
        s.clear();
        s.shrink_to_fit();
    }

    /// 字节缓冲擦除(值清零 + 释放)
    inline void SecureWipe(std::vector<uint8_t>& b) {
        if (!b.empty()) {
            sodium_memzero(b.data(), b.size());
        }
        b.clear();
        b.shrink_to_fit();
    }

    // ---- const& 重载: 接口层传来的 const 引用, 底层对象为调用方拥有的
    // 非 const 缓冲(const_cast 合法) —— 只擦值, 不动容器本身(即将随作用域销毁)
    inline void SecureWipe(const std::vector<uint8_t>& b) {
        if (!b.empty()) {
            sodium_memzero(const_cast<uint8_t*>(b.data()), b.size());
        }
    }

    inline void SecureWipe(const std::string& s) {
        if (!s.empty()) {
            sodium_memzero(const_cast<char*>(s.data()), s.size());
        }
    }

} // namespace cleat
