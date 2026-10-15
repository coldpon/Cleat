/**
 * @file SecretVault.hpp
 * @brief 运行时保险箱(敏感数据保护 Phase C): 进程内小秘密的密文驻留
 * @details
 * 以进程随机 32B 主密钥对字节块做 XChaCha20-Poly1305 密封(Seal)/开启(Open)。
 * 主密钥自身拆成两个 XOR 半片驻留(任一半片单独不可用, 组合需临时还原到栈,
 * 用后立即擦除) —— 目标是把"内存扫描一键还原全部秘密"抬升为"需理解本方案并
 * 定位两个半片", 并让 AEAD 完整性保护配置数据不被篡改。
 *
 * 定位与边界:
 * 这不是对已完全控机的强对抗, 属于抬高内存取证/自动化扫描成本的纵深层。
 * 线程安全: 内部互斥, Seal/Open 可并发调用。
 */
#pragma once

#include <cstddef>
#include <mutex>
#include <vector>

namespace cleat {

    class SecretVault {
    public:
        static SecretVault& Instance();

        /// 生成主密钥并驻留(半片拆分 + 防换页 best-effort); 幂等
        bool Init();

        /// 密封: 输出 = nonce(24B) || ciphertext(明文+16B tag); 失败返回空 vector
        std::vector<uint8_t> Seal(const uint8_t* p, size_t n);

        /// 开启: 失败(未初始化/损坏/篡改)返回 false 且 out 置空
        bool Open(const std::vector<uint8_t>& sealed, std::vector<uint8_t>& out) const;

        /// 擦除任意缓冲(sodium_memzero + 释放)
        static void Wipe(std::vector<uint8_t>& b);

    private:
        SecretVault() = default;
        ~SecretVault();
        SecretVault(const SecretVault&) = delete;
        SecretVault& operator=(const SecretVault&) = delete;

        /// 栈上临时还原主密钥; 调用方用完必须 sodium_memzero
        bool UnsealKeyToStack(uint8_t key32[32]) const;

        mutable std::mutex mu_;
        bool init_ = false;
        std::vector<uint8_t> mk_a_;   ///< 主密钥半片 A(驻留; B XOR A = 主密钥)
        std::vector<uint8_t> mk_b_;   ///< 主密钥半片 B
    };

} // namespace cleat
