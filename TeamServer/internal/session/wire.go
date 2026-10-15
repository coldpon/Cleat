package session

import (
	"bytes"
	"encoding/json"
	"fmt"
)

// 明文会话帧的拆/装权威(帧形状归一化的单点实现)。
//
// 形状(协议):
//
//	混合帧:  JSON头 + '\0' + 二进制体   —— 流数据/文件分块/截图等大载荷
//	纯 JSON: JSON头(无分隔符、无体)     —— 命令/响应/确认等小帧
//
// 两种形状共享同一概念模型: **纯 JSON 即空体的混合帧**。判据是"找第一个 '\0'":
// 合法 JSON 文本的字节流不可能含裸 NUL(JSON 规范强制转义, encoding/json 与
// Agent 侧 nlohmann 同), 故分隔符的存在性是确定性判定, 不是 try-parse 失败后
// 的启发式兜底。找不到分隔符 = 空体, 找到 = 分割点, 没有第三种形状。
//
// **本文件是全仓唯一的拆/装权威**: 任何解析点不得再对原始帧自行 json.Unmarshal
// 或手拼 '\0' —— 裸 JSON 帧直接 Unmarshal 会碰巧成功, 混合帧才炸, 且炸点远离
// 病灶(#99→#113 的匿名注册风暴即此模式的实锤)。下游一律经 SplitFrame 拿
// (header, body) 结构化结果; 带体的出站帧一律经 BuildFrame 构造。

// SplitFrame 把明文帧拆为 JSON 头与二进制体。
//
// header 恒为合法 JSON(分隔符前的完整字节; 无分隔符时即整帧); body 为 nil 表示
// 无二进制体(纯 JSON 帧)。头里声明了 body_size 时恒校验, 与实际体长不符即错 ——
// 此前 Agent 侧 HybridFrameBuilder::Parse 校验而 handleOne 不校验, 同一契约两种
// 执行并存, 在此收口。
func SplitFrame(raw []byte) (header json.RawMessage, body []byte, err error) {
	head, bin := raw, []byte(nil)
	if i := bytes.IndexByte(raw, 0); i >= 0 {
		head, bin = raw[:i], raw[i+1:]
	}
	var probe struct {
		BodySize *int64 `json:"body_size"`
	}
	if err := json.Unmarshal(head, &probe); err != nil {
		return nil, nil, fmt.Errorf("帧头不是合法 JSON: %w", err)
	}
	if probe.BodySize != nil && *probe.BodySize != int64(len(bin)) {
		return nil, nil, fmt.Errorf("body_size 不符: 头声明 %d, 实际 %d", *probe.BodySize, len(bin))
	}
	return json.RawMessage(head), bin, nil
}

// BuildFrame 构造出站帧: body 为空 → 纯 JSON(无分隔符, 与 json.Marshal(header)
// 等价); 非空 → JSON头 + '\0' + 体, 且 body_size **恒以实际体长覆盖写入** ——
// 头里手写的任何值都不可信(SendHybrid 补了、SendHybridPush 漏了的历史漂移,
// 从构造端根除)。
func BuildFrame(header []byte, body []byte) ([]byte, error) {
	if len(body) == 0 {
		if !json.Valid(header) {
			return nil, fmt.Errorf("帧头不是合法 JSON")
		}
		// 空体: 语义上是空体混合帧, 物理上省掉分隔符(线上形状不变)
		return header, nil
	}
	var head map[string]json.RawMessage
	if err := json.Unmarshal(header, &head); err != nil {
		return nil, fmt.Errorf("帧头不是合法 JSON: %w", err)
	}
	head["body_size"], _ = json.Marshal(len(body))
	fixed, err := json.Marshal(head)
	if err != nil {
		return nil, err
	}
	out := make([]byte, 0, len(fixed)+1+len(body))
	out = append(out, fixed...)
	out = append(out, 0)
	out = append(out, body...)
	return out, nil
}

// decodeResponse 把响应帧解码为响应 map。混合帧响应(如截图 JPEG)的二进制体
// 以 "__binary__" 键挂进响应顶层(与 handlers.go 的推送二进制同键惯例),
// 上层(Service.Command)据此提升为 data.image —— 纯 JSON 响应路径完全不变。
func decodeResponse(respData []byte) (map[string]interface{}, error) {
	header, bin, err := SplitFrame(respData)
	if err != nil {
		return nil, fmt.Errorf("响应帧解析失败: %w", err)
	}
	var resp map[string]interface{}
	if err := json.Unmarshal(header, &resp); err != nil {
		return nil, fmt.Errorf("响应头不是 JSON 对象: %w", err)
	}
	if len(bin) > 0 {
		resp["__binary__"] = bin
	}
	return resp, nil
}
