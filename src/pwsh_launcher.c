/**
 * @file src/pwsh_launcher.c
 * @description Zero-dependency native C11 4-OS + WASI PowerShell launcher.
 * Hydrates rolling-latest PowerShell once from awesome-os/b8g-ghost (cas-v1)
 * into ~/.b8g/cas/pwsh/<sha256>/ and forwards the command line verbatim:
 *   - Windows (_WIN32): GetCommandLineW() + CreateProcessW() (zero cmd.exe, zero %* mangling)
 *   - POSIX: execv()
 *   - WASI (__wasi__): --ghost-version, --ghost-info, --help, and local CAS manifest inspection
 */

#if !defined(_WIN32) && !defined(__wasi__) && !defined(_POSIX_C_SOURCE)
  #define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <direct.h>
  #define PATH_SEP '\\'
  #define OS_KEY "win64"
  #define PWSH_REL_BIN "pwsh.exe"
#elif defined(__wasi__)
  #define PATH_SEP '/'
  #define OS_KEY "wasm32-wasi"
  #define PWSH_REL_BIN "pwsh"
#else
  #include <unistd.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #define PATH_SEP '/'
  #if defined(__APPLE__) && defined(__aarch64__)
    #define OS_KEY "macos-arm64"
  #elif defined(__aarch64__)
    #define OS_KEY "linux-arm64"
  #else
    #define OS_KEY "linux-x64"
  #endif
  #define PWSH_REL_BIN "pwsh"
#endif

#define GHOST_PWSH_LAUNCHER_VERSION "0.2.0"
#define INDEX_URL "https://raw.githubusercontent.com/awesome-os/b8g-ghost/main/manifests/index.json"
#define BLOB_BASE_URL "https://github.com/awesome-os/b8g-ghost/releases/download/cas-v1"
#define MAX_PATH_BUF 1024

static int get_cas_dir(char *out, size_t cap) {
  const char *env_dir = getenv("B8G_GHOST_CACHE_DIR");
  if (env_dir && env_dir[0] != '\0') {
    snprintf(out, cap, "%s", env_dir);
    return 0;
  }
#if defined(_WIN32)
  const char *home = getenv("USERPROFILE");
  if (!home || home[0] == '\0') home = getenv("HOME");
  if (!home || home[0] == '\0') return -1;
  snprintf(out, cap, "%s\\.b8g\\cas", home);
#else
  const char *home = getenv("HOME");
  if (!home || home[0] == '\0') {
#if defined(__wasi__)
    snprintf(out, cap, "/.b8g/cas");
    return 0;
#else
    return -1;
#endif
  }
  snprintf(out, cap, "%s/.b8g/cas", home);
#endif
  return 0;
}

static char *read_file_text(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return NULL;
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return NULL;
  }
  long sz = ftell(fp);
  if (sz <= 0 || sz > 16 * 1024 * 1024) {
    fclose(fp);
    return NULL;
  }
  fseek(fp, 0, SEEK_SET);
  char *buf = (char *)malloc((size_t)sz + 1);
  if (!buf) {
    fclose(fp);
    return NULL;
  }
  size_t n = fread(buf, 1, (size_t)sz, fp);
  buf[n] = '\0';
  fclose(fp);
  return buf;
}

static int lookup_sha256_in_index(const char *json, const char *filename, char out_sha[65]) {
  char key[256];
  snprintf(key, sizeof(key), "\"%s\"", filename);
  const char *pos = strstr(json, key);
  if (!pos) return -1;
  const char *sha_key = strstr(pos, "\"sha256\"");
  if (!sha_key) return -1;
  const char *colon = strchr(sha_key + 8, ':');
  if (!colon) return -1;
  const char *q1 = strchr(colon, '"');
  if (!q1) return -1;
  q1++;
  for (int i = 0; i < 64; i++) {
    char c = q1[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return -1;
    }
    out_sha[i] = (c >= 'A' && c <= 'F') ? (char)(c + 32) : c;
  }
  out_sha[64] = '\0';
  return 0;
}

#if !defined(__wasi__)
static int file_exists(const char *path) {
#if defined(_WIN32)
  DWORD attr = GetFileAttributesA(path);
  return (attr != INVALID_FILE_ATTRIBUTES);
#else
  struct stat st;
  return (stat(path, &st) == 0);
#endif
}

static void mkdir_p(const char *dir) {
  char tmp[MAX_PATH_BUF];
  snprintf(tmp, sizeof(tmp), "%s", dir);
  size_t len = strlen(tmp);
  if (len == 0) return;
  if (tmp[len - 1] == '/' || tmp[len - 1] == '\\') tmp[len - 1] = '\0';
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/' || *p == '\\') {
      char saved = *p;
      *p = '\0';
#if defined(_WIN32)
      if (!(p - tmp == 2 && tmp[1] == ':')) _mkdir(tmp);
#else
      mkdir(tmp, 0755);
#endif
      *p = saved;
    }
  }
#if defined(_WIN32)
  _mkdir(tmp);
#else
  mkdir(tmp, 0755);
#endif
}

static int run_cmd(const char *cmd) {
  return system(cmd);
}

static int refresh_index(const char *cas_dir, const char *index_path) {
  mkdir_p(cas_dir);
  char cmd[MAX_PATH_BUF * 2 + 256];
  snprintf(cmd, sizeof(cmd), "curl -fsSL \"%s\" -o \"%s.tmp\"", INDEX_URL, index_path);
  if (run_cmd(cmd) != 0) return -1;
  char tmp_path[MAX_PATH_BUF + 8];
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", index_path);
#if defined(_WIN32)
  MoveFileExA(tmp_path, index_path, MOVEFILE_REPLACE_EXISTING);
#else
  rename(tmp_path, index_path);
#endif
  return 0;
}

static int hydrate_blob(const char *cas_dir, const char *sha, const char *target_dir) {
  mkdir_p(target_dir);
  char tmp_arc[MAX_PATH_BUF + 128];
  snprintf(tmp_arc, sizeof(tmp_arc), "%s%cpwsh-%s.tar.zst", cas_dir, PATH_SEP, sha);

  char cmd[MAX_PATH_BUF * 3 + 256];
  fprintf(stderr, "[awesome-os/powershell] Hydrating cas-v1 blob %.12s...\n", sha);
  snprintf(cmd, sizeof(cmd), "curl -fsSL \"%s/%s\" -o \"%s\"", BLOB_BASE_URL, sha, tmp_arc);
  if (run_cmd(cmd) != 0) {
    remove(tmp_arc);
    return -1;
  }

#if defined(_WIN32)
  snprintf(cmd, sizeof(cmd), "tar -xf \"%s\" -C \"%s\"", tmp_arc, target_dir);
#else
  snprintf(cmd, sizeof(cmd), "tar --zstd -xf \"%s\" -C \"%s\"", tmp_arc, target_dir);
#endif
  int rc = run_cmd(cmd);
  remove(tmp_arc);
  return rc == 0 ? 0 : -1;
}
#endif

#if defined(_WIN32)
static const wchar_t *skip_argv0_wide(const wchar_t *cmdline) {
  const wchar_t *p = cmdline;
  while (*p == L' ' || *p == L'\t') p++;
  if (*p == L'"') {
    p++;
    while (*p && *p != L'"') p++;
    if (*p == L'"') p++;
  } else {
    while (*p && *p != L' ' && *p != L'\t') p++;
  }
  return p;
}

static int exec_pwsh_win32_verbatim(const char *pwsh_bin) {
  int wide_bin_len = MultiByteToWideChar(CP_UTF8, 0, pwsh_bin, -1, NULL, 0);
  if (wide_bin_len <= 0) return 1;
  wchar_t *wide_bin = (wchar_t *)malloc((size_t)wide_bin_len * sizeof(wchar_t));
  if (!wide_bin) return 1;
  MultiByteToWideChar(CP_UTF8, 0, pwsh_bin, -1, wide_bin, wide_bin_len);

  const wchar_t *raw_cmd = GetCommandLineW();
  const wchar_t *tail = skip_argv0_wide(raw_cmd);
  size_t bin_chars = (size_t)(wide_bin_len - 1);
  size_t tail_chars = wcslen(tail);

  /* Build: "<wide_bin>"<verbatim tail> */
  size_t total_chars = 1 + bin_chars + 1 + tail_chars + 1;
  wchar_t *new_cmd = (wchar_t *)malloc(total_chars * sizeof(wchar_t));
  if (!new_cmd) {
    free(wide_bin);
    return 1;
  }

  size_t pos = 0;
  new_cmd[pos++] = L'"';
  memcpy(new_cmd + pos, wide_bin, bin_chars * sizeof(wchar_t));
  pos += bin_chars;
  new_cmd[pos++] = L'"';
  memcpy(new_cmd + pos, tail, (tail_chars + 1) * sizeof(wchar_t));

  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  ZeroMemory(&pi, sizeof(pi));
  si.cb = sizeof(si);

  if (!CreateProcessW(wide_bin, new_cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
    fprintf(stderr, "[awesome-os/powershell] CreateProcessW failed (%lu) for %s\n",
            (unsigned long)GetLastError(), pwsh_bin);
    free(new_cmd);
    free(wide_bin);
    return 127;
  }

  free(new_cmd);
  free(wide_bin);

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return (int)exit_code;
}
#endif

static void print_help(const char *logical_key) {
  printf("@awesome-os/powershell %s - Zero-cmd.exe native Ghost PowerShell launcher\n",
         GHOST_PWSH_LAUNCHER_VERSION);
  printf("Usage:\n");
  printf("  pwsh [powershell-args...]   Forward verbatim to hydrated PowerShell\n");
  printf("  pwsh --ghost-version        Print launcher version and target key\n");
  printf("  pwsh --ghost-info           Inspect CAS cache path and manifest state\n");
  printf("  pwsh --ghost-help           Show this launcher help\n");
  printf("Target: os=%s | key=%s\n", OS_KEY, logical_key);
}

int main(int argc, char **argv) {
  char logical_key[128];
#if defined(__wasi__)
  snprintf(logical_key, sizeof(logical_key), "pwsh-linux-x64.tar.zst");
#else
  snprintf(logical_key, sizeof(logical_key), "pwsh-%s.tar.zst", OS_KEY);
#endif

  if (argc == 2 && strcmp(argv[1], "--ghost-version") == 0) {
    printf("@awesome-os/powershell %s (os=%s, key=%s)\n",
           GHOST_PWSH_LAUNCHER_VERSION, OS_KEY, logical_key);
    return 0;
  }

  if (argc == 2 && (strcmp(argv[1], "--ghost-help") == 0
#if defined(__wasi__)
                    || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0
#endif
                    )) {
    print_help(logical_key);
    return 0;
  }

  char cas_dir[MAX_PATH_BUF];
  if (get_cas_dir(cas_dir, sizeof(cas_dir)) != 0) {
    fprintf(stderr, "[awesome-os/powershell] Could not resolve CAS cache dir (~/.b8g/cas).\n");
    return 1;
  }

  char index_path[MAX_PATH_BUF + 32];
  snprintf(index_path, sizeof(index_path), "%s%cindex.json", cas_dir, PATH_SEP);

  if (argc == 2 && strcmp(argv[1], "--ghost-info") == 0) {
    char cached_sha[65] = "uncached";
    char *json = read_file_text(index_path);
    if (json) {
      if (lookup_sha256_in_index(json, logical_key, cached_sha) != 0) {
        snprintf(cached_sha, sizeof(cached_sha), "missing-in-index");
      }
      free(json);
    }
    printf("@awesome-os/powershell %s - os=%s | key=%s | sha256=%s | cas=%s\n",
           GHOST_PWSH_LAUNCHER_VERSION, OS_KEY, logical_key, cached_sha, cas_dir);
    return 0;
  }

  char sha[65] = {0};
  char *json = read_file_text(index_path);
  int found = (json != NULL && lookup_sha256_in_index(json, logical_key, sha) == 0);
  if (json) free(json);

#if defined(__wasi__)
  if (found) {
    printf("@awesome-os/powershell %s (wasm32-wasi) - manifest entry %s -> sha256=%s\n",
           GHOST_PWSH_LAUNCHER_VERSION, logical_key, sha);
    return 0;
  }
  printf("@awesome-os/powershell %s (wasm32-wasi) - WASI mode supports --ghost-version, --ghost-info, --help, and local CAS manifest inspection (%s)\n",
         GHOST_PWSH_LAUNCHER_VERSION, index_path);
  return 0;
#else
  if (!found) {
    if (refresh_index(cas_dir, index_path) != 0) {
      fprintf(stderr, "[awesome-os/powershell] Failed to fetch cas-v1 index from %s\n", INDEX_URL);
      return 1;
    }
    json = read_file_text(index_path);
    found = (json != NULL && lookup_sha256_in_index(json, logical_key, sha) == 0);
    if (json) free(json);
    if (!found) {
      fprintf(stderr, "[awesome-os/powershell] Entry '%s' not found in cas-v1 index.\n", logical_key);
      return 1;
    }
  }

  char target_dir[MAX_PATH_BUF + 128];
  char pwsh_bin[MAX_PATH_BUF + 160];
  snprintf(target_dir, sizeof(target_dir), "%s%cpwsh%c%s", cas_dir, PATH_SEP, PATH_SEP, sha);
  snprintf(pwsh_bin, sizeof(pwsh_bin), "%s%c%s", target_dir, PATH_SEP, PWSH_REL_BIN);

  if (!file_exists(pwsh_bin)) {
    if (hydrate_blob(cas_dir, sha, target_dir) != 0) {
      fprintf(stderr, "[awesome-os/powershell] Failed to hydrate %s (%s)\n", logical_key, sha);
      return 1;
    }
  }

#if !defined(_WIN32)
  if (!file_exists(pwsh_bin)) {
    snprintf(pwsh_bin, sizeof(pwsh_bin), "%s/bin/%s", target_dir, PWSH_REL_BIN);
  }
#endif

  if (!file_exists(pwsh_bin)) {
    fprintf(stderr, "[awesome-os/powershell] Hydrated binary missing at %s\n", pwsh_bin);
    return 1;
  }

#if defined(_WIN32)
  return exec_pwsh_win32_verbatim(pwsh_bin);
#else
  argv[0] = pwsh_bin;
  execv(pwsh_bin, argv);
  perror("[awesome-os/powershell] execv");
  return 127;
#endif
#endif
}
