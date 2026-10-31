"""
verify_cookie.py - 多端点验证抖音 Cookie 有效性（curl_cffi）

用法:
    python verify_cookie.py <cookies_netscape.txt>

输出:
    OK      退出码 0（至少一个端点返回正常）
    FAIL    退出码 1（全部失败）
"""
import sys
import os


def load_cookies(path: str) -> dict:
    cookies = {}
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('#') or '\t' not in line:
                continue
            parts = line.split('\t')
            if len(parts) >= 7:
                name, value = parts[5], parts[6]
                # 过滤无效值：空串、纯二进制、过短
                if not value or len(value) < 2:
                    continue
                # 过滤含有不可打印字符的值
                if any(ord(c) < 0x20 or ord(c) > 0x7e for c in value if ord(c) < 0x10000):
                    continue
                cookies[name] = value
    return cookies


def do_verify(cookies: dict) -> bool:
    if not cookies:
        print("FAIL (no valid cookies)", flush=True)
        return False
    try:
        import curl_cffi.requests as reqs
    except Exception:
        print("FAIL (no curl_cffi)", flush=True)
        return False

    # 检查是否有关键 Cookie（sessionid / ttwid / uid_tt）
    key_names = {'sessionid', 'session_id', 'ttwid', 'uid_tt', 'msToken', '__ac_signature'}
    has_key = bool(key_names & set(cookies.keys()))
    if not has_key:
        print(f"FAIL (missing key cookies, got {len(cookies)} non-key cookies)", flush=True)
        return False

    # 多组 UA + 多端点，避免单一请求被拦截
    endpoints = [
        ("chrome120", "https://www.douyin.com/aweme/v1/web/aweme/detail/?aweme_id=7663486386041191551&aid=6383"),
        ("chrome124", "https://www.douyin.com/aweme/v1/web/homefeed/feed/?category=newest&count=3&as=AAAA&cp=BBBB&msToken=CCCC"),
        ("chrome131", "https://www.douyin.com/aweme/v1/web/user/reader/?user_id=1&offset=0&count=1&sub_channel=0&device_platform=windows&aid=6383"),
    ]
    headers_override = {
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36",
        "Referer": "https://www.douyin.com/",
        "Accept-Language": "zh-CN,zh;q=0.9",
    }

    for impersonate, url in endpoints:
        try:
            s = reqs.Session(impersonate=impersonate)
            s.cookies.update(cookies)
            r = s.get(url, timeout=15, headers=headers_override, allow_redirects=True)
            text = r.text
            if r.status_code == 200 and ('aweme_detail' in text or 'homefeed' in text or 'user' in text or 'status_code' in text):
                print(f"OK  ({impersonate} → {r.status_code})", flush=True)
                return True
        except Exception:
            continue

    # 最后尝试：直接 GET 首页，检查是否跳转到登录页
    try:
        s = reqs.Session(impersonate="chrome131")
        s.cookies.update(cookies)
        r = s.get("https://www.douyin.com/", timeout=10, allow_redirects=True)
        if r.status_code == 200 and "login" not in r.url.lower():
            print(f"OK  (homepage redirect OK, status={r.status_code})", flush=True)
            return True
    except Exception:
        pass

    print("FAIL", flush=True)
    return False


def main():
    if len(sys.argv) < 2:
        print("FAIL (no path)")
        sys.exit(1)

    path = sys.argv[1]
    if not os.path.isfile(path):
        print("FAIL (file not found)")
        sys.exit(1)

    cookies = load_cookies(path)
    if not cookies:
        print("FAIL (no cookies)")
        sys.exit(1)

    ok = do_verify(cookies)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
