"""
resolve_douyin_url.py - 解析抖音短链接 (v.douyin.com -> www.douyin.com/video/xxx)
用法: python resolve_douyin_url.py <short_url>
输出: 解析后的长链接（stdout 第一行），失败则空行 + 非零退出码
"""
import sys
import os
import time


def main():
    if len(sys.argv) < 2:
        print("", flush=True)
        sys.exit(1)
    url = sys.argv[1].strip()

    # 优先用 curl_cffi（快，无需浏览器）
    try:
        import curl_cffi.requests as reqs
        s = reqs.Session(impersonate='chrome131', allow_redirects=True, timeout=15)
        s.headers.update({
            'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36',
        })
        r = s.get(url)
        final = r.url
        if 'douyin.com/video/' in final or 'douyin.com/share/video/' in final:
            print(final, flush=True)
            sys.exit(0)
        loc = r.headers.get('Location', '')
        if loc and 'douyin.com' in loc:
            print(loc, flush=True)
            sys.exit(0)
    except Exception:
        pass

    # 回退：Playwright 连接已有浏览器
    exe_dir = os.path.dirname(os.path.abspath(__file__))
    script = os.path.join(exe_dir, 'playwright_extract.py')
    if os.path.isfile(script):
        try:
            import subprocess
            r = subprocess.run(
                [sys.executable, script, '--resolve-url', url],
                capture_output=True, text=True, timeout=30
            )
            out = (r.stdout or '').strip()
            if out and 'http' in out:
                print(out.split('\n')[0], flush=True)
                sys.exit(0)
        except Exception:
            pass

    # 最后回退：简单正则从短链接取 ID
    import re
    m = re.search(r'v\.douyin\.com/([A-Za-z0-9]+)/', url)
    if m:
        print(f"https://www.douyin.com/video/{m.group(1)}", flush=True)
        sys.exit(0)

    print("", flush=True)
    sys.exit(1)


if __name__ == '__main__':
    main()
