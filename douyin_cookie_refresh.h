/*
 * douyin_cookie_refresh.h - 抖音桌面版 Cookie 提取库 (header-only, 零第三方依赖)
 *
 * 与 Python 版 douyin_cookie_refresh.py 流程一致:
 *   1. 复制 %APPDATA%\douyin\Network\Cookies 与 Local State 到临时目录
 *      (若被运行中的抖音锁定, 先结束 douyin/douyin_guard/douyin_tray 进程)
 *   2. DPAPI 解密 Local State 中的 AES 密钥
 *   3. 内置只读 SQLite B+树解析器读取 Cookie 库, AES-256-GCM 解密每个值,
 *      剥离 32 字节盐前缀
 *   4. 生成两种格式输出
 *
 * 用法: 在任意一个 .c 文件中定义 DY_COOKIE_REFRESH_IMPL 后包含本文件:
 *
 *     #define DY_COOKIE_REFRESH_IMPL
 *     #include "douyin_cookie_refresh.h"
 *
 *     int main(void) {
 *         return dy_douyin_refresh(L".");
 *     }
 *
 * 编译:
 *   MSVC : cl test.c            (自动链接 crypt32.lib / bcrypt.lib)
 *   MinGW: gcc test.c -lcrypt32 -lbcrypt
 *
 * API:
 *   int dy_douyin_refresh(const wchar_t *out_dir);
 *       提取 Cookie 并写入 <out_dir>\cookies.txt 与 <out_dir>\cookies_netscape.txt
 *       返回 0 成功, 非 0 失败 (可从 dy_douyin_last_error() 取错误信息)
 *   int dy_douyin_extract(dy_cookie_result_t *out);
 *       提取 Cookie 到结果结构体, 不写文件
 *   int dy_douyin_save_files(const dy_cookie_result_t *r, const wchar_t *out_dir);
 *       将结果写成两种格式文件
 *   char *dy_cookie_header_string(const dy_cookie_result_t *r);
 *       生成 Cookie 请求头文本 (malloc, 调用者 free)
 *   char *dy_cookie_netscape_string(const dy_cookie_result_t *r);
 *       生成 Netscape 格式文本 (malloc, 调用者 free)
 *   void dy_cookie_result_free(dy_cookie_result_t *r);
 *   const char *dy_douyin_last_error(void);
 *       上次失败的错误信息
 */
#ifndef DY_COOKIE_REFRESH_H
#define DY_COOKIE_REFRESH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char host_key[256];
    char name[128];
    char path[128];
    char value[16384];
    long long expires_utc; /* Chromium WebKit 微秒, 0 表示会话 Cookie */
    unsigned is_secure : 1;
    unsigned is_httponly : 1;
} dy_cookie_t;

typedef struct {
    dy_cookie_t *items;
    size_t count;
    size_t cap;
    int skipped;
    char error[512];
} dy_cookie_result_t;

int dy_douyin_extract(dy_cookie_result_t *out);
int dy_douyin_save_files(const dy_cookie_result_t *r, const wchar_t *out_dir);
int dy_douyin_refresh(const wchar_t *out_dir);
char *dy_cookie_header_string(const dy_cookie_result_t *r);
char *dy_cookie_netscape_string(const dy_cookie_result_t *r);
void dy_cookie_result_free(dy_cookie_result_t *r);
const char *dy_douyin_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* DY_COOKIE_REFRESH_H */

#ifdef DY_COOKIE_REFRESH_IMPL
#ifndef DY_COOKIE_REFRESH_IMPL_ONCE
#define DY_COOKIE_REFRESH_IMPL_ONCE

#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>

#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#endif

#define DY_WIN_EPOCH_TO_UNIX 11644473600LL
#define DY_SALT_PREFIX_LEN 32
#define DY_MAX_REC_SERIALS 128
#define DY_MAX_TREE_DEPTH 64

static char dy_g_error[512] = "";

const char *dy_douyin_last_error(void) { return dy_g_error; }

static void dy_set_error(dy_cookie_result_t *r, const char *msg) {
    if (msg) {
        strncpy(dy_g_error, msg, sizeof(dy_g_error) - 1);
        dy_g_error[sizeof(dy_g_error) - 1] = 0;
        if (r) {
            strncpy(r->error, msg, sizeof(r->error) - 1);
            r->error[sizeof(r->error) - 1] = 0;
        }
    }
}

static char *dy_strdup(const char *s, size_t n) {
    char *d = (char *)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static int dy_ends_with_ci(const char *s, size_t slen, const char *suffix) {
    size_t suflen = strlen(suffix);
    if (slen < suflen) return 0;
    return _strnicmp(s + slen - suflen, suffix, suflen) == 0;
}

static void dy_kill_douyin(void) {
    static const wchar_t *names[] = {L"douyin.exe", L"douyin_guard.exe", L"douyin_tray.exe"};
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
                if (_wcsicmp(pe.szExeFile, names[i]) == 0) {
                    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (h) { TerminateProcess(h, 1); CloseHandle(h); }
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

static unsigned char *dy_read_file(const wchar_t *path, size_t *out_len) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) { CloseHandle(h); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)sz.QuadPart);
    if (!buf) { CloseHandle(h); return NULL; }
    DWORD rd = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)sz.QuadPart, &rd, NULL);
    CloseHandle(h);
    if (!ok) { free(buf); return NULL; }
    *out_len = rd;
    return buf;
}

static int dy_copy_file(const wchar_t *src, const wchar_t *dst, int kill_on_lock) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (CopyFileW(src, dst, FALSE)) return 0;
        DWORD e = GetLastError();
        if (e == ERROR_SHARING_VIOLATION && kill_on_lock && attempt == 0) {
            dy_kill_douyin();
            Sleep(1000);
            continue;
        }
        return -1;
    }
    return -1;
}

static int dy_b64_decode(const char *s, size_t n, unsigned char *out, size_t cap) {
    int acc = 0, bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o < cap) out[o++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    return (int)o;
}

static void *dy_memmem(const void *hay, size_t hay_len, const void *needle, size_t needle_len) {
    if (needle_len == 0 || hay_len < needle_len) return NULL;
    const unsigned char *h = (const unsigned char *)hay;
    const unsigned char *n = (const unsigned char *)needle;
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, needle_len) == 0)
            return (void *)(h + i);
    }
    return NULL;
}

static int dy_aes_gcm_decrypt(const unsigned char *key, size_t key_len,
                              const unsigned char *nonce, size_t nonce_len,
                              const unsigned char *ct, size_t ct_len,
                              const unsigned char *tag, size_t tag_len,
                              unsigned char *out, size_t *out_len) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE kh = NULL;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0);
    if (st != 0) return -1;
    st = BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                           (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                           sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (st != 0) { BCryptCloseAlgorithmProvider(alg, 0); return -1; }
    st = BCryptGenerateSymmetricKey(alg, &kh, NULL, 0, (PUCHAR)key, (ULONG)key_len, 0);
    if (st != 0) { BCryptCloseAlgorithmProvider(alg, 0); return -1; }
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    info.dwInfoVersion = BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO_VERSION;
    info.pbNonce = (PUCHAR)nonce;
    info.cbNonce = (ULONG)nonce_len;
    info.pbTag = (PUCHAR)tag;
    info.cbTag = (ULONG)tag_len;
    ULONG written = 0;
    st = BCryptDecrypt(kh, (PUCHAR)ct, (ULONG)ct_len, &info, NULL, 0,
                       out, (ULONG)ct_len, &written, 0);
    BCryptDestroyKey(kh);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st != 0) return -1;
    *out_len = written;
    return 0;
}

/* ---- 只读 SQLite 解析 ---- */

typedef struct {
    const unsigned char *base;
    size_t len;
    int page_size;
    int reserved;
} dy_db_t;

typedef struct {
    long long serials[DY_MAX_REC_SERIALS];
    int n;
    size_t hdr;
    size_t rec_len;
} dy_rec_t;

static long long dy_varint(const unsigned char **pp, const unsigned char *end) {
    const unsigned char *p = *pp;
    if (p >= end) return -1;
    long long v = 0;
    for (int i = 0; i < 8; i++) {
        unsigned char b = *p++;
        v = (v << 7) | (b & 0x7f);
        if (!(b & 0x80)) { *pp = p; return v; }
        if (p >= end) return -1;
    }
    if (p >= end) return -1;
    v = (v << 8) | *p++;
    *pp = p;
    return v;
}

static unsigned int dy_be32(const unsigned char *p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}

static long long dy_be_int(const unsigned char *p, int n) {
    unsigned long long v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | p[i];
    if (n < 8 && (p[0] & 0x80)) v |= (~0ULL) << (n * 8);
    return (long long)v;
}

static size_t dy_serial_len(long long st) {
    if (st == 0) return 0;
    if (st >= 1 && st <= 4) return (size_t)st;
    if (st == 5) return 6;
    if (st == 6 || st == 7) return 8;
    if (st >= 12) return (size_t)(st - 12) / 2;
    return 0; /* 8, 9, 10, 11 */
}

static int dy_rec_parse(dy_rec_t *r, const unsigned char *payload, size_t len) {
    const unsigned char *p = payload;
    const unsigned char *end = payload + len;
    long long hdr = dy_varint(&p, end);
    if (hdr < 1 || hdr > (long long)len) return 0;
    const unsigned char *hdr_end = payload + hdr;
    int n = 0;
    while (p < hdr_end && n < DY_MAX_REC_SERIALS) {
        long long st = dy_varint(&p, hdr_end);
        if (st < 0) return 0;
        r->serials[n++] = st;
    }
    r->n = n;
    r->hdr = (size_t)hdr;
    r->rec_len = len;
    return 1;
}

static const unsigned char *dy_rec_field(dy_rec_t *r, const unsigned char *payload,
                                         int idx, long long *intv, size_t *len) {
    if (idx < 0 || idx >= r->n) return NULL;
    long long st = r->serials[idx];
    const unsigned char *p = payload + r->hdr;
    for (int i = 0; i < idx; i++) p += dy_serial_len(r->serials[i]);
    size_t flen = dy_serial_len(st);
    if (p + flen > payload + r->rec_len) return NULL;
    if (intv) {
        if (st == 9) *intv = 1;
        else if (st >= 1 && st <= 6) *intv = dy_be_int(p, (int)flen);
        else *intv = 0;
    }
    if (len) *len = flen;
    return p;
}

typedef void (*dy_row_cb)(void *ud, const unsigned char *payload, size_t len);

static int dy_scan_table(dy_db_t *db, int page_no, dy_row_cb cb, void *ud, int depth) {
    if (depth > DY_MAX_TREE_DEPTH) return 0;
    size_t off = (size_t)(page_no - 1) * (size_t)db->page_size;
    if (off >= db->len) return 0;
    const unsigned char *pg = db->base + off;
    const unsigned char *bt = pg + (page_no == 1 ? 100 : 0);
    int type = bt[0];
    int usable = db->page_size - (page_no == 1 ? 100 + db->reserved : db->reserved);
    if (usable < 8) return 0;
    if (type == 0x05) { /* interior table: 头部子页在 bt+8..11, 指针数组从 bt+12 开始 */
        int n = ((int)bt[3] << 8) | bt[4];
        int head_child = (int)dy_be32(bt + 8);
        if (head_child > 0) dy_scan_table(db, head_child, cb, ud, depth + 1);
        if (12 + 2 * n > usable) return 0;
        const unsigned char *ptrs = bt + 12;
        for (int i = 0; i < n; i++) {
            int cptr = ((int)ptrs[i * 2] << 8) | ptrs[i * 2 + 1];
            if (cptr < 8 || cptr + 4 > usable) continue;
            int child = (int)dy_be32(db->base + off + (size_t)cptr);
            if (child > 0) dy_scan_table(db, child, cb, ud, depth + 1);
        }
        return 1;
    }
    int n = ((int)bt[3] << 8) | bt[4];
    if (8 + 2 * n > usable) return 0;
    const unsigned char *ptrs = bt + 8;
    for (int i = 0; i < n; i++) {
        int cptr = ((int)ptrs[i * 2] << 8) | ptrs[i * 2 + 1];
        if (cptr < 8 || cptr >= usable) continue;
        const unsigned char *cell = pg + cptr;
        { /* leaf table */
            const unsigned char *p = cell;
            const unsigned char *cell_end = pg + usable;
            long long plen = dy_varint(&p, cell_end);
            if (plen < 0) continue;
            long long rowid = dy_varint(&p, cell_end);
            (void)rowid;
            if (p > cell_end) continue;
            size_t in_avail = (size_t)(cell_end - p);
            const unsigned char *data = p;
            size_t dlen = (size_t)plen;
            unsigned char *buf = NULL;
            if (plen > (long long)in_avail) {
                size_t prefix = in_avail > 4 ? in_avail - 4 : 0;
                buf = (unsigned char *)malloc((size_t)plen);
                if (!buf) continue;
                memcpy(buf, p, prefix);
                size_t filled = prefix;
                unsigned int ov = dy_be32(p + prefix);
                while (filled < (size_t)plen && ov != 0) {
                    size_t ooff = (size_t)(ov - 1) * (size_t)db->page_size;
                    if (ooff >= db->len) break;
                    const unsigned char *opg = db->base + ooff;
                    unsigned int next = dy_be32(opg);
                    size_t chunk = (size_t)plen - filled;
                    size_t ov_usable = (size_t)db->page_size - (size_t)db->reserved;
                    if (chunk > ov_usable - 4) chunk = ov_usable - 4;
                    memcpy(buf + filled, opg + 4, chunk);
                    filled += chunk;
                    ov = next;
                }
                data = buf;
                dlen = filled;
            }
            cb(ud, data, dlen);
            if (buf) free(buf);
        }
    }
    return 1;
}

/* ---- 建表 SQL 列名解析 ---- */

static int dy_parse_columns(const char *sql, size_t sql_len, char out[][64], int max) {
    const char *lp = (const char *)dy_memmem(sql, sql_len, "(", 1);
    if (!lp) return 0;
    int count = 0;
    int depth = 1;
    int quote = 0;
    char seg[512];
    size_t seglen = 0;
    const char *p = lp + 1;
    const char *end = sql + sql_len;
    while (p < end && depth > 0) {
        char c = *p;
        if (quote) {
            if (c == quote) quote = 0;
            if (seglen < sizeof(seg) - 1) seg[seglen++] = c;
        } else if (c == '"' || c == '`' || c == '[') {
            quote = c;
            if (seglen < sizeof(seg) - 1) seg[seglen++] = c;
        } else if (c == '(') {
            depth++;
            if (seglen < sizeof(seg) - 1) seg[seglen++] = c;
        } else if (c == ')') {
            depth--;
            if (depth == 0) break;
            if (seglen < sizeof(seg) - 1) seg[seglen++] = c;
        } else if (c == ',') {
            if (seglen > 0 && count < max) {
                const char *s = seg, *e = seg + seglen;
                while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
                while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) e--;
                size_t nlen = 0;
                if (s < e && (*s == '"' || *s == '`')) { s++; e--; nlen = (size_t)(e - s); }
                else if (s < e && *s == '[') { s++; if (e > s && e[-1] == ']') e--; nlen = (size_t)(e - s); }
                else {
                    const char *sp = s;
                    while (sp < e && *sp != ' ' && *sp != '\t' && *sp != '(' && *sp != '\n') sp++;
                    nlen = (size_t)(sp - s);
                }
                if (nlen > 0 && nlen < 64) {
                    memcpy(out[count], s, nlen);
                    out[count][nlen] = 0;
                    count++;
                }
            }
            seglen = 0;
        } else {
            if (seglen < sizeof(seg) - 1) seg[seglen++] = c;
        }
        p++;
    }
    if (seglen > 0 && count < max) {
        const char *s = seg, *e = seg + seglen;
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        size_t nlen = 0;
        if (s < e && (*s == '"' || *s == '`')) { s++; e--; nlen = (size_t)(e - s); }
        else {
            const char *sp = s;
            while (sp < e && *sp != ' ' && *sp != '(') sp++;
            nlen = (size_t)(sp - s);
        }
        if (nlen > 0 && nlen < 64) {
            memcpy(out[count], s, nlen);
            out[count][nlen] = 0;
            count++;
        }
    }
    return count;
}

static int dy_col_index(char cols[][64], int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (strcmp(cols[i], name) == 0) return i;
    return -1;
}

/* ---- 主流程 ---- */

typedef struct {
    dy_cookie_result_t *out;
    int i_host, i_name, i_val, i_ev, i_path, i_sec, i_http, i_exp, i_expired;
    const unsigned char *aes_key;
    size_t key_len;
    long long now;
} dy_scan_ctx_t;

static char *dy_cookie_value(const unsigned char *ev, size_t ev_len,
                             const char *plain, size_t plain_len,
                             const unsigned char *key, size_t key_len) {
    if (ev_len == 0) return dy_strdup(plain ? plain : "", plain_len);
    if (ev_len < 15 || memcmp(ev, "v10", 3) != 0) return NULL;
    const unsigned char *nonce = ev + 3;
    const unsigned char *ct = ev + 15;
    size_t ct_len = ev_len - 15;
    if (ct_len <= 16) return NULL;
    size_t data_len = ct_len - 16;
    unsigned char *pt = (unsigned char *)malloc(data_len);
    if (!pt) return NULL;
    size_t pt_len = 0;
    if (dy_aes_gcm_decrypt(key, key_len, nonce, 12, ct, data_len,
                           ct + data_len, 16, pt, &pt_len) != 0) {
        free(pt);
        return NULL;
    }
    size_t off = pt_len > DY_SALT_PREFIX_LEN ? DY_SALT_PREFIX_LEN : 0;
    char *s = (char *)malloc(pt_len - off + 1);
    if (!s) { free(pt); return NULL; }
    memcpy(s, pt + off, pt_len - off);
    s[pt_len - off] = 0;
    free(pt);
    return s;
}

static void dy_row_cookie(void *ud, const unsigned char *payload, size_t plen) {
    dy_scan_ctx_t *ctx = (dy_scan_ctx_t *)ud;
    dy_cookie_result_t *out = ctx->out;
    dy_rec_t rec;
    size_t l;
    if (!dy_rec_parse(&rec, payload, plen)) { out->skipped++; return; }
    const unsigned char *hp = dy_rec_field(&rec, payload, ctx->i_host, NULL, &l);
    if (!hp || l == 0 || l >= 256) { out->skipped++; return; }
    char host[256];
    memcpy(host, hp, l);
    host[l] = 0;
    if (!(dy_ends_with_ci(host, l, "douyin.com") || dy_ends_with_ci(host, l, "bytedance.com"))) {
        out->skipped++;
        return;
    }
    long long is_expired = 0;
    if (ctx->i_expired >= 0)
        dy_rec_field(&rec, payload, ctx->i_expired, &is_expired, NULL);
    if (is_expired) { out->skipped++; return; }
    long long exp = 0;
    dy_rec_field(&rec, payload, ctx->i_exp, &exp, NULL);
    if (exp > 0 && exp / 1000000 - DY_WIN_EPOCH_TO_UNIX < ctx->now) {
        out->skipped++;
        return;
    }
    const unsigned char *np = dy_rec_field(&rec, payload, ctx->i_name, NULL, &l);
    if (!np || l == 0 || l >= 128) { out->skipped++; return; }
    char name[128];
    memcpy(name, np, l);
    name[l] = 0;
    const unsigned char *evp = dy_rec_field(&rec, payload, ctx->i_ev, NULL, &l);
    size_t ev_len = evp ? l : 0;
    const unsigned char *vp = dy_rec_field(&rec, payload, ctx->i_val, NULL, &l);
    size_t v_len = vp ? l : 0;
    char *value = dy_cookie_value(evp, ev_len, (const char *)vp, v_len,
                                  ctx->aes_key, ctx->key_len);
    if (!value || value[0] == 0) {
        if (value) free(value);
        out->skipped++;
        return;
    }
    const unsigned char *pp = dy_rec_field(&rec, payload, ctx->i_path, NULL, &l);
    size_t path_len = pp ? l : 0;
    if (path_len >= sizeof(((dy_cookie_t *)0)->path)) path_len = sizeof(((dy_cookie_t *)0)->path) - 1;
    long long is_sec = 0, is_http = 0;
    dy_rec_field(&rec, payload, ctx->i_sec, &is_sec, NULL);
    dy_rec_field(&rec, payload, ctx->i_http, &is_http, NULL);
    dy_cookie_t c;
    memset(&c, 0, sizeof(c));
    memcpy(c.host_key, host, sizeof(c.host_key) - 1);
    memcpy(c.name, name, sizeof(c.name) - 1);
    if (pp) { memcpy(c.path, pp, path_len); c.path[path_len] = 0; }
    size_t vlen = strlen(value);
    if (vlen >= sizeof(c.value)) vlen = sizeof(c.value) - 1;
    memcpy(c.value, value, vlen);
    c.expires_utc = exp;
    c.is_secure = is_sec ? 1 : 0;
    c.is_httponly = is_http ? 1 : 0;
    if (out->count == out->cap) {
        size_t ncap = out->cap ? out->cap * 2 : 64;
        dy_cookie_t *ni = (dy_cookie_t *)realloc(out->items, ncap * sizeof(*ni));
        if (!ni) { free(value); out->skipped++; return; }
        out->items = ni;
        out->cap = ncap;
    }
    out->items[out->count++] = c;
    free(value);
}

typedef struct {
    char type[64];
    char name[64];
    long long rootpage;
    char *sql;
    size_t sql_len;
} dy_master_row_t;

static void dy_row_master(void *ud, const unsigned char *payload, size_t plen) {
    dy_master_row_t *m = (dy_master_row_t *)ud;
    dy_rec_t rec;
    size_t l;
    if (!dy_rec_parse(&rec, payload, plen)) return;
    const unsigned char *tp = dy_rec_field(&rec, payload, 0, NULL, &l);
    const unsigned char *np = dy_rec_field(&rec, payload, 1, NULL, &l);
    long long root = 0;
    const unsigned char *rp = dy_rec_field(&rec, payload, 3, &root, NULL);
    (void)rp;
    const unsigned char *sp = dy_rec_field(&rec, payload, 4, NULL, &l);
    if (!tp || !np || !sp) return;
    size_t tl = 0, nl = 0;
    dy_rec_field(&rec, payload, 0, NULL, &tl);
    dy_rec_field(&rec, payload, 1, NULL, &nl);
    if (tl >= sizeof(m->type) || nl >= sizeof(m->name)) return;
    memcpy(m->type, tp, tl);
    m->type[tl] = 0;
    memcpy(m->name, np, nl);
    m->name[nl] = 0;
    if (strcmp(m->type, "table") == 0 && strcmp(m->name, "cookies") == 0) {
        m->rootpage = root;
        if (m->sql) { free(m->sql); m->sql = NULL; m->sql_len = 0; }
        m->sql = dy_strdup((const char *)sp, l);
        m->sql_len = l;
    }
}

static int dy_sort_cmp(const void *a, const void *b) {
    const dy_cookie_t *ca = (const dy_cookie_t *)a;
    const dy_cookie_t *cb = (const dy_cookie_t *)b;
    int c = strcmp(ca->host_key, cb->host_key);
    if (c) return c;
    return strcmp(ca->name, cb->name);
}

int dy_douyin_extract(dy_cookie_result_t *out) {
    memset(out, 0, sizeof(*out));
    const char *appdata = getenv("APPDATA");
    if (!appdata) { dy_set_error(out, "未找到 APPDATA 环境变量"); return -1; }
    wchar_t appdata_w[MAX_PATH];
    if (MultiByteToWideChar(CP_ACP, 0, appdata, -1, appdata_w, MAX_PATH) == 0) {
        dy_set_error(out, "APPDATA 转宽字符失败");
        return -1;
    }
    wchar_t cookies_src[MAX_PATH], ls_src[MAX_PATH];
    _snwprintf(cookies_src, MAX_PATH, L"%s\\douyin\\Network\\Cookies", appdata_w);
    _snwprintf(ls_src, MAX_PATH, L"%s\\douyin\\Local State", appdata_w);
    if (GetFileAttributesW(cookies_src) == INVALID_FILE_ATTRIBUTES) {
        dy_set_error(out, "未找到 Cookie 数据库 (请先安装并登录抖音桌面版)");
        return -1;
    }
    wchar_t tmpdir[MAX_PATH];
    DWORD tn = GetTempPathW(MAX_PATH, tmpdir);
    if (tn == 0 || tn >= MAX_PATH) { dy_set_error(out, "获取临时目录失败"); return -1; }
    wchar_t cookies_tmp[MAX_PATH], ls_tmp[MAX_PATH];
    _snwprintf(cookies_tmp, MAX_PATH, L"%sdy_cookies_%lu.db", tmpdir, GetCurrentProcessId());
    _snwprintf(ls_tmp, MAX_PATH, L"%sdy_localstate_%lu.json", tmpdir, GetCurrentProcessId());
    DeleteFileW(cookies_tmp);
    DeleteFileW(ls_tmp);
    if (dy_copy_file(cookies_src, cookies_tmp, 1) != 0) {
        dy_set_error(out, "无法复制 Cookie 数据库 (被锁定或不存在)");
        return -1;
    }
    if (dy_copy_file(ls_src, ls_tmp, 0) != 0) {
        dy_set_error(out, "无法复制 Local State");
        DeleteFileW(cookies_tmp);
        return -1;
    }
    size_t ls_len = 0;
    unsigned char *ls = dy_read_file(ls_tmp, &ls_len);
    if (!ls) {
        dy_set_error(out, "读取 Local State 失败");
        DeleteFileW(cookies_tmp);
        DeleteFileW(ls_tmp);
        return -1;
    }
    static const char nk[] = "\"encrypted_key\":\"";
    char *hit = (char *)dy_memmem(ls, ls_len, nk, sizeof(nk) - 1);
    if (!hit) {
        dy_set_error(out, "Local State 中未找到 encrypted_key");
        free(ls);
        DeleteFileW(cookies_tmp);
        DeleteFileW(ls_tmp);
        return -1;
    }
    char *start = hit + sizeof(nk) - 1;
    char *end = start;
    char *ls_end = (char *)ls + ls_len;
    while (end < ls_end && *end != '"' &&
           ((*end >= 'A' && *end <= 'Z') || (*end >= 'a' && *end <= 'z') ||
            (*end >= '0' && *end <= '9') || *end == '+' || *end == '/' || *end == '='))
        end++;
    unsigned char enc_key[1024];
    int enc_len = dy_b64_decode(start, (size_t)(end - start), enc_key, sizeof(enc_key));
    free(ls);
    if (enc_len < 6 || memcmp(enc_key, "DPAPI", 5) != 0) {
        dy_set_error(out, "密钥格式错误 (非 DPAPI)");
        DeleteFileW(cookies_tmp);
        DeleteFileW(ls_tmp);
        return -1;
    }
    DATA_BLOB in = {(DWORD)(enc_len - 5), enc_key + 5};
    DATA_BLOB outb = {0, NULL};
    if (!CryptUnprotectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &outb)) {
        dy_set_error(out, "DPAPI 解密失败");
        DeleteFileW(cookies_tmp);
        DeleteFileW(ls_tmp);
        return -1;
    }
    size_t db_len = 0;
    unsigned char *db = dy_read_file(cookies_tmp, &db_len);
    DeleteFileW(cookies_tmp);
    DeleteFileW(ls_tmp);
    if (!db) {
        LocalFree(outb.pbData);
        dy_set_error(out, "读取 Cookie 数据库失败");
        return -1;
    }
    static const unsigned char magic[] = "SQLite format 3";
    int rc = -1;
    do {
        if (db_len < 100 || memcmp(db, magic, 16) != 0) {
            dy_set_error(out, "Cookie 数据库格式错误");
            break;
        }
        dy_db_t dbb;
        dbb.base = db;
        dbb.len = db_len;
        dbb.page_size = (int)((db[16] << 8) | db[17]);
        dbb.reserved = db[20];
        if (dbb.page_size < 512 || dbb.page_size > 65536 ||
            (dbb.page_size & (dbb.page_size - 1)) != 0) {
            dy_set_error(out, "Cookie 数据库页大小无效");
            break;
        }
        dy_master_row_t master;
        memset(&master, 0, sizeof(master));
        dy_scan_table(&dbb, 1, dy_row_master, &master, 0);
        if (!master.sql || master.rootpage <= 0) {
            dy_set_error(out, "未找到 cookies 表");
            break;
        }
        char cols[64][64];
        int ncols = dy_parse_columns(master.sql, master.sql_len, cols, 64);
        free(master.sql);
        if (ncols <= 0) {
            dy_set_error(out, "解析 cookies 表结构失败");
            break;
        }
        dy_scan_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.out = out;
        ctx.i_host = dy_col_index(cols, ncols, "host_key");
        ctx.i_name = dy_col_index(cols, ncols, "name");
        ctx.i_val = dy_col_index(cols, ncols, "value");
        ctx.i_ev = dy_col_index(cols, ncols, "encrypted_value");
        ctx.i_path = dy_col_index(cols, ncols, "path");
        ctx.i_sec = dy_col_index(cols, ncols, "is_secure");
        ctx.i_http = dy_col_index(cols, ncols, "is_httponly");
        ctx.i_exp = dy_col_index(cols, ncols, "expires_utc");
        ctx.i_expired = dy_col_index(cols, ncols, "is_expired");
        if (ctx.i_host < 0 || ctx.i_name < 0 || ctx.i_val < 0 || ctx.i_ev < 0) {
            dy_set_error(out, "cookies 表缺少必要列");
            break;
        }
        ctx.aes_key = outb.pbData;
        ctx.key_len = outb.cbData;
        ctx.now = (long long)time(NULL);
        dy_scan_table(&dbb, (int)master.rootpage, dy_row_cookie, &ctx, 0);
        qsort(out->items, out->count, sizeof(dy_cookie_t), dy_sort_cmp);
        if (out->count == 0) {
            dy_set_error(out, "未提取到任何有效 Cookie");
            break;
        }
        rc = 0;
    } while (0);
    free(db);
    LocalFree(outb.pbData);
    if (rc != 0) dy_cookie_result_free(out);
    return rc;
}

void dy_cookie_result_free(dy_cookie_result_t *r) {
    if (!r) return;
    free(r->items);
    memset(r, 0, sizeof(*r));
}

static size_t dy_header_size(const dy_cookie_result_t *r) {
    size_t n = 1;
    for (size_t i = 0; i < r->count; i++)
        n += strlen(r->items[i].name) + 1 + strlen(r->items[i].value) + 2;
    return n;
}

char *dy_cookie_header_string(const dy_cookie_result_t *r) {
    size_t cap = dy_header_size(r);
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < r->count; i++) {
        int w = _snprintf(buf + o, cap - o, "%s=%s; ",
                          r->items[i].name, r->items[i].value);
        if (w < 0) break;
        o += (size_t)w;
    }
    if (o >= 2) o -= 2;
    buf[o] = 0;
    return buf;
}

static size_t dy_netscape_size(const dy_cookie_result_t *r) {
    size_t n = 256;
    for (size_t i = 0; i < r->count; i++) {
        const dy_cookie_t *c = &r->items[i];
        n += strlen(c->host_key) + 16 + strlen(c->path) + 16 +
             32 + strlen(c->name) + strlen(c->value) + 8;
    }
    return n;
}

char *dy_cookie_netscape_string(const dy_cookie_result_t *r) {
    size_t cap = dy_netscape_size(r);
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    size_t o = 0;
    o += (size_t)_snprintf(buf + o, cap - o, "# Netscape HTTP Cookie File\n");
    o += (size_t)_snprintf(buf + o, cap - o, "# Generated by douyin_cookie_refresh.h (%s)\n",
                           __DATE__);
    o += (size_t)_snprintf(buf + o, cap - o,
                           "# domain\tinclude_subdomains\tpath\tsecure\texpiry\tname\tvalue\n");
    for (size_t i = 0; i < r->count; i++) {
        const dy_cookie_t *c = &r->items[i];
        const char *domain = c->host_key;
        if (c->is_httponly) {
            o += (size_t)_snprintf(buf + o, cap - o, "#HttpOnly_");
        }
        const char *inc = c->host_key[0] == '.' ? "TRUE" : "FALSE";
        const char *sec = c->is_secure ? "TRUE" : "FALSE";
        long long exp = c->expires_utc > 0 ? c->expires_utc / 1000000 - DY_WIN_EPOCH_TO_UNIX : 0;
        o += (size_t)_snprintf(buf + o, cap - o, "%s\t%s\t%s\t%s\t%lld\t%s\t%s\n",
                               domain, inc, c->path[0] ? c->path : "/", sec,
                               exp, c->name, c->value);
    }
    buf[o] = 0;
    return buf;
}

static int dy_write_file(const wchar_t *path, const char *text) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD w = 0;
    BOOL ok = WriteFile(h, text, (DWORD)strlen(text), &w, NULL);
    CloseHandle(h);
    return ok ? 0 : -1;
}

int dy_douyin_save_files(const dy_cookie_result_t *r, const wchar_t *out_dir) {
    char *hdr = dy_cookie_header_string(r);
    if (!hdr) { dy_set_error((dy_cookie_result_t *)r, "内存不足"); return -1; }
    char *ns = dy_cookie_netscape_string(r);
    if (!ns) { free(hdr); dy_set_error((dy_cookie_result_t *)r, "内存不足"); return -1; }
    wchar_t p1[MAX_PATH], p2[MAX_PATH];
    _snwprintf(p1, MAX_PATH, L"%s\\cookies.txt", out_dir);
    _snwprintf(p2, MAX_PATH, L"%s\\cookies_netscape.txt", out_dir);
    int rc = 0;
    if (dy_write_file(p1, hdr) != 0) {
        dy_set_error((dy_cookie_result_t *)r, "写入 cookies.txt 失败");
        rc = -1;
    } else if (dy_write_file(p2, ns) != 0) {
        dy_set_error((dy_cookie_result_t *)r, "写入 cookies_netscape.txt 失败");
        rc = -1;
    }
    free(hdr);
    free(ns);
    return rc;
}

int dy_douyin_refresh(const wchar_t *out_dir) {
    dy_cookie_result_t r;
    int rc = dy_douyin_extract(&r);
    if (rc != 0) return rc;
    rc = dy_douyin_save_files(&r, out_dir);
    dy_cookie_result_free(&r);
    return rc;
}

#endif /* DY_COOKIE_REFRESH_IMPL_ONCE */
#endif /* DY_COOKIE_REFRESH_IMPL */