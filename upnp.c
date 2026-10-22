#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

/* Single-header codecs from upnp_codec/ (mackron/dr_libs) */
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "upnp_codec/dr_flac.h"

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "upnp_codec/dr_mp3.h"

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "upnp_codec/dr_wav.h"

extern void cabin_log(const char *format, ...);

/* Forward declarations for public UPnP functions */
bool upnp_init(void);
void upnp_shutdown(void);
void upnp_set_enabled(bool enabled);
bool upnp_is_active(void);
bool upnp_is_playing(void);
bool upnp_get_sample(float *out_left, float *out_right);

#ifdef _WIN32

#define UPNP_DEVICE_UUID "e752cab1-7a01-49b9-8e2b-e752cab10001"
#define UPNP_DEVICE_NAME "ETS2 Cabin Radio"
#define UPNP_HTTP_PORT 49152
#define UPNP_SSDP_PORT 1900
#define UPNP_SSDP_MCAST "239.255.255.250"

/* 262144 frames = ~5.46 seconds of stereo float audio */
#define UPNP_RING_SIZE 262144
#define UPNP_RING_MASK (UPNP_RING_SIZE - 1)
#define UPNP_PREBUFFER_FRAMES 12000

/* Data types for UPnP Transport & Codec Detection */
typedef enum {
    UPNP_STATE_STOPPED = 0,
    UPNP_STATE_PLAYING = 1,
    UPNP_STATE_PAUSED  = 2
} UpnpTransportState;

typedef enum {
    CODEC_FORMAT_UNKNOWN = 0,
    CODEC_FORMAT_WAV,
    CODEC_FORMAT_FLAC,
    CODEC_FORMAT_MP3
} CodecFormat;

typedef struct {
    float left[UPNP_RING_SIZE];
    float right[UPNP_RING_SIZE];
    volatile uint32_t write_pos;
    volatile uint32_t read_pos;
} UpnpRingBuffer;

/* Stream Socket Wrapper supporting plain/chunked HTTP and header peek */
typedef struct {
    SOCKET sock;
    bool is_chunked;
    size_t chunk_remaining;
    bool chunk_eof;
    bool (*should_stop)(void);
    uint8_t peek_buf[64];
    size_t peek_len;
    size_t peek_pos;
} StreamSocket;

/* GENA Event Subscription slot (for RenderingControl & AVTransport LastChange events) */
typedef struct {
    char callback_url[512];
    char sid[128];
    uint32_t seq;
    bool active;
} UpnpSubscription;

static bool g_upnp_initialized = false;
static bool g_upnp_enabled = false;
static bool g_upnp_running = false;
static char g_local_ip[64] = "127.0.0.1";

static SOCKET g_ssdp_socket = INVALID_SOCKET;
static SOCKET g_http_socket = INVALID_SOCKET;
static HANDLE g_ssdp_thread = NULL;
static HANDLE g_http_thread = NULL;

/* Generation counter to prevent race conditions between old and new streams */
static volatile LONG g_stream_gen = 0;
#ifdef _MSC_VER
#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_LOCAL __thread
#endif
static THREAD_LOCAL LONG t_my_gen = 0;
static volatile SOCKET g_stream_sock = INVALID_SOCKET;
static HANDLE g_stream_thread = NULL;
static HANDLE g_stream_stop_event = NULL;

static UpnpTransportState g_transport_state = UPNP_STATE_STOPPED;
static char g_current_uri[1024] = "";
static char g_next_uri[1024] = "";
static char g_track_duration[32] = "00:04:00";
static volatile uint64_t g_played_samples = 0;
static float g_upnp_volume = 1.0f;
static int g_upnp_volume_pct = 100;
static bool g_upnp_muted = false;

static UpnpSubscription g_sub_rc  = { "", "uuid:" UPNP_DEVICE_UUID "-rc",  0, false };
static UpnpSubscription g_sub_avt = { "", "uuid:" UPNP_DEVICE_UUID "-avt", 0, false };

static UpnpRingBuffer g_ring;
static volatile bool g_stream_active = false;
static volatile bool g_prebuffering = true;

static inline uint32_t ring_available_read(void) {
    return (g_ring.write_pos - g_ring.read_pos) & UPNP_RING_MASK;
}

static inline uint32_t ring_available_write(void) {
    return (UPNP_RING_SIZE - 1) - ring_available_read();
}

static void ring_push(float left, float right) {
    if (ring_available_write() < 1) return;
    uint32_t wp = g_ring.write_pos;
    g_ring.left[wp] = left;
    g_ring.right[wp] = right;
    MemoryBarrier();
    g_ring.write_pos = (wp + 1u) & UPNP_RING_MASK;
}

static void ring_clear(void) {
    g_ring.read_pos = g_ring.write_pos;
    g_prebuffering = true;
}

static bool stream_should_stop_cb(void) {
    return !g_upnp_running ||
           (g_transport_state != UPNP_STATE_PLAYING) ||
           (t_my_gen != g_stream_gen) ||
           (WaitForSingleObject(g_stream_stop_event, 0) == WAIT_OBJECT_0);
}

/* Rate-limiting backpressure: sleeps if buffer is full instead of discarding samples */
static bool ring_push_sample_cb(float left, float right) {
    if (ring_available_write() < 1) {
        while (ring_available_write() < 1024) {
            if (stream_should_stop_cb()) return false;
            Sleep(2);
        }
    }
    ring_push(left, right);
    return true;
}

/* Low-level socket reader supporting HTTP Chunked Transfer Encoding */
static size_t stream_read_raw(StreamSocket *s, void *buf, size_t count) {
    if (!s || s->sock == INVALID_SOCKET || count == 0) return 0;
    size_t total = 0;

    if (!s->is_chunked) {
        while (total < count) {
            if (s->should_stop && s->should_stop()) break;
            int r = recv(s->sock, (char *)buf + total, (int)(count - total), 0);
            if (r <= 0) break;
            total += (size_t)r;
        }
        return total;
    }

    /* HTTP Chunked Transfer Decoding */
    while (total < count) {
        if (s->should_stop && s->should_stop()) break;
        if (s->chunk_eof) break;

        if (s->chunk_remaining == 0) {
            char hex_line[64];
            int hex_pos = 0;
            while (hex_pos < (int)sizeof(hex_line) - 1) {
                if (s->should_stop && s->should_stop()) break;
                char c = 0;
                int r = recv(s->sock, &c, 1, 0);
                if (r <= 0) { s->chunk_eof = true; break; }
                if (c == '\n') break;
                if (c != '\r') hex_line[hex_pos++] = c;
            }
            if (s->chunk_eof) break;
            hex_line[hex_pos] = '\0';
            if (hex_pos == 0) continue; /* Skip blank line before chunk */

            char *semi = strchr(hex_line, ';');
            if (semi) *semi = '\0';

            unsigned long sz = strtoul(hex_line, NULL, 16);
            if (sz == 0) {
                s->chunk_eof = true;
                break;
            }
            s->chunk_remaining = sz;
        }

        size_t to_read = count - total;
        if (to_read > s->chunk_remaining) to_read = s->chunk_remaining;

        int r = recv(s->sock, (char *)buf + total, (int)to_read, 0);
        if (r <= 0) { s->chunk_eof = true; break; }

        total += (size_t)r;
        s->chunk_remaining -= (size_t)r;

        if (s->chunk_remaining == 0) {
            /* Consume the trailing \r\n after chunk data */
            char crlf[2];
            int cr = 0;
            while (cr < 2) {
                int rr = recv(s->sock, crlf + cr, 2 - cr, 0);
                if (rr <= 0) break;
                cr += rr;
            }
        }
    }
    return total;
}

/* Stream reader that consumes any peeked header bytes first */
static size_t stream_read(StreamSocket *s, void *buf, size_t count) {
    if (!s || count == 0) return 0;
    size_t total = 0;

    if (s->peek_pos < s->peek_len) {
        size_t avail = s->peek_len - s->peek_pos;
        size_t to_copy = count < avail ? count : avail;
        memcpy(buf, s->peek_buf + s->peek_pos, to_copy);
        s->peek_pos += to_copy;
        total += to_copy;
    }

    if (total < count) {
        total += stream_read_raw(s, (char *)buf + total, count - total);
    }
    return total;
}

/* Peek initial stream bytes without consuming them */
static size_t stream_peek(StreamSocket *s, const uint8_t **out_bytes) {
    if (s->peek_len == 0) {
        s->peek_len = stream_read_raw(s, s->peek_buf, sizeof(s->peek_buf));
        s->peek_pos = 0;
    }
    if (out_bytes) *out_bytes = s->peek_buf + s->peek_pos;
    return s->peek_len - s->peek_pos;
}

/* Forward-only stream skip helper for dr_libs seek callbacks */
static bool stream_skip(StreamSocket *s, int offset) {
    if (offset < 0) return false;
    if (offset == 0) return true;
    char discard[512];
    int remaining = offset;
    while (remaining > 0) {
        size_t to_read = remaining < (int)sizeof(discard) ? (size_t)remaining : sizeof(discard);
        size_t read_bytes = stream_read(s, discard, to_read);
        if (read_bytes == 0) return false;
        remaining -= (int)read_bytes;
    }
    return true;
}

/* Codec Detection */
static CodecFormat codec_detect_format(const char *content_type, const unsigned char *header_bytes, size_t len) {
    if (header_bytes && len >= 4) {
        if (header_bytes[0] == 'f' && header_bytes[1] == 'L' &&
            header_bytes[2] == 'a' && header_bytes[3] == 'C') {
            return CODEC_FORMAT_FLAC;
        }
        if ((header_bytes[0] == 'R' && header_bytes[1] == 'I' &&
             header_bytes[2] == 'F' && header_bytes[3] == 'F') ||
            (header_bytes[0] == 'r' && header_bytes[1] == 'i' &&
             header_bytes[2] == 'f' && header_bytes[3] == 'f')) {
            return CODEC_FORMAT_WAV;
        }
        if (header_bytes[0] == 'I' && header_bytes[1] == 'D' && header_bytes[2] == '3') {
            return CODEC_FORMAT_MP3;
        }
        if (header_bytes[0] == 0xFF && (header_bytes[1] & 0xE0) == 0xE0) {
            return CODEC_FORMAT_MP3;
        }
    }

    if (content_type) {
        if (strstr(content_type, "audio/flac") || strstr(content_type, "audio/x-flac")) {
            return CODEC_FORMAT_FLAC;
        }
        if (strstr(content_type, "audio/wav") || strstr(content_type, "audio/x-wav") ||
            strstr(content_type, "audio/wave") || strstr(content_type, "audio/L16")) {
            return CODEC_FORMAT_WAV;
        }
        if (strstr(content_type, "audio/mpeg") || strstr(content_type, "audio/mp3")) {
            return CODEC_FORMAT_MP3;
        }
    }

    return CODEC_FORMAT_UNKNOWN;
}

/* ============================================================================
 * 1. dr_flac Stream Decoder
 * ============================================================================ */
static size_t net_flac_read(void *pUserData, void *pBufferOut, size_t bytesToRead) {
    return stream_read((StreamSocket *)pUserData, pBufferOut, bytesToRead);
}

static drflac_bool32 net_flac_seek(void *pUserData, int offset, drflac_seek_origin origin) {
    if (origin == DRFLAC_SEEK_CUR && offset >= 0) {
        return stream_skip((StreamSocket *)pUserData, offset) ? DRFLAC_TRUE : DRFLAC_FALSE;
    }
    return DRFLAC_FALSE;
}

static void decode_flac_stream(StreamSocket *s,
                              bool (*push_sample)(float left, float right),
                              bool (*should_stop)(void)) {
    drflac *flac = drflac_open(net_flac_read, net_flac_seek, NULL, s, NULL);
    if (!flac) {
        cabin_log("UPnP: Failed to open FLAC stream via dr_flac.");
        return;
    }

    cabin_log("UPnP: dr_flac initialized: %u Hz, %u ch, %u-bit.",
              flac->sampleRate, flac->channels, flac->bitsPerSample);

    float resample_step = (float)flac->sampleRate / 48000.0f;
    float resample_pos = 0.0f;
    float prev_l = 0.0f, prev_r = 0.0f;

    #define FLAC_CHUNK_FRAMES 1024
    float chunk[FLAC_CHUNK_FRAMES * 8];
    uint32_t ch = flac->channels > 8 ? 8 : flac->channels;

    while (!should_stop()) {
        drflac_uint64 frames_read = drflac_read_pcm_frames_f32(flac, FLAC_CHUNK_FRAMES, chunk);
        if (frames_read == 0) break;

        for (drflac_uint64 f = 0; f < frames_read; f++) {
            float curr_l = chunk[f * flac->channels + 0];
            float curr_r = chunk[f * flac->channels + (ch > 1 ? 1 : 0)];

            while (resample_pos < 1.0f) {
                float out_l = prev_l + resample_pos * (curr_l - prev_l);
                float out_r = prev_r + resample_pos * (curr_r - prev_r);
                if (!push_sample(out_l, out_r)) {
                    drflac_close(flac);
                    return;
                }
                resample_pos += resample_step;
            }
            resample_pos -= 1.0f;
            prev_l = curr_l;
            prev_r = curr_r;
        }
    }

    drflac_close(flac);
}

/* ============================================================================
 * 2. dr_mp3 Stream Decoder
 * ============================================================================ */
static size_t net_mp3_read(void *pUserData, void *pBufferOut, size_t bytesToRead) {
    return stream_read((StreamSocket *)pUserData, pBufferOut, bytesToRead);
}

static drmp3_bool32 net_mp3_seek(void *pUserData, int offset, drmp3_seek_origin origin) {
    if (origin == DRMP3_SEEK_CUR && offset >= 0) {
        return stream_skip((StreamSocket *)pUserData, offset) ? DRMP3_TRUE : DRMP3_FALSE;
    }
    return DRMP3_FALSE;
}

static void decode_mp3_stream(StreamSocket *s,
                             bool (*push_sample)(float left, float right),
                             bool (*should_stop)(void)) {
    drmp3 mp3;
    if (!drmp3_init(&mp3, net_mp3_read, net_mp3_seek, NULL, NULL, s, NULL)) {
        cabin_log("UPnP: Failed to initialize MP3 stream via dr_mp3.");
        return;
    }

    cabin_log("UPnP: dr_mp3 initialized: %u Hz, %u ch.", mp3.sampleRate, mp3.channels);

    float resample_step = (float)mp3.sampleRate / 48000.0f;
    float resample_pos = 0.0f;
    float prev_l = 0.0f, prev_r = 0.0f;

    #define MP3_CHUNK_FRAMES 1024
    float chunk[MP3_CHUNK_FRAMES * 2];

    while (!should_stop()) {
        drmp3_uint64 frames_read = drmp3_read_pcm_frames_f32(&mp3, MP3_CHUNK_FRAMES, chunk);
        if (frames_read == 0) break;

        if (mp3.sampleRate > 0) {
            resample_step = (float)mp3.sampleRate / 48000.0f;
        }
        uint32_t ch = mp3.channels;

        for (drmp3_uint64 f = 0; f < frames_read; f++) {
            float curr_l = chunk[f * ch + 0];
            float curr_r = chunk[f * ch + (ch > 1 ? 1 : 0)];

            while (resample_pos < 1.0f) {
                float out_l = prev_l + resample_pos * (curr_l - prev_l);
                float out_r = prev_r + resample_pos * (curr_r - prev_r);
                if (!push_sample(out_l, out_r)) {
                    drmp3_uninit(&mp3);
                    return;
                }
                resample_pos += resample_step;
            }
            resample_pos -= 1.0f;
            prev_l = curr_l;
            prev_r = curr_r;
        }
    }

    drmp3_uninit(&mp3);
}

/* ============================================================================
 * 3. dr_wav Stream Decoder (+ Streaming RIFF / Raw LPCM Fallback for foobar2000)
 * ============================================================================ */
static size_t net_wav_read(void *pUserData, void *pBufferOut, size_t bytesToRead) {
    return stream_read((StreamSocket *)pUserData, pBufferOut, bytesToRead);
}

static drwav_bool32 net_wav_seek(void *pUserData, int offset, drwav_seek_origin origin) {
    if (origin == DRWAV_SEEK_CUR && offset >= 0) {
        return stream_skip((StreamSocket *)pUserData, offset) ? DRWAV_TRUE : DRWAV_FALSE;
    }
    return DRWAV_FALSE;
}

/* Fallback for infinite-length live WAV streams (size = 0 or 0xFFFFFFFF from foobar2000 UPnP) */
static void decode_live_wav_or_lpcm_stream(StreamSocket *s,
                                           bool (*push_sample)(float left, float right),
                                           bool (*should_stop)(void)) {
    const uint8_t *hdr = NULL;
    size_t hdr_len = stream_peek(s, &hdr);

    uint16_t audio_format = 1;     /* 1 = PCM, 3 = IEEE float */
    uint16_t channels = 2;
    uint32_t sample_rate = 44100;
    uint16_t bits_per_sample = 16;

    bool is_riff = (hdr_len >= 12 && memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 8, "WAVE", 4) == 0);
    if (is_riff) {
        char riff_hdr[12];
        stream_read(s, riff_hdr, 12); /* Consume "RIFF....WAVE" */

        while (!should_stop()) {
            char chunk_id[4];
            uint32_t chunk_size = 0;
            if (stream_read(s, chunk_id, 4) < 4) return;
            if (stream_read(s, &chunk_size, 4) < 4) return;

            if (memcmp(chunk_id, "fmt ", 4) == 0) {
                uint8_t fmt_data[64];
                size_t to_read = chunk_size < sizeof(fmt_data) ? chunk_size : sizeof(fmt_data);
                if (stream_read(s, fmt_data, to_read) < to_read) return;
                if (to_read >= 16) {
                    audio_format    = *(uint16_t *)(fmt_data + 0);
                    channels        = *(uint16_t *)(fmt_data + 2);
                    sample_rate     = *(uint32_t *)(fmt_data + 4);
                    bits_per_sample = *(uint16_t *)(fmt_data + 14);
                    /* WAVE_FORMAT_EXTENSIBLE (0xFFFE) subformat check */
                    if (audio_format == 0xFFFE && to_read >= 26) {
                        audio_format = *(uint16_t *)(fmt_data + 24);
                    }
                }
                if (chunk_size > to_read) {
                    if (!stream_skip(s, (int)(chunk_size - to_read))) return;
                }
            } else if (memcmp(chunk_id, "data", 4) == 0) {
                break; /* Audio PCM payload starts immediately */
            } else {
                if (!stream_skip(s, (int)chunk_size)) return;
            }
        }
    }

    if (channels < 1) channels = 1;
    if (sample_rate < 8000 || sample_rate > 384000) sample_rate = 44100;
    if (bits_per_sample != 16 && bits_per_sample != 24 && bits_per_sample != 32) {
        bits_per_sample = 16;
    }

    cabin_log("UPnP: Live WAV/LPCM stream active: %u Hz, %u ch, %u-bit (fmt=%u).",
              sample_rate, channels, bits_per_sample, audio_format);

    size_t bytes_per_sample = bits_per_sample / 8;
    size_t frame_bytes = channels * bytes_per_sample;
    float resample_step = (float)sample_rate / 48000.0f;
    float resample_pos = 0.0f;
    float prev_l = 0.0f, prev_r = 0.0f;

    #define LIVE_WAV_FRAMES 512
    uint8_t raw_buf[LIVE_WAV_FRAMES * 8 * 4];

    while (!should_stop()) {
        size_t bytes_to_read = LIVE_WAV_FRAMES * frame_bytes;
        size_t bytes_read = stream_read(s, raw_buf, bytes_to_read);
        if (bytes_read < frame_bytes) break;

        size_t frames = bytes_read / frame_bytes;
        for (size_t f = 0; f < frames; f++) {
            const uint8_t *fp = raw_buf + f * frame_bytes;
            float curr_l = 0.0f, curr_r = 0.0f;

            if (audio_format == 3 && bits_per_sample == 32) {
                curr_l = *(const float *)fp;
                curr_r = channels > 1 ? *(const float *)(fp + 4) : curr_l;
            } else if (bits_per_sample == 16) {
                const int16_t *pcm16 = (const int16_t *)fp;
                curr_l = (float)pcm16[0] / 32768.0f;
                curr_r = channels > 1 ? (float)pcm16[1] / 32768.0f : curr_l;
            } else if (bits_per_sample == 24) {
                int32_t val_l = (int32_t)(((uint32_t)fp[0]) | (((uint32_t)fp[1]) << 8) | (((uint32_t)fp[2]) << 16));
                if (val_l & 0x800000) val_l |= 0xFF000000;
                curr_l = (float)val_l / 8388608.0f;
                if (channels > 1) {
                    const uint8_t *rp = fp + 3;
                    int32_t val_r = (int32_t)(((uint32_t)rp[0]) | (((uint32_t)rp[1]) << 8) | (((uint32_t)rp[2]) << 16));
                    if (val_r & 0x800000) val_r |= 0xFF000000;
                    curr_r = (float)val_r / 8388608.0f;
                } else {
                    curr_r = curr_l;
                }
            } else if (bits_per_sample == 32) {
                const int32_t *pcm32 = (const int32_t *)fp;
                curr_l = (float)pcm32[0] / 2147483648.0f;
                curr_r = channels > 1 ? (float)pcm32[1] / 2147483648.0f : curr_l;
            }

            while (resample_pos < 1.0f) {
                float out_l = prev_l + resample_pos * (curr_l - prev_l);
                float out_r = prev_r + resample_pos * (curr_r - prev_r);
                if (!push_sample(out_l, out_r)) return;
                resample_pos += resample_step;
            }
            resample_pos -= 1.0f;
            prev_l = curr_l;
            prev_r = curr_r;
        }
    }
}

static void decode_wav_stream(StreamSocket *s,
                             bool (*push_sample)(float left, float right),
                             bool (*should_stop)(void)) {
    const uint8_t *hdr = NULL;
    size_t hdr_len = stream_peek(s, &hdr);

    /* Check if this is a finite-size RIFF WAV or an infinite UPnP stream (size 0 or >= 0x7FFFFFFF) */
    bool use_drwav = false;
    if (hdr_len >= 12 && memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 8, "WAVE", 4) == 0) {
        uint32_t riff_size = *(const uint32_t *)(hdr + 4);
        if (riff_size > 36 && riff_size < 0x7FFFFFF0u) {
            use_drwav = true;
        } else {
            cabin_log("UPnP: Detected infinite-length WAV stream (RIFF size=0x%08X); using live stream parser.", riff_size);
        }
    }

    if (!use_drwav) {
        decode_live_wav_or_lpcm_stream(s, push_sample, should_stop);
        return;
    }

    drwav wav;
    if (!drwav_init(&wav, net_wav_read, net_wav_seek, NULL, s, NULL)) {
        cabin_log("UPnP: dr_wav rejected header; falling back to live WAV parser.");
        s->peek_pos = 0; /* Rewind peek buffer so fallback sees RIFF header */
        decode_live_wav_or_lpcm_stream(s, push_sample, should_stop);
        return;
    }

    cabin_log("UPnP: dr_wav initialized: %u Hz, %u ch, %u-bit.",
              wav.sampleRate, wav.channels, wav.bitsPerSample);

    float resample_step = (float)wav.sampleRate / 48000.0f;
    float resample_pos = 0.0f;
    float prev_l = 0.0f, prev_r = 0.0f;

    #define WAV_CHUNK_FRAMES 1024
    float chunk[WAV_CHUNK_FRAMES * 8];
    uint32_t ch = wav.channels > 8 ? 8 : wav.channels;

    while (!should_stop()) {
        drwav_uint64 frames_read = drwav_read_pcm_frames_f32(&wav, WAV_CHUNK_FRAMES, chunk);
        if (frames_read == 0) break;

        for (drwav_uint64 f = 0; f < frames_read; f++) {
            float curr_l = chunk[f * wav.channels + 0];
            float curr_r = chunk[f * wav.channels + (ch > 1 ? 1 : 0)];

            while (resample_pos < 1.0f) {
                float out_l = prev_l + resample_pos * (curr_l - prev_l);
                float out_r = prev_r + resample_pos * (curr_r - prev_r);
                if (!push_sample(out_l, out_r)) {
                    drwav_uninit(&wav);
                    return;
                }
                resample_pos += resample_step;
            }
            resample_pos -= 1.0f;
            prev_l = curr_l;
            prev_r = curr_r;
        }
    }

    drwav_uninit(&wav);
}

static void codec_stream_and_decode(StreamSocket *s, CodecFormat format,
                                   bool (*push_sample)(float left, float right),
                                   bool (*should_stop)(void)) {
    switch (format) {
    case CODEC_FORMAT_FLAC:
        cabin_log("UPnP: Decoding FLAC stream via dr_flac (foobar2000 / Hi-Res).");
        decode_flac_stream(s, push_sample, should_stop);
        break;
    case CODEC_FORMAT_MP3:
        cabin_log("UPnP: Decoding MP3 stream via dr_mp3.");
        decode_mp3_stream(s, push_sample, should_stop);
        break;
    case CODEC_FORMAT_WAV:
    default:
        cabin_log("UPnP: Decoding WAV / LPCM stream via dr_wav.");
        decode_wav_stream(s, push_sample, should_stop);
        break;
    }
}

static char *find_header_value(const char *req, const char *header_name) {
    size_t hlen = strlen(header_name);
    const char *p = req;
    while (*p) {
        if ((p == req || *(p - 1) == '\n') &&
            _strnicmp(p, header_name, hlen) == 0 &&
            (p[hlen] == ':' || p[hlen] == ' ')) {
            const char *val = p + hlen;
            if (*val == ':') val++;
            while (*val == ' ' || *val == '\t') val++;
            return (char *)val;
        }
        p++;
    }
    return NULL;
}

/* ============================================================================
 * Universal GENA Event Notification (pushes Volume/Mute/Transport state to apps)
 * ============================================================================ */
static void send_gena_notify(UpnpSubscription *sub, const char *prop_body) {
    if (!sub || !sub->active || !sub->callback_url[0]) return;

    char host[256] = "";
    int port = 80;
    char path[256] = "/";

    const char *p = sub->callback_url;
    if (strncmp(p, "http://", 7) == 0) p += 7;

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    if (colon && (!slash || colon < slash)) {
        size_t hlen = (size_t)(colon - p);
        if (hlen < sizeof(host)) {
            memcpy(host, p, hlen);
            host[hlen] = '\0';
        }
        port = atoi(colon + 1);
    } else if (slash) {
        size_t hlen = (size_t)(slash - p);
        if (hlen < sizeof(host)) {
            memcpy(host, p, hlen);
            host[hlen] = '\0';
        }
    } else {
        snprintf(host, sizeof(host), "%s", p);
    }

    if (slash) {
        snprintf(path, sizeof(path), "%s", slash);
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return;

    DWORD tv = 400;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = inet_addr(host);

    if (addr.sin_addr.s_addr != INADDR_NONE &&
        connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        char req[2048];
        size_t body_len = strlen(prop_body);
        snprintf(req, sizeof(req),
                 "NOTIFY %s HTTP/1.1\r\n"
                 "HOST: %s:%d\r\n"
                 "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                 "CONTENT-LENGTH: %zu\r\n"
                 "NT: upnp:event\r\n"
                 "NTS: upnp:propchange\r\n"
                 "SID: %s\r\n"
                 "SEQ: %u\r\n"
                 "Connection: close\r\n\r\n"
                 "%s",
                 path, host, port, body_len, sub->sid, sub->seq++, prop_body);
        send(s, req, (int)strlen(req), 0);
    }
    closesocket(s);
}

static void notify_rendering_control_change(void) {
    if (!g_sub_rc.active) return;
    char body[1024];
    int m = g_upnp_muted ? 1 : 0;
    int v = g_upnp_volume_pct;
    snprintf(body, sizeof(body),
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
             "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">\r\n"
             "  <e:property>\r\n"
             "    <LastChange>&lt;Event xmlns=&quot;urn:schemas-upnp-org:metadata-1-0/RCS/&quot;&gt;"
             "&lt;InstanceID val=&quot;0&quot;&gt;"
             "&lt;Volume channel=&quot;Master&quot; val=&quot;%d&quot;/&gt;"
             "&lt;Volume channel=&quot;LF&quot; val=&quot;%d&quot;/&gt;"
             "&lt;Volume channel=&quot;RF&quot; val=&quot;%d&quot;/&gt;"
             "&lt;Mute channel=&quot;Master&quot; val=&quot;%d&quot;/&gt;"
             "&lt;Mute channel=&quot;LF&quot; val=&quot;%d&quot;/&gt;"
             "&lt;Mute channel=&quot;RF&quot; val=&quot;%d&quot;/&gt;"
             "&lt;/InstanceID&gt;&lt;/Event&gt;</LastChange>\r\n"
             "  </e:property>\r\n"
             "</e:propertyset>\r\n",
             v, v, v, m, m, m);
    send_gena_notify(&g_sub_rc, body);
}

static void notify_avtransport_change(void) {
    if (!g_sub_avt.active) return;
    const char *state_str = "STOPPED";
    if (g_transport_state == UPNP_STATE_PLAYING) state_str = "PLAYING";
    else if (g_transport_state == UPNP_STATE_PAUSED) state_str = "PAUSED_PLAYBACK";

    char body[1024];
    snprintf(body, sizeof(body),
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
             "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">\r\n"
             "  <e:property>\r\n"
             "    <LastChange>&lt;Event xmlns=&quot;urn:schemas-upnp-org:metadata-1-0/AVT/&quot;&gt;"
             "&lt;InstanceID val=&quot;0&quot;&gt;"
             "&lt;TransportState val=&quot;%s&quot;/&gt;"
             "&lt;CurrentTransportActions val=&quot;Play,Stop,Pause&quot;/&gt;"
             "&lt;/InstanceID&gt;&lt;/Event&gt;</LastChange>\r\n"
             "  </e:property>\r\n"
             "</e:propertyset>\r\n",
             state_str);
    send_gena_notify(&g_sub_avt, body);
}

static void send_ssdp_byebye(SOCKET sock, const struct sockaddr_in *dst, const char *nt) {
    char usn[256];
    if (strcmp(nt, "uuid:" UPNP_DEVICE_UUID) == 0 || strcmp(nt, UPNP_DEVICE_UUID) == 0) {
        snprintf(usn, sizeof(usn), "uuid:" UPNP_DEVICE_UUID);
    } else {
        snprintf(usn, sizeof(usn), "uuid:" UPNP_DEVICE_UUID "::%s", nt);
    }

    char packet[1024];
    snprintf(packet, sizeof(packet),
             "NOTIFY * HTTP/1.1\r\n"
             "HOST: 239.255.255.250:1900\r\n"
             "NT: %s\r\n"
             "NTS: ssdp:byebye\r\n"
             "USN: %s\r\n"
             "BOOTID.UPNP.ORG: 1\r\n"
             "\r\n",
             nt, usn);

    sendto(sock, packet, (int)strlen(packet), 0, (const struct sockaddr *)dst, sizeof(*dst));
}

static void broadcast_ssdp_byebye(SOCKET sock) {
    struct sockaddr_in mcast;
    memset(&mcast, 0, sizeof(mcast));
    mcast.sin_family = AF_INET;
    mcast.sin_port = htons(UPNP_SSDP_PORT);
    mcast.sin_addr.s_addr = inet_addr(UPNP_SSDP_MCAST);

    static const char *targets[] = {
        "upnp:rootdevice",
        ("uuid:" UPNP_DEVICE_UUID),
        "urn:schemas-upnp-org:device:MediaRenderer:1",
        "urn:schemas-upnp-org:service:AVTransport:1",
        "urn:schemas-upnp-org:service:RenderingControl:1",
        "urn:schemas-upnp-org:service:ConnectionManager:1"
    };

    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        send_ssdp_byebye(sock, &mcast, targets[i]);
    }
}

static void detect_local_ip(void) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return;

    struct sockaddr_in target;
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(53);
    target.sin_addr.s_addr = inet_addr("8.8.8.8");

    if (connect(s, (struct sockaddr *)&target, sizeof(target)) == 0) {
        struct sockaddr_in local;
        int local_len = sizeof(local);
        if (getsockname(s, (struct sockaddr *)&local, &local_len) == 0) {
            const char *ip_str = inet_ntoa(local.sin_addr);
            if (ip_str && *ip_str) {
                snprintf(g_local_ip, sizeof(g_local_ip), "%s", ip_str);
            }
        }
    }
    closesocket(s);
}

static void send_ssdp_response(SOCKET sock, const struct sockaddr_in *dst, const char *st) {
    char usn[256];
    if (strcmp(st, "uuid:" UPNP_DEVICE_UUID) == 0 || strcmp(st, UPNP_DEVICE_UUID) == 0) {
        snprintf(usn, sizeof(usn), "uuid:" UPNP_DEVICE_UUID);
    } else {
        snprintf(usn, sizeof(usn), "uuid:" UPNP_DEVICE_UUID "::%s", st);
    }

    char packet[1024];
    snprintf(packet, sizeof(packet),
             "HTTP/1.1 200 OK\r\n"
             "CACHE-CONTROL: max-age=1800\r\n"
             "EXT:\r\n"
             "LOCATION: http://%s:%d/description.xml\r\n"
             "SERVER: Windows/10.0 UPnP/1.0 ETS2CabinRadio/1.2\r\n"
             "ST: %s\r\n"
             "USN: %s\r\n"
             "BOOTID.UPNP.ORG: 1\r\n"
             "\r\n",
             g_local_ip, UPNP_HTTP_PORT, st, usn);

    sendto(sock, packet, (int)strlen(packet), 0, (const struct sockaddr *)dst, sizeof(*dst));
}

static void send_ssdp_notify(SOCKET sock, const struct sockaddr_in *dst, const char *nt) {
    char usn[256];
    if (strcmp(nt, "uuid:" UPNP_DEVICE_UUID) == 0 || strcmp(nt, UPNP_DEVICE_UUID) == 0) {
        snprintf(usn, sizeof(usn), "uuid:" UPNP_DEVICE_UUID);
    } else {
        snprintf(usn, sizeof(usn), "uuid:" UPNP_DEVICE_UUID "::%s", nt);
    }

    char packet[1024];
    snprintf(packet, sizeof(packet),
             "NOTIFY * HTTP/1.1\r\n"
             "HOST: 239.255.255.250:1900\r\n"
             "CACHE-CONTROL: max-age=1800\r\n"
             "LOCATION: http://%s:%d/description.xml\r\n"
             "NT: %s\r\n"
             "NTS: ssdp:alive\r\n"
             "SERVER: Windows/10.0 UPnP/1.0 ETS2CabinRadio/1.2\r\n"
             "USN: %s\r\n"
             "BOOTID.UPNP.ORG: 1\r\n"
             "\r\n",
             g_local_ip, UPNP_HTTP_PORT, nt, usn);

    sendto(sock, packet, (int)strlen(packet), 0, (const struct sockaddr *)dst, sizeof(*dst));
}

static void broadcast_ssdp_alive(SOCKET sock) {
    struct sockaddr_in mcast;
    memset(&mcast, 0, sizeof(mcast));
    mcast.sin_family = AF_INET;
    mcast.sin_port = htons(UPNP_SSDP_PORT);
    mcast.sin_addr.s_addr = inet_addr(UPNP_SSDP_MCAST);

    static const char *targets[] = {
        "upnp:rootdevice",
        ("uuid:" UPNP_DEVICE_UUID),
        "urn:schemas-upnp-org:device:MediaRenderer:1",
        "urn:schemas-upnp-org:service:AVTransport:1",
        "urn:schemas-upnp-org:service:RenderingControl:1",
        "urn:schemas-upnp-org:service:ConnectionManager:1"
    };

    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        send_ssdp_notify(sock, &mcast, targets[i]);
    }
}

static unsigned __stdcall upnp_ssdp_worker(void *unused) {
    (void)unused;
    char buffer[2048];

    DWORD tv = 2000;
    setsockopt(g_ssdp_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

    broadcast_ssdp_alive(g_ssdp_socket);
    uint32_t last_broadcast = GetTickCount();

    while (g_upnp_running && g_ssdp_socket != INVALID_SOCKET) {
        uint32_t now = GetTickCount();
        if (now - last_broadcast >= 4000) {
            broadcast_ssdp_alive(g_ssdp_socket);
            last_broadcast = now;
        }

        struct sockaddr_in client;
        int client_len = sizeof(client);
        int received = recvfrom(g_ssdp_socket, buffer, sizeof(buffer) - 1, 0,
                                (struct sockaddr *)&client, &client_len);
        if (received <= 0) {
            continue;
        }

        buffer[received] = '\0';
        if (strncmp(buffer, "M-SEARCH", 8) != 0) continue;

        char *st = find_header_value(buffer, "ST");
        if (!st) continue;

        char st_clean[256] = "";
        char *end = strpbrk(st, "\r\n");
        size_t st_len = end ? (size_t)(end - st) : strlen(st);
        if (st_len >= sizeof(st_clean)) st_len = sizeof(st_clean) - 1;
        memcpy(st_clean, st, st_len);
        st_clean[st_len] = '\0';

        cabin_log("UPnP: M-SEARCH from %s:%d (ST=%s)",
                  inet_ntoa(client.sin_addr), ntohs(client.sin_port), st_clean);
        st = st_clean;

        if (strstr(st, "ssdp:all")) {
            send_ssdp_response(g_ssdp_socket, &client, "upnp:rootdevice");
            send_ssdp_response(g_ssdp_socket, &client, "uuid:" UPNP_DEVICE_UUID);
            send_ssdp_response(g_ssdp_socket, &client, "urn:schemas-upnp-org:device:MediaRenderer:1");
            send_ssdp_response(g_ssdp_socket, &client, "urn:schemas-upnp-org:service:AVTransport:1");
            send_ssdp_response(g_ssdp_socket, &client, "urn:schemas-upnp-org:service:RenderingControl:1");
            send_ssdp_response(g_ssdp_socket, &client, "urn:schemas-upnp-org:service:ConnectionManager:1");
        } else if (strstr(st, "device:MediaRenderer") || strstr(st, "MediaRenderer:1")) {
            send_ssdp_response(g_ssdp_socket, &client, "urn:schemas-upnp-org:device:MediaRenderer:1");
        } else if (strstr(st, "upnp:rootdevice")) {
            send_ssdp_response(g_ssdp_socket, &client, "upnp:rootdevice");
        } else if (strstr(st, UPNP_DEVICE_UUID)) {
            send_ssdp_response(g_ssdp_socket, &client, "uuid:" UPNP_DEVICE_UUID);
        } else if (strstr(st, "service:AVTransport")) {
            send_ssdp_response(g_ssdp_socket, &client, "urn:schemas-upnp-org:service:AVTransport:1");
        } else if (strstr(st, "service:RenderingControl")) {
            send_ssdp_response(g_ssdp_socket, &client, "urn:schemas-upnp-org:service:RenderingControl:1");
        } else if (strstr(st, "service:ConnectionManager")) {
            send_ssdp_response(g_ssdp_socket, &client, "urn:schemas-upnp-org:service:ConnectionManager:1");
        }
    }
    return 0;
}

static const char kDeviceDescriptionXml[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
    "<root xmlns=\"urn:schemas-upnp-org:device-1-0\" xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">\r\n"
    "  <specVersion><major>1</major><minor>0</minor></specVersion>\r\n"
    "  <device>\r\n"
    "    <deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>\r\n"
    "    <friendlyName>" UPNP_DEVICE_NAME "</friendlyName>\r\n"
    "    <manufacturer>ETS2 Cabin Radio Team</manufacturer>\r\n"
    "    <modelName>ETS2 4.1 Virtual Cabin Audio</modelName>\r\n"
    "    <modelNumber>1.2</modelNumber>\r\n"
    "    <UDN>uuid:" UPNP_DEVICE_UUID "</UDN>\r\n"
    "    <dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC>\r\n"
    "    <serviceList>\r\n"
    "      <service>\r\n"
    "        <serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>\r\n"
    "        <serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>\r\n"
    "        <controlURL>/control/AVTransport</controlURL>\r\n"
    "        <eventSubURL>/events/AVTransport</eventSubURL>\r\n"
    "        <SCPDURL>/AVTransport.xml</SCPDURL>\r\n"
    "      </service>\r\n"
    "      <service>\r\n"
    "        <serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>\r\n"
    "        <serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>\r\n"
    "        <controlURL>/control/RenderingControl</controlURL>\r\n"
    "        <eventSubURL>/events/RenderingControl</eventSubURL>\r\n"
    "        <SCPDURL>/RenderingControl.xml</SCPDURL>\r\n"
    "      </service>\r\n"
    "      <service>\r\n"
    "        <serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>\r\n"
    "        <serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>\r\n"
    "        <controlURL>/control/ConnectionManager</controlURL>\r\n"
    "        <eventSubURL>/events/ConnectionManager</eventSubURL>\r\n"
    "        <SCPDURL>/ConnectionManager.xml</SCPDURL>\r\n"
    "      </service>\r\n"
    "    </serviceList>\r\n"
    "  </device>\r\n"
    "</root>\r\n";

static const char kAVTransportXml[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
    "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
    "  <specVersion><major>1</major><minor>0</minor></specVersion>\r\n"
    "  <actionList>\r\n"
    "    <action><name>SetAVTransportURI</name></action>\r\n"
    "    <action><name>SetNextAVTransportURI</name></action>\r\n"
    "    <action><name>Play</name></action>\r\n"
    "    <action><name>Pause</name></action>\r\n"
    "    <action><name>Stop</name></action>\r\n"
    "    <action><name>GetTransportInfo</name></action>\r\n"
    "    <action><name>GetPositionInfo</name></action>\r\n"
    "    <action><name>GetMediaInfo</name></action>\r\n"
    "    <action><name>GetDeviceCapabilities</name></action>\r\n"
    "    <action><name>GetTransportSettings</name></action>\r\n"
    "    <action><name>GetCurrentTransportActions</name></action>\r\n"
    "  </actionList>\r\n"
    "  <serviceStateTable>\r\n"
    "    <stateVariable sendEvents=\"yes\"><name>LastChange</name><dataType>string</dataType></stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\"><name>TransportState</name><dataType>string</dataType></stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\"><name>CurrentURI</name><dataType>string</dataType></stateVariable>\r\n"
    "  </serviceStateTable>\r\n"
    "</scpd>\r\n";

static const char kRenderingControlXml[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
    "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
    "  <specVersion><major>1</major><minor>0</minor></specVersion>\r\n"
    "  <actionList>\r\n"
    "    <action>\r\n"
    "      <name>ListPresets</name>\r\n"
    "      <argumentList>\r\n"
    "        <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>\r\n"
    "        <argument><name>CurrentPresetNameList</name><direction>out</direction><relatedStateVariable>PresetNameList</relatedStateVariable></argument>\r\n"
    "      </argumentList>\r\n"
    "    </action>\r\n"
    "    <action>\r\n"
    "      <name>SelectPreset</name>\r\n"
    "      <argumentList>\r\n"
    "        <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>\r\n"
    "        <argument><name>PresetName</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_PresetName</relatedStateVariable></argument>\r\n"
    "      </argumentList>\r\n"
    "    </action>\r\n"
    "    <action>\r\n"
    "      <name>GetMute</name>\r\n"
    "      <argumentList>\r\n"
    "        <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>\r\n"
    "        <argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>\r\n"
    "        <argument><name>CurrentMute</name><direction>out</direction><relatedStateVariable>Mute</relatedStateVariable></argument>\r\n"
    "      </argumentList>\r\n"
    "    </action>\r\n"
    "    <action>\r\n"
    "      <name>SetMute</name>\r\n"
    "      <argumentList>\r\n"
    "        <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>\r\n"
    "        <argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>\r\n"
    "        <argument><name>DesiredMute</name><direction>in</direction><relatedStateVariable>Mute</relatedStateVariable></argument>\r\n"
    "      </argumentList>\r\n"
    "    </action>\r\n"
    "    <action>\r\n"
    "      <name>GetVolume</name>\r\n"
    "      <argumentList>\r\n"
    "        <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>\r\n"
    "        <argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>\r\n"
    "        <argument><name>CurrentVolume</name><direction>out</direction><relatedStateVariable>Volume</relatedStateVariable></argument>\r\n"
    "      </argumentList>\r\n"
    "    </action>\r\n"
    "    <action>\r\n"
    "      <name>SetVolume</name>\r\n"
    "      <argumentList>\r\n"
    "        <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>\r\n"
    "        <argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>\r\n"
    "        <argument><name>DesiredVolume</name><direction>in</direction><relatedStateVariable>Volume</relatedStateVariable></argument>\r\n"
    "      </argumentList>\r\n"
    "    </action>\r\n"
    "  </actionList>\r\n"
    "  <serviceStateTable>\r\n"
    "    <stateVariable sendEvents=\"yes\">\r\n"
    "      <name>LastChange</name>\r\n"
    "      <dataType>string</dataType>\r\n"
    "    </stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\">\r\n"
    "      <name>PresetNameList</name>\r\n"
    "      <dataType>string</dataType>\r\n"
    "    </stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\">\r\n"
    "      <name>Mute</name>\r\n"
    "      <dataType>boolean</dataType>\r\n"
    "    </stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\">\r\n"
    "      <name>Volume</name>\r\n"
    "      <dataType>ui2</dataType>\r\n"
    "      <allowedValueRange><minimum>0</minimum><maximum>100</maximum><step>1</step></allowedValueRange>\r\n"
    "    </stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\">\r\n"
    "      <name>A_ARG_TYPE_Channel</name>\r\n"
    "      <dataType>string</dataType>\r\n"
    "      <allowedValueList><allowedValue>Master</allowedValue><allowedValue>LF</allowedValue><allowedValue>RF</allowedValue></allowedValueList>\r\n"
    "    </stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\">\r\n"
    "      <name>A_ARG_TYPE_InstanceID</name>\r\n"
    "      <dataType>ui4</dataType>\r\n"
    "    </stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\">\r\n"
    "      <name>A_ARG_TYPE_PresetName</name>\r\n"
    "      <dataType>string</dataType>\r\n"
    "      <allowedValueList><allowedValue>FactoryDefaults</allowedValue></allowedValueList>\r\n"
    "    </stateVariable>\r\n"
    "  </serviceStateTable>\r\n"
    "</scpd>\r\n";

static const char kConnectionManagerXml[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
    "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
    "  <specVersion><major>1</major><minor>0</minor></specVersion>\r\n"
    "  <actionList>\r\n"
    "    <action><name>GetProtocolInfo</name></action>\r\n"
    "    <action><name>GetCurrentConnectionIDs</name></action>\r\n"
    "    <action><name>GetCurrentConnectionInfo</name></action>\r\n"
    "  </actionList>\r\n"
    "  <serviceStateTable>\r\n"
    "    <stateVariable sendEvents=\"no\">\r\n"
    "      <name>SourceProtocolInfo</name>\r\n"
    "      <dataType>string</dataType>\r\n"
    "    </stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\">\r\n"
    "      <name>SinkProtocolInfo</name>\r\n"
    "      <dataType>string</dataType>\r\n"
    "    </stateVariable>\r\n"
    "  </serviceStateTable>\r\n"
    "</scpd>\r\n";

static void send_http_response(SOCKET client, int code, const char *mime, const char *body) {
    char header[512];
    size_t body_len = body ? strlen(body) : 0;
    snprintf(header, sizeof(header),
             "HTTP/1.1 %d OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "Server: Windows/10.0 UPnP/1.0 ETS2CabinRadio/1.2\r\n\r\n",
             code, mime, body_len);

    send(client, header, (int)strlen(header), 0);
    if (body_len > 0) {
        send(client, body, (int)body_len, 0);
    }
}

static void send_soap_response(SOCKET client, const char *action, const char *service, const char *xml_inner) {
    char response[2048];
    snprintf(response, sizeof(response),
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
             "  <s:Body>\r\n"
             "    <u:%sResponse xmlns:u=\"urn:schemas-upnp-org:service:%s:1\">\r\n"
             "      %s\r\n"
             "    </u:%sResponse>\r\n"
             "  </s:Body>\r\n"
             "</s:Envelope>\r\n",
             action, service, xml_inner ? xml_inner : "", action);

    send_http_response(client, 200, "text/xml; charset=\"utf-8\"", response);
}

static void stop_audio_stream(void);
static void start_audio_stream(void);

static void xml_unescape_uri(char *uri) {
    char *p = uri;
    while ((p = strstr(p, "&amp;")) != NULL) {
        memmove(p + 1, p + 5, strlen(p + 5) + 1);
        p++;
    }
}

static void extract_soap_action(const char *req, char *out_action, size_t max_len) {
    out_action[0] = '\0';
    char *sa = find_header_value(req, "SOAPACTION");
    if (sa) {
        char *hash = strchr(sa, '#');
        if (hash) {
            hash++;
            size_t i = 0;
            while (hash[i] && hash[i] != '"' && hash[i] != '\r' && hash[i] != '\n' && i < max_len - 1) {
                out_action[i] = hash[i];
                i++;
            }
            out_action[i] = '\0';
        }
    }
}

static void handle_soap_avtransport(SOCKET client, const char *req) {
    char action[64] = "";
    extract_soap_action(req, action, sizeof(action));

    const char *body = strstr(req, "\r\n\r\n");
    if (!body) body = req;

    if (strcmp(action, "SetNextAVTransportURI") == 0 || (!action[0] && strstr(body, "SetNextAVTransportURI"))) {
        const char *uri_tag = strstr(body, "<NextURI>");
        if (uri_tag) {
            const char *start = strchr(uri_tag, '>');
            if (start) {
                start++;
                const char *end_tag = strstr(start, "</");
                if (end_tag) {
                    size_t len = (size_t)(end_tag - start);
                    if (len < sizeof(g_next_uri)) {
                        memcpy(g_next_uri, start, len);
                        g_next_uri[len] = '\0';
                        xml_unescape_uri(g_next_uri);
                        cabin_log("UPnP: Queued NextURI: %s", g_next_uri);
                    }
                }
            }
        }
        send_soap_response(client, "SetNextAVTransportURI", "AVTransport", "");
    } else if (strcmp(action, "SetAVTransportURI") == 0 || (!action[0] && strstr(body, "SetAVTransportURI"))) {
        const char *uri_tag = strstr(body, "<CurrentURI>");
        if (uri_tag) {
            const char *start = strchr(uri_tag, '>');
            if (start) {
                start++;
                const char *end_tag = strstr(start, "</");
                if (end_tag) {
                    size_t len = (size_t)(end_tag - start);
                    if (len < sizeof(g_current_uri)) {
                        memcpy(g_current_uri, start, len);
                        g_current_uri[len] = '\0';
                        xml_unescape_uri(g_current_uri);
                        g_played_samples = 0;
                        cabin_log("UPnP: Received stream URI: %s", g_current_uri);
                    }
                }
            }
        }
        /* Try to extract track duration from DIDL-Lite metadata if present */
        const char *dur = strstr(body, "duration=&quot;");
        if (dur) {
            dur += 15;
        } else {
            dur = strstr(body, "duration=\"");
            if (dur) dur += 10;
        }
        if (dur) {
            char tmp_dur[32] = "";
            int i = 0;
            while (dur[i] && dur[i] != '&' && dur[i] != '"' && dur[i] != '.' && i < 15) {
                tmp_dur[i] = dur[i];
                i++;
            }
            tmp_dur[i] = '\0';
            if (strchr(tmp_dur, ':')) {
                if (tmp_dur[1] == ':') {
                    snprintf(g_track_duration, sizeof(g_track_duration), "0%s", tmp_dur);
                } else {
                    snprintf(g_track_duration, sizeof(g_track_duration), "%s", tmp_dur);
                }
            }
        }
        send_soap_response(client, "SetAVTransportURI", "AVTransport", "");
    } else if (strcmp(action, "Play") == 0) {
        if (g_transport_state != UPNP_STATE_PAUSED || !g_stream_active) {
            g_played_samples = 0;
            g_transport_state = UPNP_STATE_PLAYING;
            start_audio_stream();
        } else {
            g_transport_state = UPNP_STATE_PLAYING;
        }
        cabin_log("UPnP: Play action received. Overriding game multimedia radio.");
        send_soap_response(client, "Play", "AVTransport", "");
        notify_avtransport_change();
    } else if (strcmp(action, "Pause") == 0) {
        g_transport_state = UPNP_STATE_PAUSED;
        cabin_log("UPnP: Pause action received. Reverting to game radio.");
        send_soap_response(client, "Pause", "AVTransport", "");
        notify_avtransport_change();
    } else if (strcmp(action, "Stop") == 0) {
        g_transport_state = UPNP_STATE_STOPPED;
        stop_audio_stream();
        g_played_samples = 0;
        cabin_log("UPnP: Stop action received. Reverting to game radio.");
        send_soap_response(client, "Stop", "AVTransport", "");
        notify_avtransport_change();
    } else if (strcmp(action, "GetTransportInfo") == 0) {
        const char *state_str = "STOPPED";
        if (g_transport_state == UPNP_STATE_PLAYING) state_str = "PLAYING";
        else if (g_transport_state == UPNP_STATE_PAUSED) state_str = "PAUSED_PLAYBACK";

        char inner[256];
        snprintf(inner, sizeof(inner),
                 "<CurrentTransportState>%s</CurrentTransportState>\r\n"
                 "<CurrentTransportStatus>OK</CurrentTransportStatus>\r\n"
                 "<CurrentSpeed>1</CurrentSpeed>", state_str);
        send_soap_response(client, "GetTransportInfo", "AVTransport", inner);
    } else if (strcmp(action, "GetPositionInfo") == 0) {
        uint32_t total_sec = (uint32_t)(g_played_samples / 48000ULL);
        uint32_t hrs = total_sec / 3600u;
        uint32_t mins = (total_sec % 3600u) / 60u;
        uint32_t secs = total_sec % 60u;
        char rel_time[32];
        snprintf(rel_time, sizeof(rel_time), "%02u:%02u:%02u", hrs, mins, secs);

        char inner[768];
        snprintf(inner, sizeof(inner),
                 "<Track>1</Track>\r\n"
                 "<TrackDuration>%s</TrackDuration>\r\n"
                 "<TrackMetaData></TrackMetaData>\r\n"
                 "<TrackURI>%s</TrackURI>\r\n"
                 "<RelTime>%s</RelTime>\r\n"
                 "<AbsTime>%s</AbsTime>\r\n"
                 "<RelCount>2147483647</RelCount>\r\n"
                 "<AbsCount>2147483647</AbsCount>",
                 g_track_duration, g_current_uri, rel_time, rel_time);
        send_soap_response(client, "GetPositionInfo", "AVTransport", inner);
    } else if (strcmp(action, "GetMediaInfo") == 0) {
        char inner[768];
        snprintf(inner, sizeof(inner),
                 "<NrTracks>1</NrTracks>\r\n"
                 "<MediaDuration>%s</MediaDuration>\r\n"
                 "<CurrentURI>%s</CurrentURI>\r\n"
                 "<CurrentURIMetaData></CurrentURIMetaData>\r\n"
                 "<NextURI>%s</NextURI>\r\n"
                 "<NextURIMetaData></NextURIMetaData>\r\n"
                 "<PlayMedium>NETWORK</PlayMedium>\r\n"
                 "<RecordMedium>NOT_IMPLEMENTED</RecordMedium>\r\n"
                 "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>",
                 g_track_duration, g_current_uri, g_next_uri);
        send_soap_response(client, "GetMediaInfo", "AVTransport", inner);
    } else if (strcmp(action, "GetDeviceCapabilities") == 0) {
        const char *inner =
            "<PlayMedia>NETWORK</PlayMedia>\r\n"
            "<RecMedia>NOT_IMPLEMENTED</RecMedia>\r\n"
            "<RecQualityModes>NOT_IMPLEMENTED</RecQualityModes>";
        send_soap_response(client, "GetDeviceCapabilities", "AVTransport", inner);
    } else if (strcmp(action, "GetTransportSettings") == 0) {
        const char *inner =
            "<PlayMode>NORMAL</PlayMode>\r\n"
            "<RecQualityMode>NOT_IMPLEMENTED</RecQualityMode>";
        send_soap_response(client, "GetTransportSettings", "AVTransport", inner);
    } else if (strcmp(action, "GetCurrentTransportActions") == 0) {
        const char *inner = "<Actions>Play,Stop,Pause</Actions>";
        send_soap_response(client, "GetCurrentTransportActions", "AVTransport", inner);
    } else {
        cabin_log("UPnP: Unhandled AVTransport SOAP action: %s", action);
        send_soap_response(client, action[0] ? action : "GetTransportInfo", "AVTransport", "");
    }
}

static void handle_soap_rendering(SOCKET client, const char *req) {
    char action[64] = "";
    extract_soap_action(req, action, sizeof(action));

    const char *body = strstr(req, "\r\n\r\n");
    if (!body) body = req;

    if (strcmp(action, "SetVolume") == 0 || (!action[0] && strstr(body, "SetVolume"))) {
        const char *vol_tag = strstr(body, "<DesiredVolume");
        if (vol_tag) {
            const char *val = strchr(vol_tag, '>');
            if (val) {
                int vol = atoi(val + 1);
                if (vol < 0) vol = 0;
                if (vol > 100) vol = 100;
                g_upnp_volume_pct = vol;
                /* Natural quadratic audio taper: 50% slider = -12 dB (0.25 amplitude), 100% = 1.0 */
                float norm = (float)vol / 100.0f;
                g_upnp_volume = norm * norm;
                cabin_log("UPnP: SetVolume -> %d%% (gain=%.3f)", vol, g_upnp_volume);
            }
        }
        send_soap_response(client, "SetVolume", "RenderingControl", "");
        notify_rendering_control_change();
    } else if (strcmp(action, "GetVolume") == 0 || (!action[0] && strstr(body, "GetVolume"))) {
        char inner[128];
        snprintf(inner, sizeof(inner), "<CurrentVolume>%d</CurrentVolume>", g_upnp_volume_pct);
        send_soap_response(client, "GetVolume", "RenderingControl", inner);
    } else if (strcmp(action, "SetVolumeDB") == 0) {
        const char *vol_tag = strstr(body, "<DesiredVolume");
        if (vol_tag) {
            const char *val = strchr(vol_tag, '>');
            if (val) {
                int db_raw = atoi(val + 1);
                float db = (float)db_raw / 256.0f;
                if (db < -60.0f) {
                    g_upnp_volume = 0.0f;
                    g_upnp_volume_pct = 0;
                } else {
                    if (db > 0.0f) db = 0.0f;
                    g_upnp_volume = powf(10.0f, db / 20.0f);
                    g_upnp_volume_pct = (int)(sqrtf(g_upnp_volume) * 100.0f + 0.5f);
                }
                cabin_log("UPnP: SetVolumeDB -> %d (gain=%.3f)", db_raw, g_upnp_volume);
            }
        }
        send_soap_response(client, "SetVolumeDB", "RenderingControl", "");
        notify_rendering_control_change();
    } else if (strcmp(action, "GetVolumeDB") == 0) {
        int db_raw = (g_upnp_volume <= 0.0001f) ? -15360 : (int)(20.0f * log10f(g_upnp_volume) * 256.0f);
        char inner[128];
        snprintf(inner, sizeof(inner), "<CurrentVolume>%d</CurrentVolume>", db_raw);
        send_soap_response(client, "GetVolumeDB", "RenderingControl", inner);
    } else if (strcmp(action, "GetVolumeDBRange") == 0) {
        send_soap_response(client, "GetVolumeDBRange", "RenderingControl",
                           "<MinValue>-15360</MinValue><MaxValue>0</MaxValue>");
    } else if (strcmp(action, "SetMute") == 0 || (!action[0] && strstr(body, "SetMute"))) {
        const char *mute_tag = strstr(body, "<DesiredMute");
        if (mute_tag) {
            const char *val = strchr(mute_tag, '>');
            if (val) {
                val++;
                while (*val == ' ' || *val == '\t') val++;
                g_upnp_muted = (*val == '1' || *val == 't' || *val == 'T' || *val == 'y' || *val == 'Y');
                cabin_log("UPnP: SetMute -> %s", g_upnp_muted ? "MUTED" : "UNMUTED");
            }
        }
        send_soap_response(client, "SetMute", "RenderingControl", "");
        notify_rendering_control_change();
    } else if (strcmp(action, "GetMute") == 0 || (!action[0] && strstr(body, "GetMute"))) {
        char inner[128];
        snprintf(inner, sizeof(inner), "<CurrentMute>%d</CurrentMute>", g_upnp_muted ? 1 : 0);
        send_soap_response(client, "GetMute", "RenderingControl", inner);
    } else if (strcmp(action, "ListPresets") == 0 || (!action[0] && strstr(body, "ListPresets"))) {
        send_soap_response(client, "ListPresets", "RenderingControl",
                           "<CurrentPresetNameList>FactoryDefaults</CurrentPresetNameList>");
    } else if (strcmp(action, "SelectPreset") == 0 || (!action[0] && strstr(body, "SelectPreset"))) {
        send_soap_response(client, "SelectPreset", "RenderingControl", "");
    } else {
        send_soap_response(client, action[0] ? action : "GetVolume", "RenderingControl", "");
    }
}

static void handle_soap_connection(SOCKET client, const char *body) {
    if (strstr(body, "GetCurrentConnectionIDs")) {
        send_soap_response(client, "GetCurrentConnectionIDs", "ConnectionManager",
                           "<ConnectionIDs>0</ConnectionIDs>");
    } else if (strstr(body, "GetCurrentConnectionInfo")) {
        send_soap_response(client, "GetCurrentConnectionInfo", "ConnectionManager",
                           "<RcsID>0</RcsID><AVTransportID>0</AVTransportID><ProtocolInfo></ProtocolInfo>"
                           "<PeerConnectionManager></PeerConnectionManager><PeerConnectionID>-1</PeerConnectionID>"
                           "<Direction>Input</Direction><Status>OK</Status>");
    } else {
        const char *inner =
            "<Source></Source>\r\n"
            "<Sink>http-get:*:audio/flac:*,http-get:*:audio/x-flac:*,http-get:*:audio/mpeg:*,http-get:*:audio/mp3:*,http-get:*:audio/wav:*,http-get:*:audio/x-wav:*,http-get:*:audio/L16:*</Sink>";
        send_soap_response(client, "GetProtocolInfo", "ConnectionManager", inner);
    }
}

static unsigned __stdcall upnp_http_worker(void *unused) {
    (void)unused;
    static char buffer[16384];

    while (g_upnp_running && g_http_socket != INVALID_SOCKET) {
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        SOCKET client = accept(g_http_socket, (struct sockaddr *)&client_addr, &client_len);
        if (client == INVALID_SOCKET) {
            Sleep(20);
            continue;
        }

        int received = recv(client, buffer, sizeof(buffer) - 1, 0);
        if (received <= 0) {
            closesocket(client);
            continue;
        }
        buffer[received] = '\0';

        char req_method[16] = "";
        char req_url[256] = "";
        sscanf(buffer, "%15s %255s", req_method, req_url);
        cabin_log("UPnP: HTTP %s %s from %s", req_method, req_url, inet_ntoa(client_addr.sin_addr));

        /* GENA Event Subscription Support (with active LastChange initial NOTIFY) */
        if (strcmp(req_method, "SUBSCRIBE") == 0) {
            UpnpSubscription *sub = strstr(req_url, "RenderingControl") ? &g_sub_rc : &g_sub_avt;
            char *cb = find_header_value(buffer, "CALLBACK");
            if (cb) {
                const char *lt = strchr(cb, '<');
                const char *gt = lt ? strchr(lt + 1, '>') : NULL;
                if (lt && gt && (size_t)(gt - lt - 1) < sizeof(sub->callback_url)) {
                    size_t clen = (size_t)(gt - lt - 1);
                    memcpy(sub->callback_url, lt + 1, clen);
                    sub->callback_url[clen] = '\0';
                    sub->seq = 0;
                    sub->active = true;
                }
            } else {
                sub->active = true;
            }

            char response[512];
            snprintf(response, sizeof(response),
                     "HTTP/1.1 200 OK\r\n"
                     "Server: Windows/10.0 UPnP/1.0 ETS2CabinRadio/1.2\r\n"
                     "SID: %s\r\n"
                     "TIMEOUT: Second-1800\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: close\r\n\r\n",
                     sub->sid);
            send(client, response, (int)strlen(response), 0);
            closesocket(client);

            /* Immediately send initial GENA event state (required by UPnP spec for Volume/Transport UI) */
            if (sub == &g_sub_rc) {
                notify_rendering_control_change();
            } else {
                notify_avtransport_change();
            }
            continue;
        } else if (strcmp(req_method, "UNSUBSCRIBE") == 0) {
            if (strstr(req_url, "RenderingControl")) g_sub_rc.active = false;
            else g_sub_avt.active = false;

            char response[256];
            snprintf(response, sizeof(response),
                     "HTTP/1.1 200 OK\r\n"
                     "Server: Windows/10.0 UPnP/1.0 ETS2CabinRadio/1.2\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: close\r\n\r\n");
            send(client, response, (int)strlen(response), 0);
            closesocket(client);
            continue;
        }

        /* Ensure complete SOAP body is received across TCP packets */
        char *cl = find_header_value(buffer, "Content-Length");
        if (cl) {
            int body_target = atoi(cl);
            char *hdr_end = strstr(buffer, "\r\n\r\n");
            if (hdr_end) {
                int header_bytes = (int)(hdr_end - buffer) + 4;
                int current_body = received - header_bytes;
                while (current_body < body_target && received < (int)sizeof(buffer) - 1) {
                    int r = recv(client, buffer + received, (int)sizeof(buffer) - 1 - received, 0);
                    if (r <= 0) break;
                    received += r;
                    current_body += r;
                    buffer[received] = '\0';
                }
            }
        }

        if (strstr(req_url, "/description.xml")) {
            send_http_response(client, 200, "text/xml; charset=\"utf-8\"", kDeviceDescriptionXml);
        } else if (strstr(req_url, "/AVTransport.xml")) {
            send_http_response(client, 200, "text/xml; charset=\"utf-8\"", kAVTransportXml);
        } else if (strstr(req_url, "/RenderingControl.xml")) {
            send_http_response(client, 200, "text/xml; charset=\"utf-8\"", kRenderingControlXml);
        } else if (strstr(req_url, "/ConnectionManager.xml")) {
            send_http_response(client, 200, "text/xml; charset=\"utf-8\"", kConnectionManagerXml);
        } else if (strstr(req_url, "/control/AVTransport")) {
            handle_soap_avtransport(client, buffer);
        } else if (strstr(req_url, "/control/RenderingControl")) {
            handle_soap_rendering(client, buffer);
        } else if (strstr(req_url, "/control/ConnectionManager")) {
            handle_soap_connection(client, buffer);
        } else {
            send_http_response(client, 404, "text/plain", "Not Found");
        }

        closesocket(client);
    }
    return 0;
}

/* Audio HTTP Streaming Client & Decoder Worker */
static unsigned __stdcall upnp_stream_worker(void *unused) {
    (void)unused;
    t_my_gen = g_stream_gen;
    g_stream_active = true;

    char host[256] = "";
    int port = 80;
    char path[768] = "/";

    const char *p = g_current_uri;
    if (strncmp(p, "http://", 7) == 0) p += 7;

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    if (colon && (!slash || colon < slash)) {
        size_t host_len = (size_t)(colon - p);
        if (host_len < sizeof(host)) {
            memcpy(host, p, host_len);
            host[host_len] = '\0';
        }
        port = atoi(colon + 1);
    } else if (slash) {
        size_t host_len = (size_t)(slash - p);
        if (host_len < sizeof(host)) {
            memcpy(host, p, host_len);
            host[host_len] = '\0';
        }
    } else {
        snprintf(host, sizeof(host), "%s", p);
    }

    if (slash) {
        snprintf(path, sizeof(path), "%s", slash);
    }

    struct hostent *he = gethostbyname(host);
    if (!he) {
        cabin_log("UPnP: Could not resolve stream host %s", host);
        g_stream_active = false;
        return 0;
    }

    SOCKET stream_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (stream_sock == INVALID_SOCKET) {
        g_stream_active = false;
        return 0;
    }

    /* 256 KB OS-level TCP receive buffer for high-bitrate Wi-Fi WAV/FLAC streaming */
    int rcvbuf = 262144;
    setsockopt(stream_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));

    g_stream_sock = stream_sock;

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons((u_short)port);
    memcpy(&saddr.sin_addr, he->h_addr_list[0], sizeof(saddr.sin_addr));

    if (connect(stream_sock, (struct sockaddr *)&saddr, sizeof(saddr)) != 0) {
        cabin_log("UPnP: Connect failed to %s:%d", host, port);
        closesocket(stream_sock);
        if (g_stream_sock == stream_sock) g_stream_sock = INVALID_SOCKET;
        g_stream_active = false;
        return 0;
    }

    /* Request HTTP/1.0 to avoid chunked transfer encoding whenever possible */
    char req[1024];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\n"
             "Host: %s:%d\r\n"
             "User-Agent: ETS2CabinRadio/1.2\r\n"
             "Accept: */*\r\n"
             "Connection: close\r\n\r\n",
             path, host, port);
    send(stream_sock, req, (int)strlen(req), 0);

    /* Read HTTP headers until \r\n\r\n */
    char header_buf[4096];
    int header_pos = 0;
    while (header_pos < (int)sizeof(header_buf) - 1) {
        int r = recv(stream_sock, header_buf + header_pos, 1, 0);
        if (r <= 0) break;
        header_pos++;
        header_buf[header_pos] = '\0';
        if (header_pos >= 4 && strcmp(header_buf + header_pos - 4, "\r\n\r\n") == 0) {
            break;
        }
    }

    /* Log status response line */
    char status_line[128] = "";
    char *eol = strpbrk(header_buf, "\r\n");
    if (eol) {
        size_t sl_len = (size_t)(eol - header_buf);
        if (sl_len < sizeof(status_line)) {
            memcpy(status_line, header_buf, sl_len);
            status_line[sl_len] = '\0';
        }
    }
    cabin_log("UPnP: Stream HTTP response: %s", status_line[0] ? status_line : "Unknown");

    const char *ct = strstr(header_buf, "Content-Type:");
    if (!ct) ct = strstr(header_buf, "content-type:");
    char content_type[128] = "";
    if (ct) {
        ct += 13;
        while (*ct == ' ' || *ct == '\t') ct++;
        const char *e = strpbrk(ct, "\r\n;");
        size_t ct_len = e ? (size_t)(e - ct) : strlen(ct);
        if (ct_len < sizeof(content_type)) {
            memcpy(content_type, ct, ct_len);
            content_type[ct_len] = '\0';
        }
    }

    StreamSocket stream_src = {
        .sock = stream_sock,
        .is_chunked = false,
        .chunk_remaining = 0,
        .chunk_eof = false,
        .should_stop = stream_should_stop_cb,
        .peek_len = 0,
        .peek_pos = 0
    };

    if (strstr(header_buf, "Transfer-Encoding: chunked") ||
        strstr(header_buf, "transfer-encoding: chunked")) {
        stream_src.is_chunked = true;
        cabin_log("UPnP: HTTP stream uses Transfer-Encoding: chunked.");
    }

    /* Peek first 64 bytes to detect magic header alongside Content-Type */
    const uint8_t *magic_bytes = NULL;
    size_t magic_len = stream_peek(&stream_src, &magic_bytes);

    CodecFormat fmt = codec_detect_format(content_type, magic_bytes, magic_len);
    cabin_log("UPnP: Connected to %s:%d%s (format: %d, Content-Type: %s)",
              host, port, path, (int)fmt, content_type[0] ? content_type : "unspecified");

    codec_stream_and_decode(&stream_src, fmt, ring_push_sample_cb, stream_should_stop_cb);

    /* Wait for buffered samples to drain through FMOD before setting state to STOPPED */
    while (ring_available_read() > 0 && !stream_should_stop_cb()) {
        Sleep(25);
    }

    if (g_stream_sock == stream_sock) {
        g_stream_sock = INVALID_SOCKET;
    }
    closesocket(stream_sock);
    g_stream_active = false;

    /* Only update global state if this worker belongs to current active generation */
    if (t_my_gen == g_stream_gen && g_transport_state == UPNP_STATE_PLAYING) {
        g_transport_state = UPNP_STATE_STOPPED;
        notify_avtransport_change();
    }
    cabin_log("UPnP: Stream finished or disconnected.");
    return 0;
}

static void start_audio_stream(void) {
    stop_audio_stream();
    ring_clear();
    ResetEvent(g_stream_stop_event);
    InterlockedIncrement(&g_stream_gen);
    g_stream_thread = (HANDLE)_beginthreadex(NULL, 0, upnp_stream_worker, NULL, 0, NULL);
}

static void stop_audio_stream(void) {
    InterlockedIncrement(&g_stream_gen);
    SetEvent(g_stream_stop_event);
    SOCKET s = g_stream_sock;
    if (s != INVALID_SOCKET) {
        g_stream_sock = INVALID_SOCKET;
        closesocket(s);
    }
    if (g_stream_thread) {
        WaitForSingleObject(g_stream_thread, 500);
        CloseHandle(g_stream_thread);
        g_stream_thread = NULL;
    }
    ring_clear();
    g_stream_active = false;
}

/* Public API functions exported to LiveCabinDSP.c */
bool upnp_init(void) {
    if (g_upnp_initialized) return true;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        cabin_log("UPnP: WSAStartup failed.");
        return false;
    }

    g_stream_stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    detect_local_ip();
    g_upnp_initialized = true;
    return true;
}

void upnp_shutdown(void) {
    upnp_set_enabled(false);
    if (g_stream_stop_event) {
        CloseHandle(g_stream_stop_event);
        g_stream_stop_event = NULL;
    }
    if (g_upnp_initialized) {
        WSACleanup();
        g_upnp_initialized = false;
    }
}

void upnp_set_enabled(bool enabled) {
    if (g_upnp_enabled == enabled) return;
    g_upnp_enabled = enabled;

    if (enabled) {
        if (!g_upnp_initialized) upnp_init();
        g_upnp_running = true;

        /* SSDP UDP Multicast socket */
        g_ssdp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_ssdp_socket != INVALID_SOCKET) {
            BOOL reuse = TRUE;
            setsockopt(g_ssdp_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

            struct in_addr local_in;
            local_in.s_addr = inet_addr(g_local_ip);
            setsockopt(g_ssdp_socket, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&local_in, sizeof(local_in));

            struct sockaddr_in bind_addr;
            memset(&bind_addr, 0, sizeof(bind_addr));
            bind_addr.sin_family = AF_INET;
            bind_addr.sin_port = htons(UPNP_SSDP_PORT);
            bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

            if (bind(g_ssdp_socket, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) == 0) {
                struct ip_mreq mreq;
                mreq.imr_multiaddr.s_addr = inet_addr(UPNP_SSDP_MCAST);
                mreq.imr_interface.s_addr = inet_addr(g_local_ip);
                setsockopt(g_ssdp_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&mreq, sizeof(mreq));

                g_ssdp_thread = (HANDLE)_beginthreadex(NULL, 0, upnp_ssdp_worker, NULL, 0, NULL);
                cabin_log("UPnP: SSDP multicast listener started on %s:%d (%s)",
                          UPNP_SSDP_MCAST, UPNP_SSDP_PORT, g_local_ip);
            } else {
                cabin_log("UPnP: Failed to bind SSDP port %d (error %d)", UPNP_SSDP_PORT, WSAGetLastError());
            }
        }

        /* HTTP Server socket */
        g_http_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (g_http_socket != INVALID_SOCKET) {
            BOOL reuse = TRUE;
            setsockopt(g_http_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

            struct sockaddr_in http_addr;
            memset(&http_addr, 0, sizeof(http_addr));
            http_addr.sin_family = AF_INET;
            http_addr.sin_port = htons(UPNP_HTTP_PORT);
            http_addr.sin_addr.s_addr = htonl(INADDR_ANY);

            if (bind(g_http_socket, (struct sockaddr *)&http_addr, sizeof(http_addr)) == 0 &&
                listen(g_http_socket, 8) == 0) {
                g_http_thread = (HANDLE)_beginthreadex(NULL, 0, upnp_http_worker, NULL, 0, NULL);
                cabin_log("UPnP: MediaRenderer HTTP listening on http://%s:%d/description.xml",
                          g_local_ip, UPNP_HTTP_PORT);
            } else {
                cabin_log("UPnP: Failed to bind HTTP port %d (error %d)", UPNP_HTTP_PORT, WSAGetLastError());
            }
        }
    } else {
        if (g_ssdp_socket != INVALID_SOCKET) {
            broadcast_ssdp_byebye(g_ssdp_socket);
        }
        g_upnp_running = false;
        stop_audio_stream();

        if (g_ssdp_socket != INVALID_SOCKET) {
            closesocket(g_ssdp_socket);
            g_ssdp_socket = INVALID_SOCKET;
        }
        if (g_http_socket != INVALID_SOCKET) {
            closesocket(g_http_socket);
            g_http_socket = INVALID_SOCKET;
        }

        if (g_ssdp_thread) {
            WaitForSingleObject(g_ssdp_thread, 500);
            CloseHandle(g_ssdp_thread);
            g_ssdp_thread = NULL;
        }
        if (g_http_thread) {
            WaitForSingleObject(g_http_thread, 500);
            CloseHandle(g_http_thread);
            g_http_thread = NULL;
        }

        cabin_log("UPnP: MediaRenderer disabled and sockets closed.");
    }
}

bool upnp_is_active(void) {
    return g_upnp_enabled && g_upnp_running;
}

bool upnp_is_playing(void) {
    return g_upnp_enabled && (g_transport_state == UPNP_STATE_PLAYING);
}

bool upnp_get_sample(float *out_left, float *out_right) {
    if (!upnp_is_playing()) {
        if (out_left) *out_left = 0.0f;
        if (out_right) *out_right = 0.0f;
        return false;
    }

    uint32_t avail = ring_available_read();

    /* Pre-buffering cushion: wait for ~250ms of audio before starting or after an underrun */
    if (g_prebuffering) {
        if (avail < UPNP_PREBUFFER_FRAMES && g_stream_active) {
            if (out_left) *out_left = 0.0f;
            if (out_right) *out_right = 0.0f;
            return false;
        }
        g_prebuffering = false;
    }

    if (avail < 1) {
        if (g_stream_active) {
            /* Buffer underrun over Wi-Fi: re-engage pre-buffer cushion! */
            g_prebuffering = true;
        }
        if (out_left) *out_left = 0.0f;
        if (out_right) *out_right = 0.0f;
        return false;
    }

    uint32_t rp = g_ring.read_pos;
    float l = g_ring.left[rp];
    float r = g_ring.right[rp];
    MemoryBarrier();
    g_ring.read_pos = (rp + 1u) & UPNP_RING_MASK;
    g_played_samples++;

    if (g_upnp_muted) {
        l = 0.0f;
        r = 0.0f;
    } else {
        l *= g_upnp_volume;
        r *= g_upnp_volume;
    }

    if (out_left) *out_left = l;
    if (out_right) *out_right = r;
    return true;
}

#else

/* Non-Windows stubs */
bool upnp_init(void) { return false; }
void upnp_shutdown(void) {}
void upnp_set_enabled(bool enabled) { (void)enabled; }
bool upnp_is_active(void) { return false; }
bool upnp_is_playing(void) { return false; }
bool upnp_get_sample(float *out_left, float *out_right) {
    if (out_left) *out_left = 0.0f;
    if (out_right) *out_right = 0.0f;
    return false;
}

#endif