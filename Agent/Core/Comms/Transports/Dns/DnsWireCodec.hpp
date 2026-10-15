/**
 * @file DnsWireCodec.hpp
 * @brief DNS 通道的线格式编解码
 * @details
 * 抽出动机: 传输类里混着"线格式"与"会话/调度"两类关注点, 使 的 Health 判定与
 * 的积压语义难以单独改动。本文件只负责**字节 ↔ 语义**的映射, 无状态、无 IO。
 *
 * 线格式:
 *   * 查询: 头(12B) + qname + qtype=TXT(0x0010) + qclass=IN(0x0001)
 *   * qname: `<sid:08x>[-<seq:04x>-<idx>-<total>].<base32(分片载荷)按 63 字符分标签>.<domain>`
 *   * 应答: TXT 的 **RDATA 直接就是原始分片**(不是 base32 —— base32 只用于上行 qname)
 *   * 分片头 7B: [magic=0xA7][2B seq 小端][1B 片序][1B 总片数][2B 片长 小端](见 DatagramCodec)
 *
 * (记录每次查询的首段元数据与 idx/total), 本文件改动后必须回放一致。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cleat {
namespace dns {

    /// 上行分片数据量(编码后 ≤3 个 63 字符标签, 域名总长 ≤253)
    constexpr size_t kMaxFragPayload = 110;

    /// RFC4648 base32(无 padding, 小写输出 —— DNS 大小写不敏感)
    std::string Base32Encode(const std::vector<uint8_t>& data);

    /// qname 编码: "a.b.c" → 标签序列(含结尾 0)
    std::vector<uint8_t> EncodeQname(const std::string& name);

    /// 跳过 qname(标签或压缩指针), 返回新偏移; 失败返回原偏移
    size_t SkipName(const std::vector<uint8_t>& buf, size_t off);

    /**
     * @brief 构造 qname
     * @param frag       上行分片(空 = 探活/空闲轮询, qname 只含 sid)
     * @param domain     查询域
     * @param session_id 会话 id(由调用方提供 —— 注意这是**会话耦合点**: 会话身份
     */
    std::string BuildQname(const std::vector<uint8_t>& frag,
        const std::string& domain, uint64_t session_id);

    /// 构造查询报文
    std::vector<uint8_t> BuildQuery(uint16_t id, const std::string& qname);

    /**
     * @brief 解析 TXT 应答, 取出原始分片
     * @param resp  应答报文
     * @param query 对应查询(用于校验 ID 与跳过 question)
     * @param[out] txt RDATA 拼接结果(= 下行分片; 空 = 无下行数据)
     * @return 报文合法返回 true
     * @details 校验: ID 匹配 + QR=1 + RCODE=0; 遍历 answer 取 type=16 的 RDATA 段
     */
    bool ParseTxtResponse(const std::vector<uint8_t>& resp,
        const std::vector<uint8_t>& query, std::vector<uint8_t>& txt);

} // namespace dns
} // namespace cleat
