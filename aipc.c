#define DY_COOKIE_REFRESH_IMPL
#include "douyin_cookie_refresh.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <windows.h>
#include <direct.h>

// ============ 提取任意 URL ============
void extract_url_from_text(const char* text, char* url_out, size_t size) {
    url_out[0] = '\0';
    const char* p = strstr(text, "https://");
    if (!p) p = strstr(text, "http://");
    if (!p) {
        if (strlen(text) >= 10 && text[0] == 'B' && text[1] == 'V') {
            snprintf(url_out, size, "https://www.bilibili.com/video/%s", text);
            return;
        }
        strncpy(url_out, text, size - 1);
        url_out[size - 1] = '\0';
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

// ============ 转换 URL ============
void convert_url(char* url) {
    char extracted[512];
    extract_url_from_text(url, extracted, sizeof(extracted));
    if (strstr(extracted, "http") == NULL && strstr(extracted, "://") == NULL) {
        return;
    }
    if (strcmp(url, extracted) != 0) {
        printf("[提取] %s\n", extracted);
        strcpy(url, extracted);
    }
    if (strstr(url, "douyin.com") && strstr(url, "jingxuan?modal_id=")) {
        char* pos = strstr(url, "jingxuan?modal_id=");
        char id[64] = {0};
        char* start = pos + 18;
        int i = 0;
        while (start[i] != '\0' && start[i] != '&' && i < 63) {
            id[i] = start[i];
            i++;
        }
        id[i] = '\0';
        char new_url[256];
        snprintf(new_url, sizeof(new_url), "https://www.douyin.com/video/%s", id);
        strcpy(url, new_url);
        printf("[转换] 抖音 -> %s\n", url);
    }
    if (strstr(url, "b23.tv")) {
        printf("[识别] B站短链接\n");
    }
}

// ============ 规范化路径 ============
void normalize_path(char* path) {
    char* start = path;
    while (*start == ' ' || *start == '\t') start++;
    if (start != path) memmove(path, start, strlen(start) + 1);
    size_t len = strlen(path);
    if (len > 0 && (path[len - 1] == '"' || path[len - 1] == '\'')) {
        path[len - 1] = '\0';
        len--;
    }
    if (len > 0 && (path[0] == '"' || path[0] == '\'')) {
        memmove(path, path + 1, len);
        len--;
    }
    for (char* p = path; *p; p++) if (*p == '\\') *p = '/';
    len = strlen(path);
    while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\')) {
        path[len - 1] = '\0';
        len--;
    }
}

// ============ 获取程序所在目录 ============
void get_exe_dir(char* dir, size_t size) {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    char* last_slash = strrchr(exe_path, '\\');
    if (last_slash) *last_slash = '\0';
    strncpy(dir, exe_path, size - 1);
    dir[size - 1] = '\0';
}

// ============ 复制 Cookie 到目标目录 ============
int copy_cookie_to_dir(const char* target_dir) {
    char exe_dir[MAX_PATH];
    char src_path[MAX_PATH], dst_path[MAX_PATH];
    
    get_exe_dir(exe_dir, sizeof(exe_dir));
    snprintf(src_path, sizeof(src_path), "%s/cookies_netscape.txt", exe_dir);
    snprintf(dst_path, sizeof(dst_path), "%s/cookies_netscape.txt", target_dir);
    
    // 检查源文件是否存在
    if (_access(src_path, 0) != 0) {
        // 源没有 Cookie → 尝试提取
        return -1;  // 需要提取
    }
    
    // 检查目标是否已有 Cookie
    int dst_exists = (_access(dst_path, 0) == 0);
    
    if (dst_exists) {
        // 比较文件修改时间，保留新的
        HANDLE h_src = CreateFileA(src_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        HANDLE h_dst = CreateFileA(dst_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h_src != INVALID_HANDLE_VALUE && h_dst != INVALID_HANDLE_VALUE) {
            FILETIME ft_src, ft_dst;
            GetFileTime(h_src, NULL, NULL, &ft_src);
            GetFileTime(h_dst, NULL, NULL, &ft_dst);
            if (CompareFileTime(&ft_src, &ft_dst) <= 0) {
                // 目标更新或相同，不覆盖
                CloseHandle(h_src);
                CloseHandle(h_dst);
                return 1;  // 已存在且较新
            }
        }
        if (h_src != INVALID_HANDLE_VALUE) CloseHandle(h_src);
        if (h_dst != INVALID_HANDLE_VALUE) CloseHandle(h_dst);
    }
    
    // 复制文件
    if (CopyFileA(src_path, dst_path, FALSE)) {
        printf("   📋 Cookie 已复制到: %s\n", target_dir);
        return 1;
    }
    return 0;
}

// ============ 递归创建目录 ============
int mkdir_recursive(const char* path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (tmp[len - 1] == '/' || tmp[len - 1] == '\\') tmp[len - 1] = '\0';
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            _mkdir(tmp);
            *p = '/';
        }
    }
    _mkdir(tmp);
    return 0;
}

// ============ 真实验证 Cookie ============
int verify_cookie_real(void) {
    // 用 yt-dlp 测试一个固定的抖音视频
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "yt-dlp --cookies cookies_netscape.txt --dump-json --playlist-end 1 "
             "\"https://www.douyin.com/video/7663486386041191551\" 2>nul");
    FILE* fp = _popen(cmd, "r");
    if (!fp) return 0;
    char buf[256];
    int valid = 0;
    while (fgets(buf, sizeof(buf), fp)) {
        if (strstr(buf, "\"id\"") || strstr(buf, "\"aweme_id\"")) {
            valid = 1;
            break;
        }
        if (strstr(buf, "Fresh cookies") || strstr(buf, "ERROR") ||
            strstr(buf, "Failed to parse")) {
            valid = 0;
            break;
        }
    }
    _pclose(fp);
    return valid;
}

// ============ 检查 Cookie（文件存在 + 真实验证） ============
int check_cookie_valid(void) {
    // 1. 检查文件是否存在
    FILE* f = fopen("cookies_netscape.txt", "r");
    if (!f) return 0;
    fclose(f);
    
    // 2. 真实验证
    printf("   🔍 验证 Cookie...\n");
    int valid = verify_cookie_real();
    if (valid) {
        printf("   ✅ Cookie 有效\n");
    } else {
        printf("   ❌ Cookie 无效或已过期\n");
    }
    return valid;
}

// ============ 主函数 ============
int main(int argc, char *argv[]) {
    char input[2048] = {0};
    char output_dir[1024] = {0};
    int has_url_arg = 0;
    int has_dir_arg = 0;
    int no_playlist = 0;
    
    // ====== 解析命令行参数 ======
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-playlist") == 0 || strcmp(argv[i], "-np") == 0) {
            no_playlist = 1;
            continue;
        }
        int is_url = (strstr(argv[i], "http://") || strstr(argv[i], "https://") ||
                      strstr(argv[i], "BV") || strstr(argv[i], "b23.tv") ||
                      strstr(argv[i], "douyin.com") || strstr(argv[i], "v.douyin.com"));
        if (!has_url_arg && is_url) {
            strncpy(input, argv[i], sizeof(input) - 1);
            has_url_arg = 1;
        } else if (!has_dir_arg && !is_url) {
            strncpy(output_dir, argv[i], sizeof(output_dir) - 1);
            has_dir_arg = 1;
        } else if (has_url_arg && !has_dir_arg) {
            strncpy(output_dir, argv[i], sizeof(output_dir) - 1);
            has_dir_arg = 1;
        }
    }
    
    // ====== 获取链接 ======
    if (!has_url_arg) {
        printf("\n请输入视频链接或分享文本：\n");
        printf("支持: 抖音/B站/YouTube/快手/小红书/微博等\n");
        printf("> ");
        fgets(input, sizeof(input), stdin);
        input[strcspn(input, "\n")] = '\0';
        input[strcspn(input, "\r")] = '\0';
        if (strlen(input) == 0) { printf("输入为空，退出。\n"); return 1; }
        has_url_arg = 1;
    }
    
    // ====== 获取下载目录 ======
    char cwd[1024];
    getcwd(cwd, sizeof(cwd));
    if (!has_dir_arg || strlen(output_dir) == 0) {
        printf("\n请输入下载目录（留空则下载到当前目录）:\n");
        printf("当前目录: %s\n", cwd);
        printf("> ");
        char dir_input[1024];
        fgets(dir_input, sizeof(dir_input), stdin);
        dir_input[strcspn(dir_input, "\n")] = '\0';
        dir_input[strcspn(dir_input, "\r")] = '\0';
        if (strlen(dir_input) > 0) {
            strncpy(output_dir, dir_input, sizeof(output_dir) - 1);
        } else {
            strncpy(output_dir, cwd, sizeof(output_dir) - 1);
        }
    }
    normalize_path(output_dir);
    if (_access(output_dir, 0) != 0) {
        printf("目录不存在，正在创建: %s\n", output_dir);
        mkdir_recursive(output_dir);
    }
    printf("\n📁 下载目录: %s\n", output_dir);
    
    // ====== 自动复制 Cookie ======
    int cookie_copied = copy_cookie_to_dir(output_dir);
    if (cookie_copied == -1) {
        printf("   📁 程序目录没有 Cookie，需要提取\n");
    } else if (cookie_copied == 1) {
        printf("   ✅ Cookie 已就绪\n");
    }
    
    // ====== 切换到目标目录 ======
    if (_chdir(output_dir) != 0) {
        printf("⚠️ 无法切换到目录，将下载到当前目录\n");
        getcwd(cwd, sizeof(cwd));
        strcpy(output_dir, cwd);
    }
    
    // ====== Cookie 验证 + 自动提取 ======
    int cookie_valid = check_cookie_valid();
    
    if (!cookie_valid) {
        printf("\n⚠️ Cookie 无效或未找到，需要重新提取\n");
        
        // 先尝试从程序目录复制
        char exe_dir[MAX_PATH];
        get_exe_dir(exe_dir, sizeof(exe_dir));
        char src_path[MAX_PATH];
        snprintf(src_path, sizeof(src_path), "%s/cookies_netscape.txt", exe_dir);
        if (_access(src_path, 0) == 0) {
            CopyFileA(src_path, "cookies_netscape.txt", FALSE);
            printf("   📋 已从程序目录复制 Cookie\n");
            cookie_valid = check_cookie_valid();
        }
        
        if (!cookie_valid) {
            printf("是否要自动关闭抖音？(y/n): ");
            char a;
            scanf("%c", &a);
            if (a != 'y' && a != 'Y') {
                printf("请手动关闭抖音后再运行本程序\n");
                return 1;
            }
            system("taskkill /f /im Douyin.exe 2>nul");
            while (getchar() != '\n');
            
            wchar_t wcwd[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, output_dir, -1, wcwd, MAX_PATH);
            
            printf("\n正在提取 Cookie...\n");
            int ret = dy_douyin_refresh(wcwd);
            if (ret != 0) {
                printf("❌ Cookie 提取失败: %s\n", dy_douyin_last_error());
                printf("   继续下载（仅支持公开视频）...\n");
            } else {
                printf("✅ Cookie 提取成功！\n");
                cookie_valid = 1;
            }
            while (getchar() != '\n');
        }
    }
    
    if (cookie_valid) {
        printf("✅ Cookie 有效（抖音/B站高画质可用）\n");
    } else {
        printf("⚠️ 无有效 Cookie（仅支持公开视频）\n");
    }
    
    // ====== 处理 URL ======
    printf("\n[原始] %s\n", input);
    convert_url(input);
    if (strstr(input, "http") == NULL && strstr(input, "://") == NULL) {
        printf("❌ 未找到有效链接\n");
        return 1;
    }
    printf("[最终] %s\n", input);
    
    // ====== 执行下载 ======
    char cmd[3000];
    if (cookie_valid) {
        snprintf(cmd, sizeof(cmd), "yt-dlp --cookies cookies_netscape.txt%s \"%s\"",
                 no_playlist ? " --no-playlist" : "", input);
    } else {
        snprintf(cmd, sizeof(cmd), "yt-dlp%s \"%s\"",
                 no_playlist ? " --no-playlist" : "", input);
    }
    printf("\n[执行] %s\n", cmd);
    
    int result = system(cmd);
    if (result == 0) {
        printf("\n✅ 下载成功！保存在: %s\n", output_dir);
    } else {
        printf("\n❌ 下载失败，错误码: %d\n", result);
    }
    
    return 0;
}