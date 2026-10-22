#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_IE
#define _WIN32_IE 0x0600
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ID_PRESET_PREMIUM = 101,
    ID_PRESET_WARM = 102,
    ID_PRESET_STOCK = 103,
    ID_PRESET_SPATIAL = 104,
    ID_SLIDER_INTENSITY = 201,
    ID_SLIDER_REAR = 202,
    ID_SLIDER_POSITION = 203,
    ID_CHK_UPNP = 204,
    ID_BTN_LAUNCH = 301,
    ID_BTN_PLUGIN = 302,
    ID_BTN_OPEN_LOG = 303,
    ID_BTN_OPEN_MUSIC = 304
};

typedef struct {
    const char *name;
    const char *key;
    const char *summary;
} PresetInfo;

static const PresetInfo kPresets[4] = {
    { "Premium",     "premium", "Balanced 4.1 cabin: crisp front dash/door speakers, rear sleeper speakers & under-seat bass" },
    { "Warm",        "warm",    "Rich late-night cruiser with fuller rear cabin speakers and deep under-seat warmth" },
    { "Stock Radio", "stock",   "Front-focused factory door speakers with subtle rear cabin bounce" },
    { "Spatial",     "spatial", "Full 360-degree surround cabin stage with strong rear sleeper speakers & wide binaural imaging" }
};

static int g_preset_index = 0;
static float g_intensity = 0.72f;
static float g_rear = 0.75f;
static float g_position = 0.12f;
static bool g_upnp_enabled = false;
static char g_game_exe_path[MAX_PATH] = "";

static HWND g_hwnd_preset_btns[4];
static HWND g_hwnd_preset_summary = NULL;
static HWND g_hwnd_intensity_val = NULL;
static HWND g_hwnd_intensity_slider = NULL;
static HWND g_hwnd_rear_val = NULL;
static HWND g_hwnd_rear_slider = NULL;
static HWND g_hwnd_position_val = NULL;
static HWND g_hwnd_position_slider = NULL;
static HWND g_hwnd_status = NULL;
static HWND g_hwnd_chk_upnp = NULL;
static HWND g_hwnd_btn_plugin = NULL;

static HFONT g_font_title = NULL;
static HFONT g_font_subtitle = NULL;
static HFONT g_font_section = NULL;
static HFONT g_font_body = NULL;
static HFONT g_font_caption = NULL;
static HBRUSH g_bg_brush = NULL;
static HICON g_app_icon = NULL;

static void get_data_dir(char *buf, size_t size) {
    const char *local = getenv("LOCALAPPDATA");
    if (!local || !*local) local = getenv("USERPROFILE");
    if (!local || !*local) local = "C:\\Temp";
    snprintf(buf, size, "%s\\ETS2 Cabin Radio", local);
    CreateDirectoryA(buf, NULL);
}

static void get_dll_path(char *buf, size_t size) {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    char *slash = strrchr(exe_path, '\\');
    if (slash) *slash = '\0';
    snprintf(buf, size, "%s\\ETS2CabinDSP.dll", exe_path);
}

static void write_live_settings_file(void) {
    char folder[MAX_PATH];
    char file_path[MAX_PATH];
    get_data_dir(folder, sizeof(folder));
    snprintf(file_path, sizeof(file_path), "%s\\live-settings.env", folder);

    FILE *f = fopen(file_path, "wb");
    if (!f) return;
    fprintf(f, "preset=%s\nintensity=%.4f\nrear=%.4f\nposition=%.4f\nupnp=%d\n",
            kPresets[g_preset_index].key, g_intensity, g_rear, g_position, g_upnp_enabled ? 1 : 0);
    fclose(f);
}

static void save_registry_settings(void) {
    HKEY key;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\ETS2CabinRadio", 0, NULL,
                        0, KEY_WRITE, NULL, &key, NULL) == ERROR_SUCCESS) {
        const char *preset_key = kPresets[g_preset_index].key;
        RegSetValueExA(key, "Preset", 0, REG_SZ, (const BYTE *)preset_key, (DWORD)strlen(preset_key) + 1);
        DWORD int_pct = (DWORD)(g_intensity * 1000.0f + 0.5f);
        DWORD rear_pct = (DWORD)(g_rear * 1000.0f + 0.5f);
        DWORD pos_raw = (DWORD)((g_position + 1.0f) * 1000.0f + 0.5f);
        RegSetValueExA(key, "IntensityPermille", 0, REG_DWORD, (const BYTE *)&int_pct, sizeof(DWORD));
        RegSetValueExA(key, "RearPermille", 0, REG_DWORD, (const BYTE *)&rear_pct, sizeof(DWORD));
        RegSetValueExA(key, "PositionPermille", 0, REG_DWORD, (const BYTE *)&pos_raw, sizeof(DWORD));
        DWORD upnp_dw = g_upnp_enabled ? 1 : 0;
        RegSetValueExA(key, "UpnpEnabled", 0, REG_DWORD, (const BYTE *)&upnp_dw, sizeof(DWORD));
        if (g_game_exe_path[0]) {
            RegSetValueExA(key, "GameExePath", 0, REG_SZ,
                           (const BYTE *)g_game_exe_path, (DWORD)strlen(g_game_exe_path) + 1);
        }
        RegCloseKey(key);
    }
    write_live_settings_file();
}

static void load_registry_settings(void) {
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\ETS2CabinRadio", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        char preset_buf[32] = "";
        DWORD sz = sizeof(preset_buf);
        if (RegQueryValueExA(key, "Preset", NULL, NULL, (BYTE *)preset_buf, &sz) == ERROR_SUCCESS) {
            for (int i = 0; i < 4; i++) {
                if (_stricmp(preset_buf, kPresets[i].key) == 0) {
                    g_preset_index = i;
                    break;
                }
            }
        }
        DWORD val = 0;
        sz = sizeof(DWORD);
        if (RegQueryValueExA(key, "IntensityPermille", NULL, NULL, (BYTE *)&val, &sz) == ERROR_SUCCESS) {
            float v = (float)val / 1000.0f;
            if (v >= 0.2f && v <= 1.0f) g_intensity = v;
        }
        sz = sizeof(DWORD);
        if (RegQueryValueExA(key, "RearPermille", NULL, NULL, (BYTE *)&val, &sz) == ERROR_SUCCESS) {
            float v = (float)val / 1000.0f;
            if (v >= 0.0f && v <= 1.0f) g_rear = v;
        }
        sz = sizeof(DWORD);
        if (RegQueryValueExA(key, "PositionPermille", NULL, NULL, (BYTE *)&val, &sz) == ERROR_SUCCESS) {
            float v = ((float)val / 1000.0f) - 1.0f;
            if (v >= -1.0f && v <= 1.0f) g_position = v;
        }
        sz = sizeof(DWORD);
        if (RegQueryValueExA(key, "UpnpEnabled", NULL, NULL, (BYTE *)&val, &sz) == ERROR_SUCCESS) {
            g_upnp_enabled = (val != 0);
        } else {
            g_upnp_enabled = false;
        }
        sz = sizeof(g_game_exe_path);
        RegQueryValueExA(key, "GameExePath", NULL, NULL, (BYTE *)g_game_exe_path, &sz);
        RegCloseKey(key);
    }
    write_live_settings_file();
}

static bool file_exists(const char *path) {
    if (!path || !*path) return false;
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

static bool find_ets2_in_library(const char *steam_root, char *out_exe, size_t out_size) {
    snprintf(out_exe, out_size,
             "%s\\steamapps\\common\\Euro Truck Simulator 2\\bin\\win_x64\\eurotrucks2.exe",
             steam_root);
    return file_exists(out_exe);
}

static bool locate_ets2_executable(char *out_exe, size_t out_size) {
    if (file_exists(g_game_exe_path)) {
        snprintf(out_exe, out_size, "%s", g_game_exe_path);
        return true;
    }

    char steam_path[MAX_PATH] = "";
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD sz = sizeof(steam_path);
        RegQueryValueExA(key, "SteamPath", NULL, NULL, (BYTE *)steam_path, &sz);
        RegCloseKey(key);
    }
    for (char *p = steam_path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    if (steam_path[0]) {
        if (find_ets2_in_library(steam_path, out_exe, out_size)) {
            snprintf(g_game_exe_path, sizeof(g_game_exe_path), "%s", out_exe);
            return true;
        }

        char vdf_path[MAX_PATH];
        snprintf(vdf_path, sizeof(vdf_path), "%s\\steamapps\\libraryfolders.vdf", steam_path);
        FILE *vdf = fopen(vdf_path, "r");
        if (vdf) {
            char line[512];
            while (fgets(line, sizeof(line), vdf)) {
                char *path_tag = strstr(line, "\"path\"");
                if (!path_tag) continue;
                char *q1 = strchr(path_tag + 6, '"');
                if (!q1) continue;
                char *q2 = strchr(q1 + 1, '"');
                if (!q2) continue;
                *q2 = '\0';
                char lib_root[MAX_PATH];
                char *dst = lib_root;
                for (char *src = q1 + 1; *src && (dst - lib_root) < MAX_PATH - 1; src++) {
                    if (*src == '\\' && src[1] == '\\') src++;
                    *dst++ = *src;
                }
                *dst = '\0';
                if (find_ets2_in_library(lib_root, out_exe, out_size)) {
                    fclose(vdf);
                    snprintf(g_game_exe_path, sizeof(g_game_exe_path), "%s", out_exe);
                    return true;
                }
            }
            fclose(vdf);
        }
    }

    for (char drive = 'C'; drive <= 'Z'; drive++) {
        char candidate[MAX_PATH];
        snprintf(candidate, sizeof(candidate), "%c:\\SteamLibrary", drive);
        if (find_ets2_in_library(candidate, out_exe, out_size)) {
            snprintf(g_game_exe_path, sizeof(g_game_exe_path), "%s", out_exe);
            return true;
        }
        snprintf(candidate, sizeof(candidate), "%c:\\Steam", drive);
        if (find_ets2_in_library(candidate, out_exe, out_size)) {
            snprintf(g_game_exe_path, sizeof(g_game_exe_path), "%s", out_exe);
            return true;
        }
        snprintf(candidate, sizeof(candidate), "%c:\\Program Files (x86)\\Steam", drive);
        if (find_ets2_in_library(candidate, out_exe, out_size)) {
            snprintf(g_game_exe_path, sizeof(g_game_exe_path), "%s", out_exe);
            return true;
        }
    }

    return false;
}

static bool prompt_for_ets2_exe(HWND owner, char *out_exe, size_t out_size) {
    char file_buf[MAX_PATH] = "";
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = "Euro Truck Simulator 2 (eurotrucks2.exe)\0eurotrucks2.exe\0Executables (*.exe)\0*.exe\0";
    ofn.lpstrFile = file_buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Locate eurotrucks2.exe (in bin\\win_x64)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameA(&ofn)) {
        snprintf(out_exe, out_size, "%s", file_buf);
        snprintf(g_game_exe_path, sizeof(g_game_exe_path), "%s", file_buf);
        save_registry_settings();
        return true;
    }
    return false;
}

static DWORD find_running_ets2_pid(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, "eurotrucks2.exe") == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static bool is_module_loaded_in_process(DWORD pid, const char *dll_name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    bool found = false;
    if (Module32First(snap, &me)) {
        do {
            if (_stricmp(me.szModule, dll_name) == 0) {
                found = true;
                break;
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

static bool inject_dll_into_pid(DWORD pid, const char *dll_path, char *err_msg, size_t err_size) {
    if (is_module_loaded_in_process(pid, "ETS2CabinDSP.dll")) {
        return true;
    }

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) {
        snprintf(err_msg, err_size, "Could not open eurotrucks2.exe (error %lu).", GetLastError());
        return false;
    }

    size_t path_len = strlen(dll_path) + 1;
    void *remote_mem = VirtualAllocEx(proc, NULL, path_len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_mem) {
        snprintf(err_msg, err_size, "VirtualAllocEx failed (error %lu).", GetLastError());
        CloseHandle(proc);
        return false;
    }

    if (!WriteProcessMemory(proc, remote_mem, dll_path, path_len, NULL)) {
        snprintf(err_msg, err_size, "WriteProcessMemory failed (error %lu).", GetLastError());
        VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    LPTHREAD_START_ROUTINE load_lib =
        (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "LoadLibraryA");

    HANDLE thread = CreateRemoteThread(proc, NULL, 0, load_lib, remote_mem, 0, NULL);
    if (!thread) {
        snprintf(err_msg, err_size, "CreateRemoteThread failed (error %lu).", GetLastError());
        VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE);
    CloseHandle(proc);
    return true;
}

static bool get_scs_plugin_path(char *plugin_path, size_t size) {
    char exe_path[MAX_PATH];
    if (!locate_ets2_executable(exe_path, sizeof(exe_path))) return false;
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s", exe_path);
    char *slash = strrchr(dir, '\\');
    if (!slash) return false;
    *slash = '\0';
    snprintf(plugin_path, size, "%s\\plugins\\ETS2CabinDSP.dll", dir);
    return true;
}

static void update_plugin_button_label(void) {
    if (!g_hwnd_btn_plugin) return;
    char plugin_path[MAX_PATH];
    if (get_scs_plugin_path(plugin_path, sizeof(plugin_path)) && file_exists(plugin_path)) {
        SetWindowTextA(g_hwnd_btn_plugin, "Remove Steam Auto-Plugin");
    } else {
        SetWindowTextA(g_hwnd_btn_plugin, "Install Steam Auto-Plugin");
    }
}

static void update_labels(void) {
    for (int i = 0; i < 4; i++) {
        SendMessageA(g_hwnd_preset_btns[i], BM_SETCHECK,
                     (i == g_preset_index) ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    SetWindowTextA(g_hwnd_preset_summary, kPresets[g_preset_index].summary);

    char num_buf[32];
    snprintf(num_buf, sizeof(num_buf), "%d%%", (int)(g_intensity * 100.0f + 0.5f));
    SetWindowTextA(g_hwnd_intensity_val, num_buf);

    snprintf(num_buf, sizeof(num_buf), "%d%%", (int)(g_rear * 100.0f + 0.5f));
    SetWindowTextA(g_hwnd_rear_val, num_buf);

    const char *pos_label = "Centered";
    if (g_position < -0.15f) pos_label = "Left of driver";
    else if (g_position > 0.15f) pos_label = "Right of driver";
    SetWindowTextA(g_hwnd_position_val, pos_label);
}

static void launch_or_attach_ets2(HWND hwnd) {
    save_registry_settings();

    char dll_path[MAX_PATH];
    get_dll_path(dll_path, sizeof(dll_path));
    if (!file_exists(dll_path)) {
        SetWindowTextA(g_hwnd_status, "ETS2CabinDSP.dll is missing next to ETS2 Cabin Radio.exe.");
        return;
    }

    DWORD running_pid = find_running_ets2_pid();
    if (running_pid != 0) {
        char err[160] = "";
        if (inject_dll_into_pid(running_pid, dll_path, err, sizeof(err))) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Attached Live 4.1 %s DSP to running ETS2 (PID %lu). Sliders hot-reload in real time.",
                     kPresets[g_preset_index].name, (unsigned long)running_pid);
            SetWindowTextA(g_hwnd_status, msg);
        } else {
            SetWindowTextA(g_hwnd_status, err);
        }
        return;
    }

    char exe_path[MAX_PATH];
    if (!locate_ets2_executable(exe_path, sizeof(exe_path))) {
        if (!prompt_for_ets2_exe(hwnd, exe_path, sizeof(exe_path))) {
            SetWindowTextA(g_hwnd_status, "ETS2 launch canceled (eurotrucks2.exe not selected).");
            return;
        }
    }

    char work_dir[MAX_PATH];
    snprintf(work_dir, sizeof(work_dir), "%s", exe_path);
    char *slash = strrchr(work_dir, '\\');
    if (slash) *slash = '\0';

    SetEnvironmentVariableA("ETS2_CABIN_PRESET", kPresets[g_preset_index].key);
    char num_buf[32];
    snprintf(num_buf, sizeof(num_buf), "%.4f", g_intensity);
    SetEnvironmentVariableA("ETS2_CABIN_INTENSITY", num_buf);
    snprintf(num_buf, sizeof(num_buf), "%.4f", g_rear);
    SetEnvironmentVariableA("ETS2_CABIN_REAR", num_buf);
    snprintf(num_buf, sizeof(num_buf), "%.4f", g_position);
    SetEnvironmentVariableA("ETS2_CABIN_POSITION", num_buf);
    SetEnvironmentVariableA("SteamAppId", "227300");
    SetEnvironmentVariableA("SteamGameId", "227300");

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    if (!CreateProcessA(exe_path, NULL, NULL, NULL, FALSE, 0, NULL, work_dir, &si, &pi)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Could not launch eurotrucks2.exe (error %lu).", GetLastError());
        SetWindowTextA(g_hwnd_status, msg);
        return;
    }

    char err[160] = "";
    bool ok = inject_dll_into_pid(pi.dwProcessId, dll_path, err, sizeof(err));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (ok) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "ETS2 launched with live 4.1 %s DSP & 360-degree camera head-tracking.",
                 kPresets[g_preset_index].name);
        SetWindowTextA(g_hwnd_status, msg);
    } else {
        SetWindowTextA(g_hwnd_status, err);
    }
    update_plugin_button_label();
}

static void toggle_scs_plugin(HWND hwnd) {
    char exe_path[MAX_PATH];
    if (!locate_ets2_executable(exe_path, sizeof(exe_path))) {
        if (!prompt_for_ets2_exe(hwnd, exe_path, sizeof(exe_path))) return;
    }

    char plugin_dir[MAX_PATH];
    snprintf(plugin_dir, sizeof(plugin_dir), "%s", exe_path);
    char *slash = strrchr(plugin_dir, '\\');
    if (!slash) return;
    *slash = '\0';
    strncat(plugin_dir, "\\plugins", sizeof(plugin_dir) - strlen(plugin_dir) - 1);

    char plugin_path[MAX_PATH];
    snprintf(plugin_path, sizeof(plugin_path), "%s\\ETS2CabinDSP.dll", plugin_dir);

    if (file_exists(plugin_path)) {
        if (DeleteFileA(plugin_path)) {
            SetWindowTextA(g_hwnd_status, "Removed Steam auto-load plugin from bin\\win_x64\\plugins.");
        } else {
            SetWindowTextA(g_hwnd_status, "Could not remove plugin (close ETS2 first if it is running).");
        }
    } else {
        char dll_path[MAX_PATH];
        get_dll_path(dll_path, sizeof(dll_path));
        CreateDirectoryA(plugin_dir, NULL);
        if (CopyFileA(dll_path, plugin_path, FALSE)) {
            SetWindowTextA(g_hwnd_status,
                           "Installed ETS2CabinDSP.dll to bin\\win_x64\\plugins! ETS2 will now load it automatically from Steam.");
        } else {
            SetWindowTextA(g_hwnd_status, "Could not copy ETS2CabinDSP.dll to bin\\win_x64\\plugins.");
        }
    }
    update_plugin_button_label();
}

static void open_dsp_log(void) {
    char folder[MAX_PATH];
    char log_path[MAX_PATH];
    get_data_dir(folder, sizeof(folder));
    snprintf(log_path, sizeof(log_path), "%s\\live-dsp.log", folder);
    if (file_exists(log_path)) {
        ShellExecuteA(NULL, "open", log_path, NULL, NULL, SW_SHOWNORMAL);
    } else {
        SetWindowTextA(g_hwnd_status, "The live DSP log will appear after the first ETS2 launch.");
    }
}

static void open_music_folder(void) {
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, docs))) {
        char music_dir[MAX_PATH];
        snprintf(music_dir, sizeof(music_dir), "%s\\Euro Truck Simulator 2\\music", docs);
        CreateDirectoryA(music_dir, NULL);
        ShellExecuteA(NULL, "open", music_dir, NULL, NULL, SW_SHOWNORMAL);
    }
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE: {
        g_font_title    = CreateFontA(-24, 0, 0, 0, FW_BOLD,     FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        g_font_subtitle = CreateFontA(-14, 0, 0, 0, FW_NORMAL,   FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        g_font_section  = CreateFontA(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        g_font_body     = CreateFontA(-14, 0, 0, 0, FW_NORMAL,   FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        g_font_caption  = CreateFontA(-12, 0, 0, 0, FW_NORMAL,   FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");

        HINSTANCE inst = ((LPCREATESTRUCT)lparam)->hInstance;

        HWND icon_ctrl = CreateWindowExA(0, "STATIC", NULL, WS_CHILD | WS_VISIBLE | SS_ICON,
                                         24, 18, 48, 48, hwnd, NULL, inst, NULL);
        if (g_app_icon) SendMessageA(icon_ctrl, STM_SETICON, (WPARAM)g_app_icon, 0);

        HWND title = CreateWindowExA(0, "STATIC", "ETS2 Cabin Radio", WS_CHILD | WS_VISIBLE,
                                     84, 16, 400, 30, hwnd, NULL, inst, NULL);
        SendMessageA(title, WM_SETFONT, (WPARAM)g_font_title, TRUE);

        HWND subtitle = CreateWindowExA(0, "STATIC", "3D 4.1 multi-speaker cabin processor with 360-degree head-tracking",
                                        WS_CHILD | WS_VISIBLE, 84, 46, 520, 22, hwnd, NULL, inst, NULL);
        SendMessageA(subtitle, WM_SETFONT, (WPARAM)g_font_subtitle, TRUE);

        /* Sound Profile Card */
        HWND grp_sound = CreateWindowExA(0, "BUTTON", "Sound profile (Front Dash/Door + Rear Sleeper + Sub)",
                                         WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                         24, 80, 636, 256, hwnd, NULL, inst, NULL);
        SendMessageA(grp_sound, WM_SETFONT, (WPARAM)g_font_section, TRUE);

        for (int i = 0; i < 4; i++) {
            g_hwnd_preset_btns[i] = CreateWindowExA(
                0, "BUTTON", kPresets[i].name,
                WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE | (i == 0 ? WS_GROUP : 0),
                42 + i * 150, 108, 142, 32,
                hwnd, (HMENU)(intptr_t)(ID_PRESET_PREMIUM + i), inst, NULL);
            SendMessageA(g_hwnd_preset_btns[i], WM_SETFONT, (WPARAM)g_font_body, TRUE);
        }

        g_hwnd_preset_summary = CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_VISIBLE,
                                                42, 146, 596, 20, hwnd, NULL, inst, NULL);
        SendMessageA(g_hwnd_preset_summary, WM_SETFONT, (WPARAM)g_font_body, TRUE);

        /* Cabin effect slider */
        HWND lbl_int = CreateWindowExA(0, "STATIC", "Cabin effect", WS_CHILD | WS_VISIBLE,
                                       42, 174, 160, 20, hwnd, NULL, inst, NULL);
        SendMessageA(lbl_int, WM_SETFONT, (WPARAM)g_font_body, TRUE);

        g_hwnd_intensity_val = CreateWindowExA(0, "STATIC", "72%", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                               560, 174, 78, 20, hwnd, NULL, inst, NULL);
        SendMessageA(g_hwnd_intensity_val, WM_SETFONT, (WPARAM)g_font_body, TRUE);

        g_hwnd_intensity_slider = CreateWindowExA(0, TRACKBAR_CLASSA, "",
                                                  WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                                  38, 194, 604, 26,
                                                  hwnd, (HMENU)ID_SLIDER_INTENSITY, inst, NULL);
        SendMessageA(g_hwnd_intensity_slider, TBM_SETRANGE, TRUE, MAKELPARAM(20, 100));
        SendMessageA(g_hwnd_intensity_slider, TBM_SETPOS, TRUE, (LPARAM)(int)(g_intensity * 100.0f + 0.5f));

        /* Rear / sleeper speakers slider */
        HWND lbl_rear = CreateWindowExA(0, "STATIC", "Rear / sleeper speakers", WS_CHILD | WS_VISIBLE,
                                        42, 224, 220, 20, hwnd, NULL, inst, NULL);
        SendMessageA(lbl_rear, WM_SETFONT, (WPARAM)g_font_body, TRUE);

        g_hwnd_rear_val = CreateWindowExA(0, "STATIC", "75%", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                          560, 224, 78, 20, hwnd, NULL, inst, NULL);
        SendMessageA(g_hwnd_rear_val, WM_SETFONT, (WPARAM)g_font_body, TRUE);

        g_hwnd_rear_slider = CreateWindowExA(0, TRACKBAR_CLASSA, "",
                                             WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                             38, 244, 604, 26,
                                             hwnd, (HMENU)ID_SLIDER_REAR, inst, NULL);
        SendMessageA(g_hwnd_rear_slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageA(g_hwnd_rear_slider, TBM_SETPOS, TRUE, (LPARAM)(int)(g_rear * 100.0f + 0.5f));

        /* Dashboard position slider */
        HWND lbl_pos = CreateWindowExA(0, "STATIC", "Dashboard position", WS_CHILD | WS_VISIBLE,
                                       42, 274, 180, 20, hwnd, NULL, inst, NULL);
        SendMessageA(lbl_pos, WM_SETFONT, (WPARAM)g_font_body, TRUE);

        g_hwnd_position_val = CreateWindowExA(0, "STATIC", "Centered", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                              490, 274, 148, 20, hwnd, NULL, inst, NULL);
        SendMessageA(g_hwnd_position_val, WM_SETFONT, (WPARAM)g_font_body, TRUE);

        g_hwnd_position_slider = CreateWindowExA(0, TRACKBAR_CLASSA, "",
                                                 WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                                 38, 294, 604, 26,
                                                 hwnd, (HMENU)ID_SLIDER_POSITION, inst, NULL);
        SendMessageA(g_hwnd_position_slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
        SendMessageA(g_hwnd_position_slider, TBM_SETPOS, TRUE, (LPARAM)(int)((g_position + 1.0f) * 100.0f + 0.5f));

        /* Audio Streaming Card */
        HWND grp_stream = CreateWindowExA(0, "BUTTON", "Audio Streaming",
                                          WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                          24, 348, 636, 60, hwnd, NULL, inst, NULL);
        SendMessageA(grp_stream, WM_SETFONT, (WPARAM)g_font_section, TRUE);

        g_hwnd_chk_upnp = CreateWindowExA(
            0, "BUTTON", "Enable UPnP / DLNA Renderer (Stream audio from local network music players).",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            42, 374, 596, 22, hwnd, (HMENU)ID_CHK_UPNP, inst, NULL);
        SendMessageA(g_hwnd_chk_upnp, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        SendMessageA(g_hwnd_chk_upnp, BM_SETCHECK, g_upnp_enabled ? BST_CHECKED : BST_UNCHECKED, 0);

        /* Launch Card */
        HWND grp_live = CreateWindowExA(0, "BUTTON", "Launch / Live Injection",
                                        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                        24, 420, 636, 150, hwnd, NULL, inst, NULL);
        SendMessageA(grp_live, WM_SETFONT, (WPARAM)g_font_section, TRUE);

        HWND live_desc = CreateWindowExA(
            0, "STATIC",
            "Simulates Front Left/Right dash and door speakers, Rear Left/Right sleeper speakers, under-seat sub, and cabin glass reflections - reacting in 360 degrees to camera rotation and windows.",
            WS_CHILD | WS_VISIBLE, 42, 446, 596, 38, hwnd, NULL, inst, NULL);
        SendMessageA(live_desc, WM_SETFONT, (WPARAM)g_font_body, TRUE);

        g_hwnd_status = CreateWindowExA(
            0, "STATIC",
            "Ready. Adjust presets or sliders at any time - even while ETS2 is running.",
            WS_CHILD | WS_VISIBLE, 42, 488, 596, 20, hwnd, NULL, inst, NULL);
        SendMessageA(g_hwnd_status, WM_SETFONT, (WPARAM)g_font_caption, TRUE);

        HWND btn_launch = CreateWindowExA(0, "BUTTON", "Launch / Attach Live DSP",
                                          WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                          42, 516, 195, 38, hwnd, (HMENU)ID_BTN_LAUNCH, inst, NULL);
        SendMessageA(btn_launch, WM_SETFONT, (WPARAM)g_font_section, TRUE);

        g_hwnd_btn_plugin = CreateWindowExA(0, "BUTTON", "Install Steam Auto-Plugin",
                                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                            247, 516, 175, 38, hwnd, (HMENU)ID_BTN_PLUGIN, inst, NULL);
        SendMessageA(g_hwnd_btn_plugin, WM_SETFONT, (WPARAM)g_font_body, TRUE);

        HWND btn_log = CreateWindowExA(0, "BUTTON", "Open DSP Log",
                                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       432, 516, 100, 38, hwnd, (HMENU)ID_BTN_OPEN_LOG, inst, NULL);
        SendMessageA(btn_log, WM_SETFONT, (WPARAM)g_font_body, TRUE);

        HWND btn_music = CreateWindowExA(0, "BUTTON", "Music Folder",
                                         WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                         540, 516, 100, 38, hwnd, (HMENU)ID_BTN_OPEN_MUSIC, inst, NULL);
        SendMessageA(btn_music, WM_SETFONT, (WPARAM)g_font_body, TRUE);

        update_labels();
        update_plugin_button_label();
        return 0;
    }

    case WM_HSCROLL: {
        HWND src = (HWND)lparam;
        if (src == g_hwnd_intensity_slider) {
            int pos = (int)SendMessageA(g_hwnd_intensity_slider, TBM_GETPOS, 0, 0);
            g_intensity = (float)pos / 100.0f;
            update_labels();
            save_registry_settings();
        } else if (src == g_hwnd_rear_slider) {
            int pos = (int)SendMessageA(g_hwnd_rear_slider, TBM_GETPOS, 0, 0);
            g_rear = (float)pos / 100.0f;
            update_labels();
            save_registry_settings();
        } else if (src == g_hwnd_position_slider) {
            int pos = (int)SendMessageA(g_hwnd_position_slider, TBM_GETPOS, 0, 0);
            g_position = ((float)pos / 100.0f) - 1.0f;
            update_labels();
            save_registry_settings();
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wparam);
        if (id >= ID_PRESET_PREMIUM && id <= ID_PRESET_SPATIAL) {
            g_preset_index = id - ID_PRESET_PREMIUM;
            update_labels();
            save_registry_settings();
            return 0;
        }
        switch (id) {
        case ID_CHK_UPNP:
            g_upnp_enabled = (SendMessageA(g_hwnd_chk_upnp, BM_GETCHECK, 0, 0) == BST_CHECKED);
            save_registry_settings();
            if (g_upnp_enabled) {
                SetWindowTextA(g_hwnd_status, "UPnP Renderer enabled: Cast audio from your phone to 'ETS2 Cabin Radio'.");
            } else {
                SetWindowTextA(g_hwnd_status, "UPnP Renderer disabled: Game multimedia radio restored.");
            }
            return 0;
        case ID_BTN_LAUNCH:
            launch_or_attach_ets2(hwnd);
            return 0;
        case ID_BTN_PLUGIN:
            toggle_scs_plugin(hwnd);
            return 0;
        case ID_BTN_OPEN_LOG:
            open_dsp_log();
            return 0;
        case ID_BTN_OPEN_MUSIC:
            open_music_folder();
            return 0;
        }
        break;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wparam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(28, 28, 32));
        return (LRESULT)g_bg_brush;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR cmdline, int show) {
    (void)prev;
    (void)cmdline;

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    load_registry_settings();
    g_bg_brush = CreateSolidBrush(RGB(246, 246, 249));
    g_app_icon = LoadIconA(instance, MAKEINTRESOURCEA(1));

    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hIcon = g_app_icon;
    wc.hIconSm = g_app_icon;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_bg_brush;
    wc.lpszClassName = "ETS2CabinRadioWinClass";
    RegisterClassExA(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = { 0, 0, 684, 608 };
    AdjustWindowRect(&rc, style, FALSE);

    HWND hwnd = CreateWindowExA(
        0, wc.lpszClassName, "ETS2 Cabin Radio", style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, instance, NULL);

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}