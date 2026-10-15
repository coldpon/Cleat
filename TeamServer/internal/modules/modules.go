// Package modules 模块仓库: 文件系统存储, DLL 上传/列表/删除/读取。
// 存储目录: data/modules/<module_id>.dll
// module_id: [A-Za-z0-9_-], 长度 ≤64, 防路径穿越。
package modules

import (
	"crypto/sha256"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
)

// **严禁**再按 err.Error() 文案匹配 —— modules 包改一个标点, 上游静默失效的事故不再发生。
// 包装格式 `%w: %s` 保证消息原文("模块已存在: name")逐字不变。
var (
	ErrModuleExists   = errors.New("模块已存在")
	ErrModuleNotFound = errors.New("模块不存在")
)

const (
	MaxModuleSize = 32 * 1024 * 1024 // 32MB
	MaxNameLen    = 64
	DirName       = "modules"
)

var nameRe = regexp.MustCompile(`^[A-Za-z0-9_-]+$`)

// ModuleInfo 模块元数据(文件名+大小+时间+SHA-256)。
type ModuleInfo struct {
	Name     string    `json:"name"`
	Size     int64     `json:"size"`
	SHA256   string    `json:"sha256"` // hex 编码的 SHA-256
	Modified time.Time `json:"modified"`
}

// Repo 模块仓库(文件系统)。
type Repo struct {
	dir string
}

// New 创建模块仓库, 自动创建目录。
func New(dataDir string) (*Repo, error) {
	dir := filepath.Join(dataDir, DirName)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, fmt.Errorf("创建模块目录失败: %w", err)
	}
	return &Repo{dir: dir}, nil
}

// ValidateName 校验 module_id 合法性。
func ValidateName(name string) error {
	if name == "" {
		return fmt.Errorf("模块名不能为空")
	}
	if len(name) > MaxNameLen {
		return fmt.Errorf("模块名过长(最大 %d 字符)", MaxNameLen)
	}
	if !nameRe.MatchString(name) {
		return fmt.Errorf("模块名只能包含字母/数字/下划线/连字符")
	}
	// 防路径穿越
	if strings.Contains(name, "..") || strings.Contains(name, "/") || strings.Contains(name, "\\") {
		return fmt.Errorf("模块名包含非法字符")
	}
	return nil
}

// sha256Hex 计算文件的 SHA-256 hex 字符串。
func sha256Hex(data []byte) string {
	h := sha256.Sum256(data)
	return fmt.Sprintf("%x", h[:])
}

// List 列出所有模块。
func (r *Repo) List() ([]ModuleInfo, error) {
	entries, err := os.ReadDir(r.dir)
	if err != nil {
		return nil, err
	}
	var out []ModuleInfo
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		name := e.Name()
		// 只返回 .dll 文件
		if !strings.HasSuffix(strings.ToLower(name), ".dll") {
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		hash := ""
		data, err := os.ReadFile(filepath.Join(r.dir, name))
		if err == nil {
			hash = sha256Hex(data)
		}
		out = append(out, ModuleInfo{
			Name:     strings.TrimSuffix(name, ".dll"),
			Size:     info.Size(),
			SHA256:   hash,
			Modified: info.ModTime().UTC(),
		})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out, nil
}

// Get 读取模块 DLL 字节。
func (r *Repo) Get(name string) ([]byte, error) {
	if err := ValidateName(name); err != nil {
		return nil, err
	}
	path := filepath.Join(r.dir, name+".dll")
	data, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, fmt.Errorf("%w: %s", ErrModuleNotFound, name)
		}
		return nil, err
	}
	return data, nil
}

// Info 获取单个模块元数据。
func (r *Repo) Info(name string) (*ModuleInfo, error) {
	if err := ValidateName(name); err != nil {
		return nil, err
	}
	path := filepath.Join(r.dir, name+".dll")
	fi, err := os.Stat(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, fmt.Errorf("%w: %s", ErrModuleNotFound, name)
		}
		return nil, err
	}
	hash := ""
	data, err := os.ReadFile(path)
	if err == nil {
		hash = sha256Hex(data)
	}
	return &ModuleInfo{
		Name:     name,
		Size:     fi.Size(),
		SHA256:   hash,
		Modified: fi.ModTime().UTC(),
	}, nil
}

// Save 保存模块 DLL 字节。已存在返回冲突错误。
func (r *Repo) Save(name string, data []byte) (*ModuleInfo, error) {
	if err := ValidateName(name); err != nil {
		return nil, err
	}
	if len(data) > MaxModuleSize {
		return nil, fmt.Errorf("模块过大(最大 %d MB)", MaxModuleSize/1024/1024)
	}
	if len(data) == 0 {
		return nil, fmt.Errorf("模块文件为空")
	}
	path := filepath.Join(r.dir, name+".dll")
	// 检查是否已存在
	if _, err := os.Stat(path); err == nil {
		return nil, fmt.Errorf("%w: %s", ErrModuleExists, name)
	}
	if err := os.WriteFile(path, data, 0o600); err != nil {
		return nil, err
	}
	fi, _ := os.Stat(path)
	return &ModuleInfo{
		Name:     name,
		Size:     fi.Size(),
		SHA256:   sha256Hex(data),
		Modified: fi.ModTime().UTC(),
	}, nil
}

// SaveFromReader 从 io.Reader 保存模块(支持流式上传)。
func (r *Repo) SaveFromReader(name string, reader io.Reader, maxSize int64) (*ModuleInfo, error) {
	if err := ValidateName(name); err != nil {
		return nil, err
	}
	path := filepath.Join(r.dir, name+".dll")
	if _, err := os.Stat(path); err == nil {
		return nil, fmt.Errorf("%w: %s", ErrModuleExists, name)
	}
	// 限制读取大小
	limited := io.LimitReader(reader, maxSize+1)
	data, err := io.ReadAll(limited)
	if err != nil {
		return nil, err
	}
	if int64(len(data)) > maxSize {
		return nil, fmt.Errorf("模块过大(最大 %d MB)", maxSize/1024/1024)
	}
	if len(data) == 0 {
		return nil, fmt.Errorf("模块文件为空")
	}
	if err := os.WriteFile(path, data, 0o600); err != nil {
		return nil, err
	}
	fi, _ := os.Stat(path)
	return &ModuleInfo{
		Name:     name,
		Size:     fi.Size(),
		SHA256:   sha256Hex(data),
		Modified: fi.ModTime().UTC(),
	}, nil
}

// Delete 删除模块。
func (r *Repo) Delete(name string) error {
	if err := ValidateName(name); err != nil {
		return err
	}
	path := filepath.Join(r.dir, name+".dll")
	if _, err := os.Stat(path); os.IsNotExist(err) {
		return fmt.Errorf("%w: %s", ErrModuleNotFound, name)
	}
	return os.Remove(path)
}
