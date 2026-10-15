/**
 * @file DnsWireCodec.cpp
 * @brief DNS 线格式编解码实现
 */

#include "Core/Comms/Transports/Dns/DnsWireCodec.hpp"

#include <cstdio>

#include "Core/Comms/Transports/Internal/DatagramCodec.hpp"

namespace cleat {
namespace dns {

    std::string Base32Encode(const std::vector<uint8_t>& data) {
        static const char* kAlphabet = "abcdefghijklmnopqrstuvwxyz234567";
        std::string out;
        out.reserve((data.size() * 8 + 4) / 5);
        uint32_t buffer = 0;
        int bits = 0;
        for (uint8_t b : data) {
            buffer = (buffer << 8) | b;
            bits += 8;
            while (bits >= 5) {
                out.push_back(kAlphabet[(buffer >> (bits - 5)) & 0x1F]);
                bits -= 5;
            }
        }
        if (bits > 0) {
            out.push_back(kAlphabet[(buffer << (5 - bits)) & 0x1F]);
        }
        return out;
    }

    std::vector<uint8_t> EncodeQname(const std::string& name) {
        std::vector<uint8_t> out;
        size_t i = 0;
        while (i < name.size()) {
            const size_t dot = name.find('.', i);
            const size_t len = (dot == std::string::npos) ? name.size() - i : dot - i;
            out.push_back(static_cast<uint8_t>(len));
            out.insert(out.end(), name.begin() + i, name.begin() + i + len);
            i += len + 1;
        }
        out.push_back(0);
        return out;
    }

    size_t SkipName(const std::vector<uint8_t>& buf, size_t off) {
        while (off < buf.size()) {
            const uint8_t len = buf[off];
            if (len == 0) {
                return off + 1;
            }
            if ((len & 0xC0) == 0xC0) {   // 压缩指针
                return off + 2;
            }
            off += 1 + len;
        }
        return off;
    }

    std::string BuildQname(const std::vector<uint8_t>& frag,
        const std::string& domain, uint64_t session_id) {
        // meta = <sid-hex>-<seq-hex>-<fragidx>-<fragtotal>
        char meta[64]{};
        uint16_t seq = 0;
        uint8_t idx = 0, total = 0;
        if (!frag.empty() && DatagramCodec::ParseHeader(frag, seq, idx, total)) {
            std::snprintf(meta, sizeof(meta), "%08llx-%04x-%u-%u",
                static_cast<unsigned long long>(session_id), seq, idx, total);
        }
        else {
            std::snprintf(meta, sizeof(meta), "%08llx",
                static_cast<unsigned long long>(session_id));
        }

        std::string qname = meta;
        if (!frag.empty()) {
            const std::string b32 = Base32Encode(
                std::vector<uint8_t>(frag.begin() + DatagramCodec::kHeaderSize, frag.end()));
            // 每 63 字符一个标签
            for (size_t i = 0; i < b32.size(); i += 63) {
                qname += '.';
                qname += b32.substr(i, 63);
            }
        }
        qname += '.';
        qname += domain;
        return qname;
    }

    std::vector<uint8_t> BuildQuery(uint16_t id, const std::string& qname) {
        std::vector<uint8_t> out;
        // 头: ID | flags(0x0100 标准查询 RD) | QD=1 | AN=NS=AR=0
        out.push_back(static_cast<uint8_t>(id >> 8));
        out.push_back(static_cast<uint8_t>(id & 0xFF));
        out.push_back(0x01);
        out.push_back(0x00);
        out.push_back(0x00);
        out.push_back(0x01);
        out.push_back(0x00);
        out.push_back(0x00);
        out.push_back(0x00);
        out.push_back(0x00);
        out.push_back(0x00);
        out.push_back(0x00);
        // 问题: qname | qtype=TXT(16) | qclass=IN(1)
        const auto qn = EncodeQname(qname);
        out.insert(out.end(), qn.begin(), qn.end());
        out.push_back(0x00);
        out.push_back(0x10);
        out.push_back(0x00);
        out.push_back(0x01);
        return out;
    }

    bool ParseTxtResponse(const std::vector<uint8_t>& resp,
        const std::vector<uint8_t>& query, std::vector<uint8_t>& txt) {
        if (resp.size() < 12) {
            return false;
        }
        // ID 匹配 + QR=1 + RCODE=0
        if (resp[0] != query[0] || resp[1] != query[1]) {
            return false;
        }
        if ((resp[2] & 0x80) == 0 || (resp[3] & 0x0F) != 0) {
            return false;
        }
        const uint16_t ancount = static_cast<uint16_t>(resp[6] << 8) | resp[7];

        // 跳过 question
        size_t off = 12;
        off = SkipName(resp, off);
        if (off + 4 > resp.size()) {
            return false;
        }
        off += 4;   // qtype + qclass

        txt.clear();
        for (uint16_t i = 0; i < ancount; ++i) {
            if (off >= resp.size()) {
                return false;
            }
            off = SkipName(resp, off);
            if (off + 10 > resp.size()) {
                return false;
            }
            const uint16_t type = static_cast<uint16_t>(resp[off] << 8) | resp[off + 1];
            const uint16_t rdlen = static_cast<uint16_t>(resp[off + 8] << 8) | resp[off + 9];
            off += 10;
            if (off + rdlen > resp.size()) {
                return false;
            }
            if (type == 16) {   // TXT: [1B len][data] 多段
                size_t pos = off;
                const size_t end = off + rdlen;
                while (pos < end) {
                    const uint8_t seg_len = resp[pos++];
                    if (pos + seg_len > end) {
                        return false;
                    }
                    txt.insert(txt.end(), resp.begin() + pos, resp.begin() + pos + seg_len);
                    pos += seg_len;
                }
            }
            off += rdlen;
        }
        return true;
    }

} // namespace dns
} // namespace cleat
