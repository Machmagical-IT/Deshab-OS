// INSTALL/installer/dl/dl.go —— 安装器内置的静态下载/解析工具
//
// 为什么不用 busybox wget：busybox 1.35 的内建 TLS 明确
// “TLS certificate validation not implemented”，与 GitHub 握手会被重置。
// Go 的 crypto/tls 是静态编译的完整实现，产物 ~6MB，放进 initramfs 无压力。
//
// 子命令：
//   dl list <owner/repo> <latest|tag>
//       输出两段：首行 "TAG <tag>"，随后每行一个镜像资产 URL（分卷按名排序）
//   dl get <url> <outfile>
//       下载到 outfile（.part 后缀表示未完成，成功后 rename），失败自动重试
//
// 证书：从 $SSL_CERT_FILE 或 /etc/ssl/certs/ca-certificates.crt 读取。
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

const userAgent = "deshab-installer/1.0"

func client() *http.Client {
	return &http.Client{
		Timeout: 0, // 大文件下载需要长时间
		Transport: &http.Transport{
			Proxy:                 http.ProxyFromEnvironment,
			ResponseHeaderTimeout: 60 * time.Second,
			IdleConnTimeout:       90 * time.Second,
		},
	}
}

type asset struct {
	Name string `json:"name"`
	URL  string `json:"browser_download_url"`
}

type release struct {
	TagName string  `json:"tag_name"`
	Assets  []asset `json:"assets"`
}

func die(format string, a ...any) {
	fmt.Fprintf(os.Stderr, "dl: "+format+"\n", a...)
	os.Exit(1)
}

func apiURL(repo, tag string) string {
	base := "https://api.github.com/repos/" + repo + "/releases"
	if tag == "" || tag == "latest" {
		return base + "/latest"
	}
	return base + "/tags/" + tag
}

func httpGet(url string) (*http.Response, error) {
	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", userAgent)
	req.Header.Set("Accept", "application/vnd.github+json")
	return client().Do(req)
}

func cmdList(repo, tag string) {
	var lastErr error
	for attempt := 1; attempt <= 5; attempt++ {
		resp, err := httpGet(apiURL(repo, tag))
		if err != nil {
			lastErr = err
			time.Sleep(3 * time.Second)
			continue
		}
		body, err := io.ReadAll(resp.Body)
		resp.Body.Close()
		if err != nil {
			lastErr = err
			continue
		}
		if resp.StatusCode != 200 {
			lastErr = fmt.Errorf("HTTP %d: %s", resp.StatusCode, strings.TrimSpace(string(body)))
			time.Sleep(3 * time.Second)
			continue
		}
		var rel release
		if err := json.Unmarshal(body, &rel); err != nil {
			die("解析 release JSON 失败: %v", err)
		}
		var parts, single []string
		for _, a := range rel.Assets {
			switch {
			case strings.Contains(a.Name, "deshab-release.img.gz.part"):
				parts = append(parts, a.URL)
			case strings.HasSuffix(a.Name, "deshab-release.img.gz"):
				single = append(single, a.URL)
			}
		}
		sort.Strings(parts)
		fmt.Printf("TAG %s\n", rel.TagName)
		if len(parts) > 0 {
			for _, u := range parts {
				fmt.Println(u)
			}
			return
		}
		if len(single) > 0 {
			fmt.Println(single[0])
			return
		}
		die("release %s 中没有 deshab-release.img.gz 资产", rel.TagName)
	}
	die("访问 GitHub API 失败: %v", lastErr)
}

func cmdGet(url, out string) {
	tmp := out + ".part"
	var lastErr error
	for attempt := 1; attempt <= 8; attempt++ {
		lastErr = download(url, tmp)
		if lastErr == nil {
			if err := os.Rename(tmp, out); err != nil {
				die("重命名 %s 失败: %v", tmp, err)
			}
			return
		}
		fmt.Fprintf(os.Stderr, "dl: 第 %d 次下载失败: %v\n", attempt, lastErr)
		time.Sleep(5 * time.Second)
	}
	die("下载失败: %v", lastErr)
}

func download(url, path string) error {
	resp, err := httpGet(url)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()

	h := sha256.New()
	written, err := io.Copy(io.MultiWriter(f, h), resp.Body)
	if err != nil {
		return err
	}
	if resp.ContentLength > 0 && written != resp.ContentLength {
		return fmt.Errorf("长度不符: 收到 %d / 期望 %d", written, resp.ContentLength)
	}
	if err := f.Sync(); err != nil {
		return err
	}
	// 追加写 sha256 到同目录，供 shell 侧比对（失败不影响主流程）
	_ = os.WriteFile(filepath.Clean(path)+".sha256", []byte(hex.EncodeToString(h.Sum(nil))+"\n"), 0644)
	fmt.Fprintf(os.Stderr, "dl: %s 完成 (%d 字节)\n", filepath.Base(path), written)
	return nil
}

func main() {
	if len(os.Args) < 2 {
		die("用法: dl list <repo> <tag> | dl get <url> <outfile>")
	}
	switch os.Args[1] {
	case "list":
		if len(os.Args) != 4 {
			die("用法: dl list <owner/repo> <latest|tag>")
		}
		cmdList(os.Args[2], os.Args[3])
	case "get":
		if len(os.Args) != 4 {
			die("用法: dl get <url> <outfile>")
		}
		cmdGet(os.Args[2], os.Args[3])
	default:
		die("未知子命令: %s", os.Args[1])
	}
}
