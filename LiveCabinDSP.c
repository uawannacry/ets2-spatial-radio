#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


void cabin_log(const char *format, ...);

/*UPnP / DLNA MediaRenderer engine (implemented in upnp.c) */
bool upnp_init(void);
void upnp_shutdown(void);
void upnp_set_enabled(bool enabled);
bool upnp_is_active(void);
bool upnp_is_playing(void);
bool upnp_get_sample(float *out_left, float *out_right);

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

/*
 * ETS2 Cabin Radio — 3D Multi-Speaker Truck/Car Cabin Audio Processor
 * (macOS x86_64 & Windows x64)
 *
 * Simulates a physical 4.1-speaker truck/car cabin around the driver's head:
 *   1. Front Left (FL) door/dash speaker
 *   2. Front Right (FR) passenger dash/door speaker (with cross-cab Haas delay)
 *   3. Rear Left (RL) sleeper/B-pillar surround speaker
 *   4. Rear Right (RR) sleeper/B-pillar surround speaker
 *   5. Under-seat / sleeper subwoofer (< 115 Hz cabin pressure)
 *   6. 4-tap windshield / side-glass / sleeper-wall early reflections
 *
 * Every speaker source is spatialized in real time at 48 kHz using:
 *   - Fractional-sample Binaural ITD (Interaural Time Difference)
 *   - 2-band Binaural Head-Shadowing (low diffraction vs. high shadowing)
 *   - Front/back Pinna directivity cues as you rotate the camera 360 deg
 *   - Asymmetric window-open acoustic bleed (wnd_left, wnd_right, cabin_out)
 */

typedef int FMOD_RESULT;
typedef struct FMOD_STUDIO_SYSTEM FMOD_STUDIO_SYSTEM;
typedef struct FMOD_STUDIO_BUS FMOD_STUDIO_BUS;
typedef struct FMOD_SYSTEM FMOD_SYSTEM;
typedef struct FMOD_CHANNELGROUP FMOD_CHANNELGROUP;
typedef struct FMOD_DSP FMOD_DSP;

enum {
    FMOD_OK = 0,
    FMOD_DSP_TYPE_COMPRESSOR = 16,
    FMOD_DSP_TYPE_THREE_EQ = 25,
    FMOD_CHANNELCONTROL_DSP_TAIL = -3
};

enum {
    EQ_LOWGAIN = 0,
    EQ_MIDGAIN = 1,
    EQ_HIGHGAIN = 2,
    EQ_LOWCROSSOVER = 3,
    EQ_HIGHCROSSOVER = 4,
    EQ_CROSSOVERSLOPE = 5
};

enum {
    COMPRESSOR_THRESHOLD = 0,
    COMPRESSOR_RATIO = 1,
    COMPRESSOR_ATTACK = 2,
    COMPRESSOR_RELEASE = 3,
    COMPRESSOR_GAINMAKEUP = 4,
    COMPRESSOR_LINKED = 6
};

typedef FMOD_RESULT (*FMOD_DSP_READ_CALLBACK)(
    void *dsp_state,
    float *inbuffer,
    float *outbuffer,
    unsigned int length,
    int inchannels,
    int *outchannels
);

/* Exact 0xD8 (216-byte) 64-bit layout of FMOD_DSP_DESCRIPTION in FMOD 2.x. */
typedef struct {
    unsigned int pluginsdkversion;
    char name[32];
    unsigned int version;
    int numinputbuffers;
    int numoutputbuffers;
    void *create;
    void *release;
    void *reset;
    FMOD_DSP_READ_CALLBACK read;
    void *process;
    void *setposition;
    int numparameters;
    void *paramdesc;
    void *setparameterfloat;
    void *setparameterint;
    void *setparameterbool;
    void *setparameterdata;
    void *getparameterfloat;
    void *getparameterint;
    void *getparameterbool;
    void *getparameterdata;
    void *shouldiprocess;
    void *userdata;
    void *sys_register;
    void *sys_deregister;
    void *sys_mix;
} FMOD_DSP_DESCRIPTION;

typedef FMOD_RESULT (*StudioUpdateFn)(FMOD_STUDIO_SYSTEM *);
typedef FMOD_RESULT (*StudioGetBusFn)(FMOD_STUDIO_SYSTEM *, const char *, FMOD_STUDIO_BUS **);
typedef FMOD_RESULT (*StudioGetCoreFn)(FMOD_STUDIO_SYSTEM *, FMOD_SYSTEM **);
typedef FMOD_RESULT (*StudioFlushFn)(FMOD_STUDIO_SYSTEM *);
typedef FMOD_RESULT (*StudioGetParamFn)(FMOD_STUDIO_SYSTEM *, const char *, float *, float *);
typedef FMOD_RESULT (*BusLockFn)(FMOD_STUDIO_BUS *);
typedef FMOD_RESULT (*BusGetGroupFn)(FMOD_STUDIO_BUS *, FMOD_CHANNELGROUP **);
typedef FMOD_RESULT (*CreateDSPByTypeFn)(FMOD_SYSTEM *, int, FMOD_DSP **);
typedef FMOD_RESULT (*CreateDSPFn)(FMOD_SYSTEM *, const FMOD_DSP_DESCRIPTION *, FMOD_DSP **);
typedef FMOD_RESULT (*AddDSPFn)(FMOD_CHANNELGROUP *, int, FMOD_DSP *);
typedef FMOD_RESULT (*SetFloatFn)(FMOD_DSP *, int, float);
typedef FMOD_RESULT (*SetIntFn)(FMOD_DSP *, int, int);
typedef FMOD_RESULT (*ReleaseDSPFn)(FMOD_DSP *);
typedef FMOD_RESULT (*GetVersionFn)(FMOD_SYSTEM *, unsigned int *, unsigned int *);

typedef struct {
    StudioGetBusFn get_bus;
    StudioGetCoreFn get_core;
    StudioFlushFn flush;
    StudioGetParamFn get_param;
    BusLockFn lock_bus;
    BusGetGroupFn get_group;
    CreateDSPByTypeFn create_dsp_by_type;
    CreateDSPFn create_dsp;
    AddDSPFn add_dsp;
    SetFloatFn set_float;
    SetIntFn set_int;
    ReleaseDSPFn release_dsp;
    GetVersionFn get_version;
} CabinAPI;

typedef struct {
    float amount;
    float rear_level;
    float position;
    float low_gain;
    float mid_gain;
    float high_gain;
    float low_crossover;
    float high_crossover;
    float front_gain;
    float rear_gain;
    float sub_gain;
    float reflection_gain;
    float crossfeed;
    float compressor_threshold;
    float compressor_ratio;
} CabinSettings;

/*
 * Target acoustic state updated on the game thread and smoothed sample-by-sample
 * on the FMOD mixer thread for click-free 360-degree head rotation.
 */
typedef struct {
    float delay_L[4];
    float delay_R[4];
    float low_gain_L[4];
    float low_gain_R[4];
    float high_gain_L[4];
    float high_gain_R[4];
    float sub_gain;
    float refl_gain_L;
    float refl_gain_R;
    float dry_blend;
} SpatialTargets;

#define CABIN_DELAY_MASK 4095
#define CABIN_DELAY_SIZE 4096

typedef struct {
    float front_L[CABIN_DELAY_SIZE];
    float front_R[CABIN_DELAY_SIZE];
    float rear_L[CABIN_DELAY_SIZE];
    float rear_R[CABIN_DELAY_SIZE];
    float sub_buf[CABIN_DELAY_SIZE];
    unsigned int write_pos;

    /* One-pole filter states (at 48 kHz) */
    float rear_lp_L;
    float rear_lp_R;
    float spk_lp[4];
    float sub_lp1;
    float sub_lp2;
    float refl_lp_L;
    float refl_lp_R;
    float eq_lp1_L;
    float eq_lp1_R;
    float eq_lp2_L;
    float eq_lp2_R;

    /* Smoothed per-sample parameters */
    SpatialTargets current;
    bool initialized;
} CabinDSPState;

#ifdef _WIN32
static SRWLOCK g_lock = SRWLOCK_INIT;
static void lock_state(void) { AcquireSRWLockExclusive(&g_lock); }
static void unlock_state(void) { ReleaseSRWLockExclusive(&g_lock); }
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock_state(void) { pthread_mutex_lock(&g_lock); }
static void unlock_state(void) { pthread_mutex_unlock(&g_lock); }
#endif

static bool g_symbols_resolved = false;
static bool g_installed = false;
static bool g_disabled = false;
static bool g_dry_run = false;
static bool g_hook_logged = false;
static bool g_turn_logged = false;
static bool g_radio_bus_locked = false;
static FMOD_STUDIO_BUS *g_radio_bus = NULL;
static FMOD_DSP *g_eq_dsp = NULL;
static FMOD_DSP *g_spatial_dsp = NULL;
static FMOD_DSP *g_compressor_dsp = NULL;
static CabinSettings g_settings;
static float g_dyn_low_gain = 0.0f;
static float g_dyn_mid_gain = 0.0f;
static float g_dyn_high_gain = 0.0f;
static bool g_settings_loaded_once = false;
static SpatialTargets g_targets;
static CabinDSPState g_dsp_state;
static float g_last_rot = 999.0f;
static float g_last_out = 999.0f;
static float g_last_wnd_l = 999.0f;
static float g_last_wnd_r = 999.0f;
static unsigned int g_retry_counter = 0;
static CabinAPI g_api;
static FILE *g_log = NULL;
static bool g_upnp_target_enabled = false;
static bool g_upnp_active = false;

#ifdef __APPLE__
extern FMOD_RESULT FMOD_Studio_System_Update(FMOD_STUDIO_SYSTEM *system);
extern FMOD_RESULT FMOD_CPP_Studio_System_Update(FMOD_STUDIO_SYSTEM *system)
    __asm__("__ZN4FMOD6Studio6System6updateEv");
#endif

static float clamp_float(float value, float minimum, float maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static void data_folder_path(char *buffer, size_t size) {
#ifdef _WIN32
    const char *local_appdata = getenv("LOCALAPPDATA");
    if (!local_appdata || !*local_appdata) local_appdata = getenv("USERPROFILE");
    if (!local_appdata || !*local_appdata) local_appdata = "C:\\Temp";
    snprintf(buffer, size, "%s\\ETS2 Cabin Radio", local_appdata);
    CreateDirectoryA(buffer, NULL);
#else
    const char *home = NULL;
    struct passwd *account = getpwuid(getuid());
    if (account) home = account->pw_dir;
    if (!home) home = "/tmp";
    snprintf(buffer, size, "%s/Library/Logs/ETS2 Cabin Radio", home);
    mkdir(buffer, 0755);
#endif
}

static void open_log(void) {
    if (g_log) return;

    const char *override = getenv("ETS2_CABIN_LOG_PATH");
    if (override && *override) {
        g_log = fopen(override, "a");
        if (!g_log) g_log = stderr;
        return;
    }

    char folder[PATH_MAX];
    char file[PATH_MAX];
    data_folder_path(folder, sizeof(folder));
#ifdef _WIN32
    snprintf(file, sizeof(file), "%s\\live-dsp.log", folder);
#else
    snprintf(file, sizeof(file), "%s/live-dsp.log", folder);
#endif
    g_log = fopen(file, "a");
    if (!g_log) g_log = stderr;
}

void cabin_log(const char *format, ...) {
    open_log();
    time_t now = time(NULL);
    struct tm local;
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);
    fprintf(g_log, "[%s] ", timestamp);

    va_list arguments;
    va_start(arguments, format);
    vfprintf(g_log, format, arguments);
    va_end(arguments);
    fputc('\n', g_log);
    fflush(g_log);
}

static void *load_symbol(const char *name) {
#ifdef _WIN32
    static const char *const modules[] = {
        "fmodstudio.dll",
        "fmodstudioL.dll",
        "fmod.dll",
        "fmodL.dll"
    };
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); i++) {
        HMODULE mod = GetModuleHandleA(modules[i]);
        if (mod) {
            FARPROC proc = GetProcAddress(mod, name);
            if (proc) return (void *)proc;
        }
    }
    cabin_log("Missing symbol %s in FMOD DLLs", name);
    return NULL;
#else
    void *symbol = dlsym(RTLD_DEFAULT, name);
    if (!symbol) cabin_log("Missing symbol %s: %s", name, dlerror());
    return symbol;
#endif
}

static bool resolve_symbols(void) {
    if (g_symbols_resolved) return true;

    g_api.get_bus = (StudioGetBusFn)load_symbol("FMOD_Studio_System_GetBus");
    g_api.get_core = (StudioGetCoreFn)load_symbol("FMOD_Studio_System_GetCoreSystem");
    g_api.flush = (StudioFlushFn)load_symbol("FMOD_Studio_System_FlushCommands");
    g_api.get_param = (StudioGetParamFn)load_symbol("FMOD_Studio_System_GetParameterByName");
    g_api.lock_bus = (BusLockFn)load_symbol("FMOD_Studio_Bus_LockChannelGroup");
    g_api.get_group = (BusGetGroupFn)load_symbol("FMOD_Studio_Bus_GetChannelGroup");
    g_api.create_dsp_by_type = (CreateDSPByTypeFn)load_symbol("FMOD_System_CreateDSPByType");
    g_api.create_dsp = (CreateDSPFn)load_symbol("FMOD_System_CreateDSP");
    g_api.add_dsp = (AddDSPFn)load_symbol("FMOD_ChannelGroup_AddDSP");
    g_api.set_float = (SetFloatFn)load_symbol("FMOD_DSP_SetParameterFloat");
    g_api.set_int = (SetIntFn)load_symbol("FMOD_DSP_SetParameterInt");
    g_api.release_dsp = (ReleaseDSPFn)load_symbol("FMOD_DSP_Release");
    g_api.get_version = (GetVersionFn)load_symbol("FMOD_System_GetVersion");

    g_symbols_resolved = g_api.get_bus && g_api.get_core && g_api.flush &&
        g_api.lock_bus && g_api.get_group && g_api.create_dsp_by_type &&
        g_api.create_dsp && g_api.add_dsp && g_api.set_float &&
        g_api.set_int && g_api.release_dsp;
    return g_symbols_resolved;
}

static float environment_float(const char *name, float fallback, float minimum, float maximum) {
    const char *value = getenv(name);
    if (!value || !*value) return fallback;
    char *end = NULL;
    float parsed = strtof(value, &end);
    if (end == value || !isfinite(parsed)) return fallback;
    return clamp_float(parsed, minimum, maximum);
}

static CabinSettings build_settings(const char *preset, float amount, float rear_level, float position) {
    amount = clamp_float(amount, 0.0f, 1.0f);
    rear_level = clamp_float(rear_level, 0.0f, 1.0f);
    position = clamp_float(position, -1.0f, 1.0f);
    if (!preset || !*preset) preset = "premium";

    CabinSettings settings = {
        .amount = amount,
        .rear_level = rear_level,
        .position = position,
        .low_gain = 1.6f * amount,
        .mid_gain = -0.6f * amount,
        .high_gain = -0.8f * amount,
        .low_crossover = 220.0f,
        .high_crossover = 5000.0f,
        .front_gain = 0.56f,
        .rear_gain = (0.28f + 0.34f * amount) * rear_level,
        .sub_gain = 0.24f * amount,
        .reflection_gain = 0.22f * amount,
        .crossfeed = 0.16f + 0.12f * amount,
        .compressor_threshold = -10.0f - (5.0f * amount),
        .compressor_ratio = 1.25f + (1.1f * amount)
    };

    if (strcmp(preset, "warm") == 0) {
        settings.low_gain = 2.6f * amount;
        settings.mid_gain = -0.4f * amount;
        settings.high_gain = -1.8f * amount;
        settings.front_gain = 0.52f;
        settings.rear_gain = (0.34f + 0.38f * amount) * rear_level;
        settings.sub_gain = 0.32f * amount;
        settings.reflection_gain = 0.25f * amount;
    } else if (strcmp(preset, "stock") == 0) {
        settings.low_gain = 2.8f * amount;
        settings.mid_gain = -1.5f * amount;
        settings.high_gain = -2.8f * amount;
        settings.front_gain = 0.62f;
        settings.rear_gain = (0.18f + 0.24f * amount) * rear_level;
        settings.sub_gain = 0.14f * amount;
        settings.reflection_gain = 0.28f * amount;
        settings.compressor_ratio = 1.5f + (1.4f * amount);
    } else if (strcmp(preset, "spatial") == 0) {
        settings.low_gain = 1.5f * amount;
        settings.mid_gain = -0.8f * amount;
        settings.high_gain = -0.6f * amount;
        settings.front_gain = 0.50f;
        settings.rear_gain = (0.38f + 0.44f * amount) * rear_level;
        settings.sub_gain = 0.28f * amount;
        settings.reflection_gain = 0.31f * amount;
        settings.crossfeed = 0.22f + 0.14f * amount;
    }

    cabin_log("Preset=%s intensity=%.2f rear=%.2f position=%.2f",
              preset, amount, rear_level, position);
    return settings;
}

static void live_settings_path(char *buffer, size_t size) {
    char folder[PATH_MAX];
    data_folder_path(folder, sizeof(folder));
#ifdef _WIN32
    snprintf(buffer, size, "%s\\live-settings.env", folder);
#else
    snprintf(buffer, size, "%s/live-settings.env", folder);
#endif
}

static CabinSettings read_settings(void) {
    float amount = environment_float("ETS2_CABIN_INTENSITY", 0.72f, 0.0f, 1.0f);
    float rear_level = environment_float("ETS2_CABIN_REAR", 0.75f, 0.0f, 1.0f);
    float position = environment_float("ETS2_CABIN_POSITION", 0.12f, -1.0f, 1.0f);
    char preset[32] = "premium";
    const char *env_preset = getenv("ETS2_CABIN_PRESET");
    if (env_preset && *env_preset) {
        snprintf(preset, sizeof(preset), "%s", env_preset);
    }

    char path[PATH_MAX];
    live_settings_path(path, sizeof(path));
    FILE *file = fopen(path, "r");
    if (file) {
        char line[128];
        while (fgets(line, sizeof(line), file)) {
            char *nl = strpbrk(line, "\r\n");
            if (nl) *nl = '\0';
            if (strncmp(line, "preset=", 7) == 0 && line[7]) {
                snprintf(preset, sizeof(preset), "%s", line + 7);
            } else if (strncmp(line, "intensity=", 10) == 0) {
                amount = strtof(line + 10, NULL);
            } else if (strncmp(line, "rear=", 5) == 0) {
                rear_level = strtof(line + 5, NULL);
            } else if (strncmp(line, "position=", 9) == 0) {
                position = strtof(line + 9, NULL);
            } else if (strncmp(line, "upnp=", 5) == 0) {
                g_upnp_target_enabled = (atoi(line + 5) != 0);
            }
        }
        fclose(file);
    }

    return build_settings(preset, amount, rear_level, position);
}

/*
 * Fractional sample reader from a 4096-sample circular buffer.
 */
static inline float read_delay_frac(const float *buf, unsigned int write_pos, float delay_samples) {
    if (delay_samples < 1.0f) delay_samples = 1.0f;
    if (delay_samples > (float)(CABIN_DELAY_SIZE - 4)) delay_samples = (float)(CABIN_DELAY_SIZE - 4);
    int d_int = (int)delay_samples;
    float frac = delay_samples - (float)d_int;
    unsigned int idx0 = (write_pos - (unsigned int)d_int) & CABIN_DELAY_MASK;
    unsigned int idx1 = (idx0 - 1u) & CABIN_DELAY_MASK;
    return buf[idx0] + frac * (buf[idx1] - buf[idx0]);
}

static FMOD_RESULT cabin_should_i_process(
    void *dsp_state,
    int inputsidle,
    unsigned int length,
    int inmask,
    int inchannels,
    int speakermode
) {
    (void)dsp_state; (void)inputsidle; (void)length;
    (void)inmask; (void)inchannels; (void)speakermode;
    return FMOD_OK; /* Process continuously even if game radio bus is idle */
}

/*
 * Real-time 48 kHz 3D Multi-Speaker Cabin DSP callback.
 * Simulates Front L/R (dash + door), Rear L/R (sleeper/B-pillar), Under-seat Sub,
 * and 4-tap cabin glass/wall reflections with continuous Binaural ITD & ILD.
 */
static FMOD_RESULT cabin_spatial_dsp_read(
    void *dsp_state,
    float *inbuffer,
    float *outbuffer,
    unsigned int length,
    int inchannels,
    int *outchannels
) {
    (void)dsp_state;
    if (outchannels) *outchannels = inchannels;
    if (!outbuffer || inchannels < 1) return FMOD_OK;

    bool upnp_active = g_upnp_active && upnp_is_playing();

    static bool s_diag_logged = false;
    if (!s_diag_logged) {
        s_diag_logged = true;
        cabin_log("DSP READ ACTIVE: inchannels=%d, upnp_active=%d, inbuf=%p, len=%u",
                  inchannels, upnp_active, inbuffer, length);
    }

    if (!inbuffer && !upnp_active) {
        memset(outbuffer, 0, length * inchannels * sizeof(float));
        return FMOD_OK;
    }

    if (inchannels == 1 && !upnp_active) {
        memcpy(outbuffer, inbuffer, length * sizeof(float));
        return FMOD_OK;
    }

    CabinDSPState *st = &g_dsp_state;
    SpatialTargets tgt = g_targets;
    if (!st->initialized) {
        st->current = tgt;
        st->initialized = true;
    }

    /* Filter coefficients at 48 kHz:
     *   rear_alpha (~3.2 kHz): warm rear sleeper/B-pillar speaker cone roll-off
     *   split_alpha (~850 Hz): separates diffracting low/body from directional highs
     *   sub_alpha (~115 Hz):   under-seat / sleeper subwoofer crossover
     *   refl_alpha (~2.5 kHz): cabin upholstery / glass reflection damping
     */
    const float rear_alpha = 0.34f;
    const float split_alpha = 0.105f;
    const float sub_alpha = 0.015f;
    const float refl_alpha = 0.28f;
    const float smooth = 0.0025f;
    const float cross = g_settings.crossfeed;

    /* Equalizer gain multipliers from dynamic live state (220 Hz / 5000 Hz crossovers at 48 kHz) */
    float eq_g_low  = powf(10.0f, g_dyn_low_gain / 20.0f);
    float eq_g_mid  = powf(10.0f, g_dyn_mid_gain / 20.0f);
    float eq_g_high = powf(10.0f, g_dyn_high_gain / 20.0f);
    const float eq_alpha_low = 0.0284f;  /* ~220 Hz crossover at 48 kHz */
    const float eq_alpha_high = 0.4803f; /* ~5000 Hz crossover at 48 kHz */

    static bool s_flowing_logged = false;
    if (upnp_active && !s_flowing_logged) {
        s_flowing_logged = true;
        cabin_log("UPnP audio flowing into 4.1 cabin DSP graph (inchannels=%d).", inchannels);
    } else if (!upnp_active && s_flowing_logged) {
        s_flowing_logged = false;
    }

    for (unsigned int i = 0; i < length; i++) {
        float in_L = 0.0f;
        float in_R = 0.0f;

        if (upnp_active) {
            upnp_get_sample(&in_L, &in_R);

            /* Apply 3-Band Cabin EQ to UPnP audio (matching g_eq_dsp) */
            st->eq_lp1_L += eq_alpha_low * (in_L - st->eq_lp1_L);
            st->eq_lp1_R += eq_alpha_low * (in_R - st->eq_lp1_R);
            st->eq_lp2_L += eq_alpha_high * (in_L - st->eq_lp2_L);
            st->eq_lp2_R += eq_alpha_high * (in_R - st->eq_lp2_R);

            float low_L = st->eq_lp1_L;
            float low_R = st->eq_lp1_R;
            float mid_L = st->eq_lp2_L - st->eq_lp1_L;
            float mid_R = st->eq_lp2_R - st->eq_lp1_R;
            float high_L = in_L - st->eq_lp2_L;
            float high_R = in_R - st->eq_lp2_R;

            in_L = low_L * eq_g_low + mid_L * eq_g_mid + high_L * eq_g_high;
            in_R = low_R * eq_g_low + mid_R * eq_g_mid + high_R * eq_g_high;
        } else if (inbuffer) {
            in_L = inbuffer[i * inchannels + 0];
            in_R = (inchannels > 1) ? inbuffer[i * inchannels + 1] : in_L;
        }

        /* Smoothly interpolate spatial targets sample-by-sample (click-free head turns). */
        for (int s = 0; s < 4; s++) {
            st->current.delay_L[s]     += smooth * (tgt.delay_L[s]     - st->current.delay_L[s]);
            st->current.delay_R[s]     += smooth * (tgt.delay_R[s]     - st->current.delay_R[s]);
            st->current.low_gain_L[s]  += smooth * (tgt.low_gain_L[s]  - st->current.low_gain_L[s]);
            st->current.low_gain_R[s]  += smooth * (tgt.low_gain_R[s]  - st->current.low_gain_R[s]);
            st->current.high_gain_L[s] += smooth * (tgt.high_gain_L[s] - st->current.high_gain_L[s]);
            st->current.high_gain_R[s] += smooth * (tgt.high_gain_R[s] - st->current.high_gain_R[s]);
        }
        st->current.sub_gain    += smooth * (tgt.sub_gain    - st->current.sub_gain);
        st->current.refl_gain_L += smooth * (tgt.refl_gain_L - st->current.refl_gain_L);
        st->current.refl_gain_R += smooth * (tgt.refl_gain_R - st->current.refl_gain_R);
        st->current.dry_blend   += smooth * (tgt.dry_blend   - st->current.dry_blend);

        unsigned int wp = st->write_pos;

        /* 1. Feed Front Left & Front Right speaker buffers. */
        st->front_L[wp] = in_L;
        st->front_R[wp] = in_R;

        /* 2. Feed Rear Left & Rear Right sleeper/cabin speaker buffers with warm contour + acoustic cross-blend. */
        float rear_src_L = in_L * (1.0f - cross) + in_R * cross;
        float rear_src_R = in_R * (1.0f - cross) + in_L * cross;
        st->rear_lp_L += rear_alpha * (rear_src_L - st->rear_lp_L);
        st->rear_lp_R += rear_alpha * (rear_src_R - st->rear_lp_R);
        st->rear_L[wp] = st->rear_lp_L + 0.55f * (rear_src_L - st->rear_lp_L);
        st->rear_R[wp] = st->rear_lp_R + 0.55f * (rear_src_R - st->rear_lp_R);

        /* 3. Feed Under-Seat / Bunk Subwoofer (2nd-order 115 Hz low-pass). */
        float mono = 0.5f * (in_L + in_R);
        st->sub_lp1 += sub_alpha * (mono - st->sub_lp1);
        st->sub_lp2 += sub_alpha * (st->sub_lp1 - st->sub_lp2);
        st->sub_buf[wp] = st->sub_lp2;

        /* 4. Render all 4 physical speakers (FL=0, FR=1, RL=2, RR=3) with Binaural ITD & 2-band ILD. */
        const float *spk_bufs[4] = { st->front_L, st->front_R, st->rear_L, st->rear_R };
        float ear_L = 0.0f;
        float ear_R = 0.0f;

        for (int s = 0; s < 4; s++) {
            float sample_L = read_delay_frac(spk_bufs[s], wp, st->current.delay_L[s]);
            float sample_R = read_delay_frac(spk_bufs[s], wp, st->current.delay_R[s]);

            /* Update speaker low/body crossover state (~850 Hz). */
            float avg_s = 0.5f * (sample_L + sample_R);
            st->spk_lp[s] += split_alpha * (avg_s - st->spk_lp[s]);
            float low_s = st->spk_lp[s];
            float high_L = sample_L - low_s;
            float high_R = sample_R - low_s;

            ear_L += low_s * st->current.low_gain_L[s] + high_L * st->current.high_gain_L[s];
            ear_R += low_s * st->current.low_gain_R[s] + high_R * st->current.high_gain_R[s];
        }

        /* 5. Add Under-Seat Subwoofer (~2.8 ms propagation delay = 134 samples). */
        float sub_sig = st->sub_buf[(wp - 134u) & CABIN_DELAY_MASK] * st->current.sub_gain;
        ear_L += sub_sig;
        ear_R += sub_sig;

        /* 6. 4-Tap Cabin Early Reflections (Windshield 11.5ms, Side Glass 18.8ms, Roof 28.1ms, Sleeper Wall 41.2ms). */
        float tap1_L = st->front_L[(wp - 552u)  & CABIN_DELAY_MASK];
        float tap1_R = st->front_R[(wp - 576u)  & CABIN_DELAY_MASK];
        float tap2_L = st->front_R[(wp - 902u)  & CABIN_DELAY_MASK];
        float tap2_R = st->front_L[(wp - 936u)  & CABIN_DELAY_MASK];
        float tap3_L = st->rear_L[(wp - 1348u)  & CABIN_DELAY_MASK];
        float tap3_R = st->rear_R[(wp - 1392u)  & CABIN_DELAY_MASK];
        float tap4_L = st->rear_R[(wp - 1940u)  & CABIN_DELAY_MASK];
        float tap4_R = st->rear_L[(wp - 1996u)  & CABIN_DELAY_MASK];

        float raw_refl_L = 0.40f * tap1_L + 0.30f * tap2_L + 0.20f * tap3_L + 0.14f * tap4_L;
        float raw_refl_R = 0.40f * tap1_R + 0.30f * tap2_R + 0.20f * tap3_R + 0.14f * tap4_R;
        st->refl_lp_L += refl_alpha * (raw_refl_L - st->refl_lp_L);
        st->refl_lp_R += refl_alpha * (raw_refl_R - st->refl_lp_R);

        ear_L += st->refl_lp_L * st->current.refl_gain_L;
        ear_R += st->refl_lp_R * st->current.refl_gain_R;

        /* Blend with dry signal when Cabin Effect slider is lowered toward 0. */
        float dry = st->current.dry_blend;
        float wet = 1.0f - dry;
        float out_L = dry * in_L + wet * ear_L;
        float out_R = dry * in_R + wet * ear_R;

        if (inchannels == 1) {
            outbuffer[i] = 0.5f * (out_L + out_R);
        } else {
            outbuffer[i * inchannels + 0] = out_L;
            outbuffer[i * inchannels + 1] = out_R;
            for (int c = 2; c < inchannels; c++) {
                outbuffer[i * inchannels + c] = inbuffer ? inbuffer[i * inchannels + c] : 0.0f;
            }
        }

        st->write_pos = (wp + 1u) & CABIN_DELAY_MASK;
    }

    return FMOD_OK;
}

static FMOD_DSP *create_effect_by_type(FMOD_SYSTEM *core, int type, const char *name) {
    FMOD_DSP *effect = NULL;
    FMOD_RESULT result = g_api.create_dsp_by_type(core, type, &effect);
    if (result != FMOD_OK || !effect) {
        cabin_log("Could not create %s DSP (FMOD result %d)", name, result);
        return NULL;
    }
    return effect;
}

static FMOD_DSP *create_spatial_cabin_dsp(FMOD_SYSTEM *core) {
    static FMOD_DSP_DESCRIPTION desc;
    memset(&desc, 0, sizeof(desc));
    desc.pluginsdkversion = 0x00010000;
    snprintf(desc.name, sizeof(desc.name), "ETS2 3D Cabin Spatializer");
    desc.version = 0x00010200;
    desc.numinputbuffers = 1;
    desc.numoutputbuffers = 1;
    desc.read = cabin_spatial_dsp_read;
    desc.shouldiprocess = cabin_should_i_process;

    FMOD_DSP *dsp = NULL;
    FMOD_RESULT result = g_api.create_dsp(core, &desc, &dsp);
    if (result != FMOD_OK || !dsp) {
        cabin_log("Could not create 3D Cabin Spatializer DSP (FMOD result %d)", result);
        return NULL;
    }
    return dsp;
}

static bool attach_effect(FMOD_CHANNELGROUP *group, FMOD_DSP *effect, const char *name) {
    FMOD_RESULT result = g_api.add_dsp(group, FMOD_CHANNELCONTROL_DSP_TAIL, effect);
    if (result != FMOD_OK) {
        cabin_log("Could not attach %s DSP (FMOD result %d)", name, result);
        g_api.release_dsp(effect);
        return false;
    }
    cabin_log("Attached %s DSP", name);
    return true;
}

static void configure_eq(FMOD_DSP *effect, CabinSettings s) {
    g_dyn_low_gain = s.low_gain;
    g_dyn_mid_gain = s.mid_gain;
    g_dyn_high_gain = s.high_gain;
    if (!effect) return;
    g_api.set_float(effect, EQ_LOWGAIN, s.low_gain);
    g_api.set_float(effect, EQ_MIDGAIN, s.mid_gain);
    g_api.set_float(effect, EQ_HIGHGAIN, s.high_gain);
    g_api.set_float(effect, EQ_LOWCROSSOVER, s.low_crossover);
    g_api.set_float(effect, EQ_HIGHCROSSOVER, s.high_crossover);
    g_api.set_int(effect, EQ_CROSSOVERSLOPE, 1);
}

static void configure_compressor(FMOD_DSP *effect, CabinSettings s) {
    g_api.set_float(effect, COMPRESSOR_THRESHOLD, s.compressor_threshold);
    g_api.set_float(effect, COMPRESSOR_RATIO, s.compressor_ratio);
    g_api.set_float(effect, COMPRESSOR_ATTACK, 20.0f);
    g_api.set_float(effect, COMPRESSOR_RELEASE, 220.0f);
    g_api.set_float(effect, COMPRESSOR_GAINMAKEUP, 0.6f);
    g_api.set_int(effect, COMPRESSOR_LINKED, 1);
}

static float read_studio_param(FMOD_STUDIO_SYSTEM *studio, const char *name, float fallback) {
    if (!g_api.get_param) return fallback;
    float value = fallback;
    float final_value = fallback;
    if (g_api.get_param(studio, name, &value, &final_value) == FMOD_OK && isfinite(final_value)) {
        return final_value;
    }
    return fallback;
}

/*
 * Computes binaural delays (ITD) and 2-band head-shadowing gains (ILD + Pinna)
 * for all 4 cabin speakers (Front L, Front R, Rear L, Rear R) given the driver's
 * current head rotation (rot_rad) and window state.
 */
static void compute_spatial_targets(float rot_rad, float cabin_out, float wnd_left, float wnd_right) {
    CabinSettings s = g_settings;
    float pos = s.position;
    float window_open = 0.5f * (wnd_left + wnd_right);
    float exterior_atten = 1.0f - 0.55f * cabin_out;

    /*
     * Physical speaker azimuths (radians) around the driver's seat in cabin coordinates:
     *   0: Front Left  (FL) — driver door / A-pillar (-33 deg)
     *   1: Front Right (FR) — passenger dash / door (+50 deg, further across cab)
     *   2: Rear Left   (RL) — sleeper / rear B-pillar behind driver (-136 deg)
     *   3: Rear Right  (RR) — sleeper / rear B-pillar behind passenger (+140 deg)
     */
    const float spk_angles[4] = {
        -0.58f + (0.28f * pos),
        +0.87f + (0.28f * pos),
        -2.37f + (0.14f * pos),
        +2.44f + (0.14f * pos)
    };

    /*
     * Base acoustic propagation delays in samples at 48 kHz (1 ms = 48 samples):
     *   FL is closest (~0.95 ms), FR is across the dash (~2.35 ms),
     *   RL is behind driver (~3.55 ms), RR is diagonally behind in sleeper (~4.85 ms).
     *   Shifting Dashboard Position left/right shifts the FL vs. FR arrival time!
     */
    const float base_delays[4] = {
        46.0f  + (28.0f * pos),
        113.0f - (28.0f * pos),
        170.0f + (14.0f * pos),
        232.0f - (14.0f * pos)
    };

    const float base_gains[4] = {
        s.front_gain * (1.02f - 0.10f * pos) * exterior_atten,
        s.front_gain * (0.92f + 0.10f * pos) * exterior_atten,
        s.rear_gain  * (0.98f - 0.06f * pos) * exterior_atten,
        s.rear_gain  * (0.90f + 0.06f * pos) * exterior_atten
    };

    SpatialTargets next;
    memset(&next, 0, sizeof(next));

    for (int k = 0; k < 4; k++) {
        /* Angle of speaker k relative to driver's nose:
         * Turning head left (rot_rad < 0) increases rel_angle -> shifts sound toward right ear (+).
         * Turning head right (rot_rad > 0) decreases rel_angle -> shifts sound toward left ear (-).
         */
        float rel_angle = spk_angles[k] - rot_rad;
        float az = sinf(rel_angle);        /* -1 = directly left ear, +1 = directly right ear */
        float front_cos = cosf(rel_angle); /* +1 = directly in front of face, -1 = directly behind head */

        /* Binaural Interaural Time Difference (ITD): +/- 15 samples (~0.62 ms interaural span). */
        float itd = 15.0f * az;
        next.delay_L[k] = base_delays[k] + itd;
        next.delay_R[k] = base_delays[k] - itd;

        /* Pinna front/back directivity: sound sources in front of the face have full treble presence;
         * sound sources behind the head have natural pinna high-frequency rolloff.
         * Turning your head toward the rear speakers brings their front_cos > 0, opening them up!
         */
        float front_factor = 0.5f * (1.0f + front_cos); /* 0 behind head, 1 in front of face */
        float pinna_high = 0.56f + 0.44f * front_factor;
        float pinna_low  = 0.86f + 0.14f * front_factor;

        /* Binaural Head-Shadowing (ILD):
         * Low frequencies (< 850 Hz) diffract around the head (mild ILD).
         * High frequencies (> 850 Hz) are strongly shadowed by the head on the opposite ear.
         */
        float pan_strength = 0.55f + 0.35f * s.amount;
        float low_ild_L  = clamp_float(1.0f - (0.34f * pan_strength * az), 0.45f, 1.35f);
        float low_ild_R  = clamp_float(1.0f + (0.34f * pan_strength * az), 0.45f, 1.35f);
        float high_ild_L = clamp_float(1.0f - (0.78f * pan_strength * az), 0.16f, 1.55f);
        float high_ild_R = clamp_float(1.0f + (0.78f * pan_strength * az), 0.16f, 1.55f);

        next.low_gain_L[k]  = base_gains[k] * pinna_low  * low_ild_L;
        next.low_gain_R[k]  = base_gains[k] * pinna_low  * low_ild_R;
        next.high_gain_L[k] = base_gains[k] * pinna_high * high_ild_L;
        next.high_gain_R[k] = base_gains[k] * pinna_high * high_ild_R;
    }

    /* Under-seat subwoofer loses cabin pressure when windows or doors/camera are open. */
    float cabin_seal = clamp_float(1.0f - 0.45f * window_open - 0.65f * cabin_out, 0.15f, 1.0f);
    next.sub_gain = s.sub_gain * cabin_seal;

    /* Asymmetric side-glass reflections: rolling down left window reduces left glass reflection. */
    float refl_seal_L = clamp_float(1.0f - 0.55f * wnd_left  - 0.70f * cabin_out, 0.10f, 1.0f);
    float refl_seal_R = clamp_float(1.0f - 0.55f * wnd_right - 0.70f * cabin_out, 0.10f, 1.0f);
    next.refl_gain_L = s.reflection_gain * refl_seal_L;
    next.refl_gain_R = s.reflection_gain * refl_seal_R;

    /* Dry blend only when user lowers Cabin Effect below 25%. */
    next.dry_blend = clamp_float(1.0f - (s.amount * 1.15f), 0.0f, 0.80f);

    g_targets = next;
}

static uint64_t g_last_cfg_mtime = 0;
static unsigned int g_cfg_tick = 0;

static uint64_t file_mtime_token(const char *path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return 0;
    return ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) |
           (uint64_t)fad.ftLastWriteTime.dwLowDateTime;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return ((uint64_t)st.st_mtimespec.tv_sec << 32) ^
           (uint64_t)st.st_mtimespec.tv_nsec;
#endif
}

static void check_live_reload(void) {
    if ((g_cfg_tick++ % 30) != 0) return;
    char path[PATH_MAX];
    live_settings_path(path, sizeof(path));
    uint64_t token = file_mtime_token(path);
    if (token == 0) return;
    if (g_last_cfg_mtime == 0) {
        g_last_cfg_mtime = token;
        return;
    }
    if (token == g_last_cfg_mtime) return;
    g_last_cfg_mtime = token;

    g_settings = read_settings();
    if (g_upnp_target_enabled != g_upnp_active) {
        g_upnp_active = g_upnp_target_enabled;
        upnp_set_enabled(g_upnp_active);
        cabin_log("UPnP Cabin Renderer %s via live settings.", g_upnp_active ? "enabled" : "disabled");
    }
    configure_eq(g_eq_dsp, g_settings);
    if (g_compressor_dsp) configure_compressor(g_compressor_dsp, g_settings);
    g_last_rot = 999.0f;
}

static void update_spatial_state(FMOD_STUDIO_SYSTEM *studio) {
    check_live_reload();
    if (!g_api.get_param) return;

    float raw_rot = read_studio_param(studio, "cabin_rot", 0.0f);
    float cabin_out = clamp_float(read_studio_param(studio, "cabin_out", 0.0f), 0.0f, 1.0f);
    float wnd_left = clamp_float(read_studio_param(studio, "wnd_left", 0.0f), 0.0f, 1.0f);
    float wnd_right = clamp_float(read_studio_param(studio, "wnd_right", 0.0f), 0.0f, 1.0f);
    float window_open = 0.5f * (wnd_left + wnd_right);

    /* Handle both radians [-pi, pi] and degrees [-180, 180] defensively. */
    float rot_rad = fabsf(raw_rot) > 3.25f ? (raw_rot * 0.0174532925f) : raw_rot;

    if (fabsf(rot_rad - g_last_rot) < 0.0015f &&
        fabsf(cabin_out - g_last_out) < 0.005f &&
        fabsf(wnd_left - g_last_wnd_l) < 0.005f &&
        fabsf(wnd_right - g_last_wnd_r) < 0.005f) {
        return;
    }

    g_last_rot = rot_rad;
    g_last_out = cabin_out;
    g_last_wnd_l = wnd_left;
    g_last_wnd_r = wnd_right;

    compute_spatial_targets(rot_rad, cabin_out, wnd_left, wnd_right);

    float open_air = clamp_float(cabin_out + 0.45f * window_open, 0.0f, 1.0f);
    g_dyn_low_gain  = g_settings.low_gain * (1.0f - 0.45f * open_air) - (2.2f * cabin_out);
    g_dyn_mid_gain  = g_settings.mid_gain - (1.5f * cabin_out);
    g_dyn_high_gain = g_settings.high_gain - (3.2f * cabin_out);

    if (g_eq_dsp) {
        g_api.set_float(g_eq_dsp, EQ_LOWGAIN, g_dyn_low_gain);
        g_api.set_float(g_eq_dsp, EQ_MIDGAIN, g_dyn_mid_gain);
        g_api.set_float(g_eq_dsp, EQ_HIGHGAIN, g_dyn_high_gain);
    }

    if (!g_turn_logged && fabsf(rot_rad) > 0.25f) {
        g_turn_logged = true;
        cabin_log("360-degree 4.1 multi-speaker head-tracking confirmed (cabin_rot=%.2f rad).", rot_rad);
    }
}

static bool try_install(FMOD_STUDIO_SYSTEM *studio) {
    if (!resolve_symbols()) {
        g_disabled = true;
        cabin_log("Required FMOD symbols are unavailable; live processing disabled safely.");
        return false;
    }

    FMOD_RESULT result = FMOD_OK;
    if (!g_radio_bus) {
        result = g_api.get_bus(studio, "bus:/radio", &g_radio_bus);
        if (result != FMOD_OK || !g_radio_bus) {
            if (g_retry_counter == 30) {
                cabin_log("Waiting for bus:/radio (last FMOD result %d)...", result);
            }
            return false;
        }
        cabin_log("Found bus:/radio.");
    }

    FMOD_CHANNELGROUP *group = NULL;
    result = g_api.get_group(g_radio_bus, &group);
    if (result != FMOD_OK || !group) {
        if (!g_radio_bus_locked) {
            FMOD_RESULT lock_result = g_api.lock_bus(g_radio_bus);
            if (lock_result == FMOD_OK || lock_result == 78) {
                g_radio_bus_locked = true;
                if (lock_result == FMOD_OK) g_api.flush(studio);
            } else {
                cabin_log("Found radio bus but could not lock it (FMOD result %d)", lock_result);
                return false;
            }
        }
        result = g_api.get_group(g_radio_bus, &group);
        if (result != FMOD_OK || !group) {
            if (g_retry_counter == 30) {
                cabin_log("Radio bus channel group is not ready yet (FMOD result %d)", result);
            }
            return false;
        }
    }

    FMOD_SYSTEM *core = NULL;
    result = g_api.get_core(studio, &core);
    if (result != FMOD_OK || !core) {
        cabin_log("Could not access FMOD core system (FMOD result %d)", result);
        return false;
    }

    unsigned int version = 0;
    unsigned int build = 0;
    if (g_api.get_version && g_api.get_version(core, &version, &build) == FMOD_OK) {
        cabin_log("FMOD runtime version: 0x%08x (build %u)", version, build);
    }

    g_settings = read_settings();
    if (g_upnp_target_enabled != g_upnp_active) {
        g_upnp_active = g_upnp_target_enabled;
        upnp_set_enabled(g_upnp_active);
        cabin_log("UPnP Cabin Renderer %s via live settings.", g_upnp_active ? "enabled" : "disabled");
    }
    compute_spatial_targets(0.0f, 0.0f, 0.0f, 0.0f);
    int attached = 0;

    FMOD_DSP *eq = create_effect_by_type(core, FMOD_DSP_TYPE_THREE_EQ, "three-band cabin EQ");
    if (eq) {
        configure_eq(eq, g_settings);
        if (attach_effect(group, eq, "three-band cabin EQ")) {
            g_eq_dsp = eq;
            attached++;
        }
    }

    FMOD_DSP *spatial = create_spatial_cabin_dsp(core);
    if (spatial) {
        if (attach_effect(group, spatial, "3D 4.1 multi-speaker cabin spatializer (FL/FR/RL/RR + Sub + Reflections)")) {
            g_spatial_dsp = spatial;
            attached++;
        }
    }

    FMOD_DSP *compressor = create_effect_by_type(core, FMOD_DSP_TYPE_COMPRESSOR, "speaker compressor");
    if (compressor) {
        configure_compressor(compressor, g_settings);
        if (attach_effect(group, compressor, "speaker compressor")) {
            g_compressor_dsp = compressor;
            attached++;
        }
    }

    if (attached > 0) {
        cabin_log("Live 4.1 cabin processing active on bus:/radio (%d stages).", attached);
        float init_rot = 0.0f, init_final = 0.0f;
        if (g_api.get_param && g_api.get_param(studio, "cabin_rot", &init_rot, &init_final) == FMOD_OK) {
            cabin_log("Camera head-tracking active (cabin_rot=%.2f, front L/R + rear L/R + sub).", init_final);
            update_spatial_state(studio);
        }
        return true;
    }

    cabin_log("No effects could be attached; original audio remains unchanged.");
    return false;
}

static void cabin_after_update(FMOD_STUDIO_SYSTEM *studio, const char *hook_name) {
    if (!g_hook_logged) {
        g_hook_logged = true;
        cabin_log("FMOD update hook active: %s", hook_name);
    }

    if (!g_settings_loaded_once) {
        g_settings_loaded_once = true;
        char path[PATH_MAX];
        live_settings_path(path, sizeof(path));
        g_last_cfg_mtime = file_mtime_token(path);
        g_settings = read_settings();
        configure_eq(NULL, g_settings);
        if (g_upnp_target_enabled != g_upnp_active) {
            g_upnp_active = g_upnp_target_enabled;
            upnp_set_enabled(g_upnp_active);
            cabin_log("UPnP Cabin Renderer %s on startup.", g_upnp_active ? "enabled" : "disabled");
        }
    }

    if (g_dry_run || g_disabled || !studio) return;

    if (g_installed) {
        update_spatial_state(studio);
        return;
    }

    check_live_reload();

    unsigned int tick = g_retry_counter++;
    if (tick < 3) return;
    unsigned int interval = tick < 35 ? 1 : 120;
    if ((tick % interval) != 0) return;

    lock_state();
    if (!g_installed && !g_disabled) g_installed = try_install(studio);
    unlock_state();
}

#ifdef __APPLE__

static FMOD_RESULT cabin_update(FMOD_STUDIO_SYSTEM *studio) {
    FMOD_RESULT result = FMOD_Studio_System_Update(studio);
    cabin_after_update(studio, "C API");
    return result;
}

static FMOD_RESULT cabin_cpp_update(FMOD_STUDIO_SYSTEM *studio) {
    FMOD_RESULT result = FMOD_CPP_Studio_System_Update(studio);
    cabin_after_update(studio, "C++ API");
    return result;
}

__attribute__((used))
static struct {
    const void *replacement;
    const void *replacee;
} update_interpose __attribute__((section("__DATA,__interpose"))) = {
    (const void *)cabin_update,
    (const void *)FMOD_Studio_System_Update
};

__attribute__((used))
static struct {
    const void *replacement;
    const void *replacee;
} cpp_update_interpose __attribute__((section("__DATA,__interpose"))) = {
    (const void *)cabin_cpp_update,
    (const void *)FMOD_CPP_Studio_System_Update
};

__attribute__((constructor))
static void cabin_loaded(void) {
    upnp_init();
    const char *dry_run = getenv("ETS2_CABIN_DRY_RUN");
    g_dry_run = dry_run && strcmp(dry_run, "1") == 0;
    cabin_log("ETS2 Cabin Radio live DSP loaded (pid %d).", getpid());
    if (g_dry_run) cabin_log("Dry-run mode enabled: no DSP will be attached.");
}

__attribute__((destructor))
static void cabin_unloaded(void) {
    upnp_shutdown();
}

#elif defined(_WIN32)

static StudioUpdateFn g_orig_c_update = NULL;
static StudioUpdateFn g_orig_cpp_update = NULL;
static volatile LONG g_win_hook_started = 0;

static FMOD_RESULT cabin_win_c_update(FMOD_STUDIO_SYSTEM *studio) {
    FMOD_RESULT result = g_orig_c_update ? g_orig_c_update(studio) : FMOD_OK;
    cabin_after_update(studio, "Windows IAT (C API)");
    return result;
}

static FMOD_RESULT cabin_win_cpp_update(FMOD_STUDIO_SYSTEM *studio) {
    FMOD_RESULT result = g_orig_cpp_update ? g_orig_cpp_update(studio) : FMOD_OK;
    cabin_after_update(studio, "Windows IAT (C++ API)");
    return result;
}

static int patch_module_iat(HMODULE module) {
    if (!module) return 0;
    uint8_t *base = (uint8_t *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;

    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    IMAGE_DATA_DIRECTORY import_dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!import_dir.VirtualAddress || !import_dir.Size) return 0;

    int patched = 0;
    IMAGE_IMPORT_DESCRIPTOR *desc =
        (IMAGE_IMPORT_DESCRIPTOR *)(base + import_dir.VirtualAddress);

    for (; desc->Name != 0; desc++) {
        const char *dll_name = (const char *)(base + desc->Name);
        if (_strnicmp(dll_name, "fmodstudio", 10) != 0) continue;

        IMAGE_THUNK_DATA *orig_thunk = desc->OriginalFirstThunk
            ? (IMAGE_THUNK_DATA *)(base + desc->OriginalFirstThunk)
            : NULL;
        IMAGE_THUNK_DATA *iat_thunk =
            (IMAGE_THUNK_DATA *)(base + desc->FirstThunk);

        for (; iat_thunk->u1.Function != 0; iat_thunk++, orig_thunk = orig_thunk ? orig_thunk + 1 : NULL) {
            const char *sym_name = NULL;
            if (orig_thunk && !(orig_thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                IMAGE_IMPORT_BY_NAME *by_name =
                    (IMAGE_IMPORT_BY_NAME *)(base + orig_thunk->u1.AddressOfData);
                sym_name = (const char *)by_name->Name;
            }

            ULONG_PTR current_fn = iat_thunk->u1.Function;
            ULONG_PTR replacement = 0;

            if ((g_orig_cpp_update && current_fn == (ULONG_PTR)g_orig_cpp_update) ||
                (sym_name && strcmp(sym_name, "?update@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@XZ") == 0)) {
                if (!g_orig_cpp_update) g_orig_cpp_update = (StudioUpdateFn)current_fn;
                replacement = (ULONG_PTR)&cabin_win_cpp_update;
            } else if ((g_orig_c_update && current_fn == (ULONG_PTR)g_orig_c_update) ||
                       (sym_name && strcmp(sym_name, "FMOD_Studio_System_Update") == 0)) {
                if (!g_orig_c_update) g_orig_c_update = (StudioUpdateFn)current_fn;
                replacement = (ULONG_PTR)&cabin_win_c_update;
            }

            if (replacement && current_fn != replacement) {
                DWORD old_protect = 0;
                if (VirtualProtect(&iat_thunk->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &old_protect)) {
                    iat_thunk->u1.Function = replacement;
                    VirtualProtect(&iat_thunk->u1.Function, sizeof(ULONG_PTR), old_protect, &old_protect);
                    patched++;
                }
            }
        }
    }
    return patched;
}

static unsigned __stdcall cabin_hook_worker(void *unused) {
    (void)unused;
    upnp_init();
    for (int attempt = 0; attempt < 300; attempt++) {
        HMODULE fmod_studio = GetModuleHandleA("fmodstudio.dll");
        if (!fmod_studio) fmod_studio = GetModuleHandleA("fmodstudioL.dll");
        if (fmod_studio) {
            g_orig_cpp_update = (StudioUpdateFn)GetProcAddress(
                fmod_studio, "?update@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@XZ");
            g_orig_c_update = (StudioUpdateFn)GetProcAddress(
                fmod_studio, "FMOD_Studio_System_Update");

            int patched = patch_module_iat(GetModuleHandleW(NULL));
            if (patched > 0) {
                cabin_log("Hooked FMOD Studio update via Windows PE IAT (%d slot%s).",
                          patched, patched == 1 ? "" : "s");
                return 0;
            }
        }
        Sleep(100);
    }
    cabin_log("Timed out waiting for fmodstudio.dll IAT in process.");
    return 0;
}

static void start_windows_hook_once(const char *source) {
    if (InterlockedCompareExchange(&g_win_hook_started, 1, 0) != 0) return;
    const char *dry_run = getenv("ETS2_CABIN_DRY_RUN");
    g_dry_run = dry_run && strcmp(dry_run, "1") == 0;
    cabin_log("ETS2 Cabin Radio live DSP loaded on Windows (%s, pid %lu).",
              source, (unsigned long)GetCurrentProcessId());
    if (g_dry_run) cabin_log("Dry-run mode enabled: no DSP will be attached.");

    uintptr_t thread = _beginthreadex(NULL, 0, cabin_hook_worker, NULL, 0, NULL);
    if (thread) CloseHandle((HANDLE)thread);
}

/* Optional SCS Telemetry SDK plugin entry points (for bin\win_x64\plugins\ auto-load). */
__declspec(dllexport) int __cdecl scs_telemetry_init(int version, const void *params) {
    (void)version;
    (void)params;
    start_windows_hook_once("SCS Plugin");
    return 0; /* SCS_RESULT_ok */
}

__declspec(dllexport) void __cdecl scs_telemetry_shutdown(void) {
    upnp_shutdown();
    cabin_log("SCS plugin shutdown notification received.");
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        start_windows_hook_once("DllMain");
    } else if (reason == DLL_PROCESS_DETACH) {
        if (reserved == NULL) {
            upnp_shutdown();
        }
    }
    return TRUE;
}

#endif