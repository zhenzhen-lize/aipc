#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <windows.h>
#include <direct.h>
#include <wchar.h>

#define MAX_PATH_LEN 2048
#define MAX_URL_LEN  2048
#define PROG_NAME    "aipc"

/* ============ 前向声明 ============ */
static void get_exe_dir(char* dir, size_t size);
static int  run_cmd_capture(const char* utf8_cmd, char* out, size_t outsz);

/* ============ 工具函数 ============ */

static void print_usage(void) {
    printf("用法:\n");
    printf("  %s <url> [options]\n\n", PROG_NAME);
    printf("参数:\n");
    printf("  url [url2 ...]   视频链接，可一次给多个（抖音/B站/YouTube/快手/小红书/微博等）\n\n");
    printf("选项:\n");
    printf("  --quality N      设置下载质量 (360/480/720/1080/2160)，默认最高\n");
    printf("  -q, --quiet      安静模式（减少输出）\n");
    printf("  --no-watermark   下载无水印版本（仅抖音，走专用通道）\n");
    printf("  --no-playlist, -np  仅下载单个视频，不下载合集\n");
    printf("  --dir PATH       指定下载目录\n");
    printf("  --cookie PATH    指定 cookies_netscape.txt 路径\n");
    printf("  -f, --file PATH  从文本文件读取链接列表（每行一条, # 开头为注释）\n");
    printf("  --retry N        失败后重试次数（默认 2）\n");
    printf("  --retry-delay S  每次重试前等待秒数（默认 2）\n");
    printf("  --dry-run        只显示信息，不下载\n");
    printf("  -h, --help       显示此帮助\n");
    printf("  --version        显示版本信息\n\n");
    printf("环境变量:\n");
    printf("  AIPC_DOWNLOAD_DIR  默认下载目录\n");
    printf("  AIPC_COOKIES       cookies_netscape.txt 路径\n");
    printf("  AIPC_QUIET         1=安静模式（减少输出）\n");
}

static void extract_url_from_text(const char* text, char* url_out, size_t size) {
    url_out[0] = '\0';
    const char* p = strstr(text, "https://");
    if (!p) p = strstr(text, "http://");
    if (!p) {
        if (strlen(text) >= 4 && text[0] == 'B' && text[1] == 'V') {
            snprintf(url_out, size, "https://www.bilibili.com/video/%s", text);
            return;
        }
        if (strlen(text) < size - 1) {
            strncpy(url_out, text, size - 1);
            url_out[size - 1] = '\0';
        }
        return;
    }
    size_t i = 0;
    while (p[i] && i < size - 1) {
        char c = p[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') break;
        url_out[i] = c;
        i++;
    }
    url_out[i] = '\0';
}

static void convert_url(char* url) {
    char extracted[1024];
    extract_url_from_text(url, extracted, sizeof(extracted));
    if (strstr(extracted, "http") == NULL && strstr(extracted, "://") == NULL) return;
    if (strcmp(url, extracted) != 0) {
        printf("[提取] %s\n", extracted);
        strncpy(url, extracted, MAX_URL_LEN - 1);
        url[MAX_URL_LEN - 1] = '\0';
    }
    if (strstr(url, "douyin.com") && strstr(url, "jingxuan?modal_id=")) {
        char* pos = strstr(url, "jingxuan?modal_id=");
        char id[64] = {0};
        char* start = pos + 18;
        int i = 0;
        while (start[i] != '\0' && start[i] != '&' && i < 63) { id[i] = start[i]; i++; }
        char new_url[256];
        snprintf(new_url, sizeof(new_url), "https://www.douyin.com/video/%s", id);
        strncpy(url, new_url, MAX_URL_LEN - 1);
        url[MAX_URL_LEN - 1] = '\0';
        printf("[转换] 抖音精选 -> %s\n", url);
    }
    /* 抖音短链接解析：v.douyin.com/xxx -> www.douyin.com/video/xxx */
    if (strstr(url, "v.douyin.com")) {
        printf("[解析] 抖音短链接，正在展开...\n");
        char cmd[3000];
        char exe_dir[MAX_PATH];
        get_exe_dir(exe_dir, sizeof(exe_dir));
        char script[MAX_PATH];
        snprintf(script, sizeof(script), "%s\\resolve_douyin_url.py", exe_dir);
        snprintf(cmd, sizeof(cmd), "python \"%s\" \"%s\"", script, url);
        char out[1024] = {0};
        int ret = run_cmd_capture(cmd, out, sizeof(out));
        if (ret == 0 && out[0]) {
            char* p = out;
            while (*p == ' ' || *p == '\n' || *p == '\r') p++;
            char* nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            if (strlen(p) > 8 && strstr(p, "http")) {
                strncpy(url, p, MAX_URL_LEN - 1);
                url[MAX_URL_LEN - 1] = '\0';
                printf("[解析] %s\n", url);
            } else {
                printf("[解析] 短链接解析失败，使用原始链接\n");
            }
        } else {
            printf("[解析] 解析脚本无输出，使用原始链接\n");
        }
    }
    if (strstr(url, "b23.tv")) printf("[识别] B站短链接 (b23.tv)\n");
    if (strstr(url, "v.kuaishou.com") || strstr(url, "kwai.jp")) printf("[识别] 快手链接\n");
    if (strstr(url, "xiaohongshu.com") || strstr(url, "xhslink.com")) printf("[识别] 小红书链接\n");
}

static void normalize_path(char* path) {
    char* start = path;
    while (*start == ' ' || *start == '\t') start++;
    if (start != path) memmove(path, start, strlen(start) + 1);
    size_t len = strlen(path);
    if (len > 0 && (path[len - 1] == '"' || path[len - 1] == '\'')) { path[len - 1] = '\0'; len--; }
    if (len > 0 && (path[0] == '"' || path[0] == '\'')) { memmove(path, path + 1, len); len--; }
    for (char* p = path; *p; p++) if (*p == '\\') *p = '/';
    len = strlen(path);
    while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\')) { path[len - 1] = '\0'; len--; }
}

static void get_exe_dir(char* dir, size_t size) {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    char* last_slash = strstr(exe_path, "\\");
    while (last_slash) {
        char* nx = strstr(last_slash + 1, "\\");
        if (!nx) break;
        last_slash = nx;
    }
    if (last_slash) *last_slash = '\0';
    strncpy(dir, exe_path, size - 1);
    dir[size - 1] = '\0';
}

static int mkdir_recursive(const char* path) {
    char tmp[2048];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) tmp[len - 1] = '\0';
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') { *p = '\0'; _mkdir(tmp); *p = '/'; }
    }
    _mkdir(tmp);
    return 0;
}

static int copy_file_safe(const char* src, const char* dst) {
    if (_access(src, 0) != 0) return 0;
    if (CopyFileA(src, dst, FALSE)) return 1;
    return 0;
}

static long long get_file_mtime(const char* path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FILETIME ft;
    if (!GetFileTime(h, NULL, NULL, &ft)) { CloseHandle(h); return 0; }
    CloseHandle(h);
    return ((long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

/* 从 URL 提取视频 ID（用于去重检测） */
static void extract_video_id(const char* url, char* id_out, size_t id_size) {
    id_out[0] = '\0';
    const char* patterns[] = {
        "video/", "modal_id=", "aweme_id=", "bvid=", "item_id="
    };
    for (int pi = 0; pi < 5; pi++) {
        const char* p = strstr(url, patterns[pi]);
        if (!p) continue;
        p += strlen(patterns[pi]);
        int i = 0;
        while (*p && *p != '/' && *p != '?' && *p != '&' && *p != ' ' && i < (int)id_size - 1) {
            id_out[i++] = *p++;
        }
        id_out[i] = '\0';
        if (id_out[0]) return;
    }
}

/* 检查目标目录是否已存在同名或同 ID 的视频文件 */
static int is_duplicate(const char* out_dir, const char* url) {
    char vid[64] = {0};
    extract_video_id(url, vid, sizeof(vid));
    /* 检查历史记录文件（.aipc_history） */
    char hist_path[MAX_PATH_LEN];
    snprintf(hist_path, sizeof(hist_path), "%s\\.aipc_history", out_dir);
    FILE* hf = fopen(hist_path, "r");
    if (hf) {
        char line[256];
        while (fgets(line, sizeof(line), hf)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (strlen(line) >= strlen(vid) && strstr(line, vid) == line) {
                fclose(hf);
                return 1;
            }
        }
        fclose(hf);
    }
    /* 再检查文件名 */
    char search_path[MAX_PATH_LEN];
    snprintf(search_path, sizeof(search_path), "%s\\*.mp4", out_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search_path, &fd);
    if (h == INVALID_HANDLE_VALUE) { FindClose(h); return 0; }
    int found = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (vid[0] && strstr(fd.cFileName, vid)) { found = 1; break; }
            /* 按标题关键字匹配 */
            const char* title_hint = strstr(url, "/video/");
            if (title_hint) {
                title_hint += 7;
                int hl = (int)strlen(title_hint);
                for (int i = 0; i < hl && title_hint[i] != '/' && title_hint[i] != '?'; i++) hl = i;
                if (hl > 3 && strstr(fd.cFileName, title_hint)) { found = 1; break; }
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

/* 将视频 ID 追加到历史记录文件 */
static void record_downloaded(const char* out_dir, const char* url) {
    char vid[64] = {0};
    extract_video_id(url, vid, sizeof(vid));
    if (!vid[0]) return;
    char hist_path[MAX_PATH_LEN];
    snprintf(hist_path, sizeof(hist_path), "%s\\.aipc_history", out_dir);
    FILE* hf = fopen(hist_path, "a");
    if (hf) { fprintf(hf, "%s\n", vid); fclose(hf); }
}

/* ============ 命令执行（兼容中文路径） ============ */
/* 将 UTF-8 命令转为 ANSI(GBK) 后交给 cmd，解决中文目录乱码问题 */

static char* utf8_to_acp(const char* u) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, u, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t* w = (wchar_t*)malloc((size_t)wlen * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, u, -1, w, wlen);
    int alen = WideCharToMultiByte(CP_ACP, 0, w, -1, NULL, 0, NULL, NULL);
    char* a = (char*)malloc((size_t)alen);
    WideCharToMultiByte(CP_ACP, 0, w, -1, a, alen, NULL, NULL);
    free(w);
    return a;
}

static int run_cmd(const char* utf8_cmd) {
    char* a = utf8_to_acp(utf8_cmd);
    if (!a) return -1;
    FILE* fp = _popen(a, "r");
    free(a);
    int ret = -1;
    if (fp) {
        char buf[8192];
        while (fgets(buf, sizeof(buf), fp)) {
            /* 始终输出下载进度行和错误行 */
            if (strstr(buf, "ERROR") || strstr(buf, "Error") || strstr(buf, "error") ||
                strstr(buf, "WARNING") || strstr(buf, "Already") ||
                strstr(buf, "Destination") || strstr(buf, "[download]") ||
                strstr(buf, "[BiliBili]") || strstr(buf, "Merging") ||
                strstr(buf, "Deleting") || strstr(buf, "Writing") ||
                strstr(buf, "HTTP") || strstr(buf, "进度") ||
                strstr(buf, "%") || strstr(buf, "MB/s")) {
                printf("  %s", buf);
            }
        }
        ret = _pclose(fp);
    } else {
        printf("  ❌ 无法启动命令（请确认 yt-dlp / python 在 PATH 中）\n");
    }
    return ret;
}

static int run_cmd_capture(const char* utf8_cmd, char* out, size_t outsz) {
    char* a = utf8_to_acp(utf8_cmd);
    if (!a) return -1;
    FILE* fp = _popen(a, "r");
    free(a);
    if (out && outsz) out[0] = '\0';
    int ret = -1;
    if (fp) {
        char buf[8192];
        size_t total = 0;
        while (fgets(buf, sizeof(buf), fp)) {
            if (out && outsz) {
                size_t l = strlen(buf);
                if (total + l < outsz - 1) { memcpy(out + total, buf, l); total += l; out[total] = '\0'; }
            }
        }
        ret = _pclose(fp);
    }
    return ret;
}

/* ============ Cookie 管理 ============ */

static int copy_cookie_to_dir(const wchar_t* target_dir, const char* force_cookie) {
    char exe_dir[MAX_PATH];
    wchar_t dst_path_w[MAX_PATH];
    char src_path[MAX_PATH], dst_path[MAX_PATH];
    get_exe_dir(exe_dir, sizeof(exe_dir));
    const char* cookie_src = force_cookie ? force_cookie : NULL;
    if (!cookie_src) {
        snprintf(src_path, sizeof(src_path), "%s/cookies_netscape.txt", exe_dir);
        cookie_src = src_path;
    }
    wcscpy(dst_path_w, target_dir);
    wcscat(dst_path_w, L"\\cookies_netscape.txt");
    WideCharToMultiByte(CP_ACP, 0, dst_path_w, -1, dst_path, sizeof(dst_path), NULL, NULL);
    if (_waccess(dst_path_w, 0) == 0 && _access(cookie_src, 0) == 0) {
        long long mt_dst = get_file_mtime(dst_path);
        long long mt_src = get_file_mtime(cookie_src);
        if (mt_src > 0 && mt_dst >= mt_src) return 1;
    }
    if (CopyFileA(cookie_src, dst_path, FALSE)) return 1;
    return 0;
}

static int verify_cookie_with_api(const char* cookie_path) {
    char exe_dir[MAX_PATH];
    get_exe_dir(exe_dir, sizeof(exe_dir));
    char script[MAX_PATH];
    snprintf(script, sizeof(script), "%s/verify_cookie.py", exe_dir);
    if (_access(script, 0) != 0) { printf("⚠️  未找到 verify_cookie.py\n"); return 0; }
    char cmd[3000];
    snprintf(cmd, sizeof(cmd), "python \"%s\" \"%s\"", script, cookie_path);
    char out[512];
    run_cmd_capture(cmd, out, sizeof(out));
    return (strstr(out, "OK") != NULL);
}

static int check_cookie(const char* cookie_path) {
    if (!cookie_path || _access(cookie_path, 0) != 0) {
        printf("⚠️  未找到 Cookie 文件\n");
        return 0;
    }
    FILE* f = fopen(cookie_path, "r");
    if (!f) { printf("⚠️  无法读取 Cookie 文件\n"); return 0; }
    char line[1024];
    int count = 0;
    while (fgets(line, sizeof(line), f) && count < 200) {
        if (line[0] != '#' && strchr(line, '\t')) count++;
    }
    fclose(f);
    if (count == 0) { printf("⚠️  Cookie 文件为空\n"); return 0; }
    printf("🔍 验证 Cookie (%d 个)...", count);
    fflush(stdout);
    int valid = verify_cookie_with_api(cookie_path);
    printf("%s\n", valid ? "✅ 有效" : "❌ 无效或过期");
    return valid;
}

static int ensure_cookie(const char* out_dir, const char* cookie_path) {
    wchar_t w_out[MAX_PATH_LEN];
    MultiByteToWideChar(CP_ACP, 0, out_dir, -1, w_out, MAX_PATH_LEN);
    if (!cookie_path || _access(cookie_path, 0) != 0) cookie_path = NULL;
    if (!copy_cookie_to_dir(w_out, cookie_path))
        printf("⚠️  未找到 Cookie 文件，请用 --cookie 指定文件\n");
    char target[MAX_PATH_LEN];
    snprintf(target, sizeof(target), "%s/cookies_netscape.txt", out_dir);
    int valid = check_cookie(target);
    if (valid) return valid;
    return valid;
}

/* ============ 下载 ============ */

static const char* quality_filter(int q) {
    switch (q) {
        case 360:  return "bv[height<=360]+ba/b";
        case 480:  return "bv[height<=480]+ba/b";
        case 720:  return "bv[height<=720]+ba/b";
        case 1080: return "bv[height<=1080]+ba/b";
        case 2160: return "bv[height<=2160]+ba/b";
        default:   return "bv+ba/b";
    }
}

static void get_ytdlp_cmd(char* buf, size_t sz) {
    char exe_dir[MAX_PATH_LEN];
    get_exe_dir(exe_dir, sizeof(exe_dir));
    char cand[MAX_PATH_LEN];
    snprintf(cand, sizeof(cand), "%s/yt-dlp.exe", exe_dir);
    if (_access(cand, 0) == 0) { snprintf(buf, sz, "\"%s\"", cand); return; }
    /* 回退：调用系统 python 的 yt_dlp 模块（需 pip install yt-dlp） */
    snprintf(buf, sz, "python -m yt_dlp");
}

static int run_ytdlp(const char* url, const char* out_dir, int no_playlist, int quality) {
    char ytdlp[MAX_PATH_LEN];
    get_ytdlp_cmd(ytdlp, sizeof(ytdlp));
    char cookie_path[MAX_PATH_LEN];
    snprintf(cookie_path, sizeof(cookie_path), "%s/cookies_netscape.txt", out_dir);
    char f_arg[128];
    snprintf(f_arg, sizeof(f_arg), "%s", quality_filter(quality));
    char q_args[128] = {0};
    if (no_playlist) strcat(q_args, " --no-playlist");
    char dir_cmd[4608];
    snprintf(dir_cmd, sizeof(dir_cmd),
        "cd /d \"%s\" && %s --cookies \"%s\" -f \"%s\" %s -o \"%%(title)s [%%(id)s].%%(ext)s\" \"%s\"",
        out_dir, ytdlp, cookie_path, f_arg, q_args, url);
    printf("\n[yt-dlp] %s\n", url);
    int result = run_cmd(dir_cmd);
    return (result == 0);
}

static int run_python_helper(const char* url, const char* out_dir,
                             int no_playlist, int quality, int no_watermark) {
    char python_script[MAX_PATH_LEN];
    get_exe_dir(python_script, sizeof(python_script));
    strcat(python_script, "\\download_helper.py");
    if (_access(python_script, 0) != 0) {
        printf("  ❌ 未找到 download_helper.py\n");
        return 0;
    }
    char cmd[4608];
    snprintf(cmd, sizeof(cmd),
        "cd /d \"%s\" && python \"%s\" \"%s\" --out-dir \".\" --quality %d %s %s",
        out_dir, python_script, url, quality,
        no_playlist ? "--no-playlist" : "",
        no_watermark ? "--no-watermark" : "");
    printf("\n[Python Helper] %s\n", url);
    int result = run_cmd(cmd);
    return (result == 0);
}

/* ============ 主函数 ============ */

#define MAX_URLS 64

static char g_urls[MAX_URLS][MAX_URL_LEN];

static int looks_like_url(const char* s) {
    if (!s) return 0;
    return (strstr(s, "http://") || strstr(s, "https://") ||
            strstr(s, "BV") || strstr(s, "b23.tv") ||
            strstr(s, "douyin.com") || strstr(s, "bilibili.com") ||
            strstr(s, "kuaishou.com") || strstr(s, "xiaohongshu.com") ||
            strstr(s, "weibo.com") || strstr(s, "youtube.com") || strstr(s, "youtu.be"));
}

/* 从文本文件逐行读取链接(跳过空行与 # 注释), 追加到 g_urls; 返回新增数量 */
static int load_urls_from_file(const char* path, int* count) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("⚠️  无法打开链接文件: %s\n", path); return 0; }
    char line[MAX_URL_LEN];
    int added = 0;
    while (fgets(line, sizeof(line), f)) {
        char* s = line;
        if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
            s += 3; /* 去 UTF-8 BOM */
        s[strcspn(s, "\r\n")] = '\0';
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0' || *s == '#') continue;
        if (*count >= MAX_URLS) break;
        strncpy(g_urls[*count], s, MAX_URL_LEN - 1);
        g_urls[*count][MAX_URL_LEN - 1] = '\0';
        (*count)++;
        added++;
    }
    fclose(f);
    return added;
}

int main(int argc, char *argv[]) {
    char output_dir[MAX_PATH_LEN] = {0};
    char cookie_path[MAX_PATH_LEN] = {0};
    char file_list[MAX_PATH_LEN] = {0};
    int no_playlist = 0, no_watermark = 0, dry_run = 0;
    int quality = 0, quiet = 0;
    int retry_count = 2, retry_delay = 2;

    int url_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(); return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("aipc version 2.1.0\n");
            return 0;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
        } else if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "-q") == 0) {
            quiet = 1;
        } else if (strcmp(argv[i], "--no-playlist") == 0 || strcmp(argv[i], "-np") == 0) {
            no_playlist = 1;
        } else if (strcmp(argv[i], "--no-watermark") == 0) {
            no_watermark = 1;
        } else if (strcmp(argv[i], "--quality") == 0 && i + 1 < argc) {
            quality = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--retry") == 0 && i + 1 < argc) {
            retry_count = atoi(argv[++i]);
            if (retry_count < 0) retry_count = 0;
        } else if (strcmp(argv[i], "--retry-delay") == 0 && i + 1 < argc) {
            retry_delay = atoi(argv[++i]);
            if (retry_delay < 0) retry_delay = 0;
        } else if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            strncpy(output_dir, argv[++i], sizeof(output_dir) - 1);
        } else if (strcmp(argv[i], "--cookie") == 0 && i + 1 < argc) {
            strncpy(cookie_path, argv[++i], sizeof(cookie_path) - 1);
        } else if ((strcmp(argv[i], "--file") == 0 || strcmp(argv[i], "-f") == 0) && i + 1 < argc) {
            strncpy(file_list, argv[++i], sizeof(file_list) - 1);
        } else if (looks_like_url(argv[i])) {
            if (url_count < MAX_URLS) {
                strncpy(g_urls[url_count], argv[i], MAX_URL_LEN - 1);
                g_urls[url_count][MAX_URL_LEN - 1] = '\0';
                url_count++;
            }
        } else if (!output_dir[0] && strlen(argv[i]) > 0 && argv[i][0] != '-' && strlen(argv[i]) < 200) {
            strncpy(output_dir, argv[i], sizeof(output_dir) - 1);
        }
    }

    /* 从文件追加链接(每行一个, 支持 # 注释) */
    if (file_list[0]) load_urls_from_file(file_list, &url_count);

    char* env_dir = getenv("AIPC_DOWNLOAD_DIR");
    char* env_cookie = getenv("AIPC_COOKIES");
    char* env_quiet = getenv("AIPC_QUIET");
    if (!output_dir[0] && env_dir) strncpy(output_dir, env_dir, sizeof(output_dir) - 1);
    if (!cookie_path[0] && env_cookie) strncpy(cookie_path, env_cookie, sizeof(cookie_path) - 1);
    if (env_quiet && strcmp(env_quiet, "1") == 0) quiet = 1;

    /* 无链接参数：交互式输入（可一次粘贴多行，空行结束） */
    if (url_count == 0) {
        printf("aipc - 智能视频下载器\n\n");
        printf("请输入视频链接或分享文本（可多条，每行一条，空行开始下载）：\n");
        printf("支持: 抖音/B站/YouTube/快手/小红书/微博等\n");
        char line[MAX_URL_LEN];
        while (url_count < MAX_URLS) {
            printf("> ");
            if (!fgets(line, sizeof(line), stdin)) break;
            line[strcspn(line, "\n")] = '\0';
            line[strcspn(line, "\r")] = '\0';
            char* p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\0') break;
            strncpy(g_urls[url_count], p, MAX_URL_LEN - 1);
            g_urls[url_count][MAX_URL_LEN - 1] = '\0';
            url_count++;
        }
        if (url_count == 0) { printf("输入为空，退出。\n"); return 1; }
    }

    char cwd[MAX_PATH_LEN];
    getcwd(cwd, sizeof(cwd));
    if (!output_dir[0]) {
        const char* user_profile = getenv("USERPROFILE");
        if (user_profile && user_profile[0])
            snprintf(output_dir, sizeof(output_dir), "%s\\Videos\\aipc", user_profile);
        else
            strncpy(output_dir, "aipc_downloads", sizeof(output_dir) - 1);
    }
    normalize_path(output_dir);
    /* 转为绝对路径，保证 cookie / 输出位置一致 */
    char out_abs[MAX_PATH_LEN] = {0};
    if (output_dir[0]) {
        if (!_fullpath(out_abs, output_dir, MAX_PATH_LEN)) strncpy(out_abs, output_dir, sizeof(out_abs) - 1);
    } else {
        strncpy(out_abs, cwd, sizeof(out_abs) - 1);
    }
    strncpy(output_dir, out_abs, sizeof(output_dir) - 1);

    if (_access(output_dir, 0) != 0) mkdir_recursive(output_dir);
    if (!quiet) printf("\n📁 下载目录: %s\n", output_dir);
    if (!quiet && url_count > 1) printf("📋 本次共 %d 个链接\n", url_count);

    /* Cookie 只需准备一次（下载目录已固定） */
    int cookie_valid = ensure_cookie(output_dir, cookie_path[0] ? cookie_path : NULL);
    if (cookie_valid) {
        if (!quiet) printf("✅ Cookie 有效\n");
    } else {
        if (!quiet) printf("⚠️  无有效 Cookie（仅支持公开视频，或请先登录抖音）\n");
    }

    if (dry_run) { printf("\n[dry-run] 跳过下载\n"); return 0; }

    /* ===== 逐条下载 ===== */
    int ok = 0, fail = 0, skipped = 0;
    for (int idx = 0; idx < url_count; idx++) {
        char cur[MAX_URL_LEN];
        strncpy(cur, g_urls[idx], sizeof(cur) - 1);
        cur[sizeof(cur) - 1] = '\0';

        if (url_count > 1 && !quiet)
            printf("\n================ [%d/%d] ================\n", idx + 1, url_count);
        if (!quiet) printf("[原始] %s\n", cur);
        convert_url(cur);
        if (strstr(cur, "http") == NULL && strstr(cur, "://") == NULL) {
            printf("❌ 第 %d 条未找到有效链接，跳过\n", idx + 1);
            fail++;
            continue;
        }
        if (!quiet) printf("[目标] %s\n", cur);

        /* 去重检测 */
        if (is_duplicate(output_dir, cur)) {
            printf("⏭️  第 %d 条：视频已存在，跳过\n", idx + 1);
            skipped++;
            continue;
        }

        int is_douyin = (strstr(cur, "douyin.com") != NULL);
        int success = 0;
        for (int attempt = 0; attempt <= retry_count; attempt++) {
            if (attempt > 0) {
                if (!quiet) printf("\n[重试] 第 %d/%d 次，等待 %d 秒后重试...\n", attempt, retry_count, retry_delay);
                Sleep((DWORD)(retry_delay * 1000));
            }
            if (is_douyin) {
                if (!quiet) printf(no_watermark ? "\n--- 无水印模式：使用专用通道 ---\n" : "\n--- 抖音专用下载通道 ---\n");
                success = run_python_helper(cur, output_dir, no_playlist, quality, no_watermark);
            } else {
                if (!quiet) printf("\n--- 尝试 yt-dlp ---\n");
                success = run_ytdlp(cur, output_dir, no_playlist, quality);
            }
            if (success) break;
        }
        if (success) { ok++; record_downloaded(output_dir, cur); } else fail++;
    }

    printf("\n");
    if (url_count > 1) printf("全部完成: 成功 %d / %d  (跳过 %d)\n", ok, url_count, skipped);
    if (ok > 0) printf("✅ 保存位置: %s\n", output_dir);
    if (fail > 0) {
        printf("❌ 有 %d 条失败\n", fail);
        printf("   提示: 抖音请确保已登录并可用 --refresh-cookie; 其他平台依赖 yt-dlp。\n");
    }
    return (ok > 0) ? 0 : 1;
}
