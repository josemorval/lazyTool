#include "audio.h"
#include "dx11_ctx.h"
#include "resources.h"
#include "user_cb.h"
#include "log.h"
#include <mmsystem.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "winmm.lib")

struct AudioCBData {
    uint32_t sample_start;
    uint32_t sample_count;
    uint32_t sample_rate;
    uint32_t channels;
    float    time_seconds;
    float    duration_seconds;
    float    master_volume;
    uint32_t loop;
};

static const AudioSettings k_audio_defaults = {
    /* enabled          */ false,
    /* shader           */ INVALID_HANDLE,
    /* sample_rate      */ 48000,
    /* duration_seconds */ 60.0f,
    /* master_volume    */ 0.35f,
    /* loop             */ false
};

AudioSettings g_audio_settings = k_audio_defaults;

static const int LT_AUDIO_CHUNK_SAMPLES = 2048;
static const int LT_AUDIO_BUFFER_COUNT = 2;

struct AudioWaveBuffer {
    WAVEHDR hdr;
    unsigned char* bytes;
    bool prepared;
    bool queued;
};

static bool s_audio_initialized = false;
static bool s_stream_started = false;
static bool s_reset_requested = false;
static bool s_audio_finished = false;
static float s_reset_seconds = 0.0f;
static char s_status[160] = "disabled";
static unsigned long long s_sample_cursor = 0;

static HWAVEOUT s_wave = nullptr;
static AudioWaveBuffer s_wave_buffers[LT_AUDIO_BUFFER_COUNT] = {};
static int s_wave_buffer_count = 0;

static ID3D11Buffer* s_audio_cb = nullptr;
static ID3D11Buffer* s_audio_out = nullptr;
static ID3D11UnorderedAccessView* s_audio_out_uav = nullptr;
static ID3D11Buffer* s_audio_staging = nullptr;

static int s_cached_sample_rate = 0;
static float s_cached_duration_seconds = 0.0f;
static ResHandle s_cached_shader = INVALID_HANDLE;
static bool s_cached_loop = false;

static void audio_set_status(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status, sizeof(s_status), fmt, ap);
    va_end(ap);
    s_status[sizeof(s_status) - 1] = '\0';
}

static int audio_clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static unsigned long long audio_total_samples() {
    if (g_audio_settings.sample_rate <= 0)
        return 0;
    double total = (double)g_audio_settings.duration_seconds * (double)g_audio_settings.sample_rate;
    if (total < 1.0)
        total = 1.0;
    return (unsigned long long)(total + 0.5);
}

static void audio_sanitize_settings() {
    g_audio_settings.sample_rate = audio_clamp_int(g_audio_settings.sample_rate, 8000, 192000);
    g_audio_settings.duration_seconds = clampf(g_audio_settings.duration_seconds, 1.0f, 3600.0f);
    g_audio_settings.master_volume = clampf(g_audio_settings.master_volume, 0.0f, 1.0f);
}

static void audio_release_gpu() {
    if (s_audio_staging) { s_audio_staging->Release(); s_audio_staging = nullptr; }
    if (s_audio_out_uav) { s_audio_out_uav->Release(); s_audio_out_uav = nullptr; }
    if (s_audio_out) { s_audio_out->Release(); s_audio_out = nullptr; }
    if (s_audio_cb) { s_audio_cb->Release(); s_audio_cb = nullptr; }
}

static void audio_reset_wave_queue() {
    if (s_wave)
        waveOutReset(s_wave);
    for (int i = 0; i < s_wave_buffer_count; i++)
        s_wave_buffers[i].queued = false;
    s_stream_started = false;
}

static void audio_release_wave() {
    if (s_wave)
        waveOutReset(s_wave);

    for (int i = 0; i < LT_AUDIO_BUFFER_COUNT; i++) {
        AudioWaveBuffer& b = s_wave_buffers[i];
        if (s_wave && b.prepared)
            waveOutUnprepareHeader(s_wave, &b.hdr, sizeof(b.hdr));
        if (b.bytes) {
            free(b.bytes);
            b.bytes = nullptr;
        }
        memset(&b.hdr, 0, sizeof(b.hdr));
        b.prepared = false;
        b.queued = false;
    }

    if (s_wave) {
        waveOutClose(s_wave);
        s_wave = nullptr;
    }
    s_wave_buffer_count = 0;
    s_stream_started = false;
}

static void audio_release_all() {
    audio_release_wave();
    audio_release_gpu();
    s_cached_sample_rate = 0;
    s_cached_duration_seconds = 0.0f;
    s_cached_shader = INVALID_HANDLE;
    s_cached_loop = false;
    s_audio_finished = false;
}

static bool audio_settings_changed() {
    return s_cached_sample_rate != g_audio_settings.sample_rate ||
           fabsf(s_cached_duration_seconds - g_audio_settings.duration_seconds) > 0.0001f ||
           s_cached_shader != g_audio_settings.shader ||
           s_cached_loop != g_audio_settings.loop;
}

static bool audio_shader_ready(Resource** out_shader) {
    Resource* shader = res_get(g_audio_settings.shader);
    if (!shader) {
        audio_set_status("select an audio shader");
        return false;
    }
    if (shader->type != RES_SHADER || !shader->audio_shader ||
        shader->shader_kind != SHADER_PROGRAM_CS ||
        !shader->cs || shader->using_fallback) {
        audio_set_status("shader must be a compiled audio shader");
        return false;
    }
    if (out_shader)
        *out_shader = shader;
    return true;
}

static bool audio_create_gpu_resources() {
    if (!g_dx.dev)
        return false;

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = (sizeof(AudioCBData) + 15u) & ~15u;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRESULT hr = g_dx.dev->CreateBuffer(&cbd, nullptr, &s_audio_cb);
    if (FAILED(hr) || !s_audio_cb) {
        audio_set_status("AudioCB create failed");
        return false;
    }

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = (UINT)(LT_AUDIO_CHUNK_SAMPLES * 4);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 4;
    hr = g_dx.dev->CreateBuffer(&bd, nullptr, &s_audio_out);
    if (FAILED(hr) || !s_audio_out) {
        audio_set_status("audio output buffer create failed");
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = (UINT)LT_AUDIO_CHUNK_SAMPLES;
    hr = g_dx.dev->CreateUnorderedAccessView(s_audio_out, &ud, &s_audio_out_uav);
    if (FAILED(hr) || !s_audio_out_uav) {
        audio_set_status("audio output UAV create failed");
        return false;
    }

    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = g_dx.dev->CreateBuffer(&bd, nullptr, &s_audio_staging);
    if (FAILED(hr) || !s_audio_staging) {
        audio_set_status("audio staging buffer create failed");
        return false;
    }
    return true;
}

static bool audio_open_wave() {
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = (DWORD)g_audio_settings.sample_rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)((wf.nChannels * wf.wBitsPerSample) / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

    MMRESULT mm = waveOutOpen(&s_wave, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL);
    if (mm != MMSYSERR_NOERROR || !s_wave) {
        audio_set_status("waveOutOpen failed (%u)", (unsigned)mm);
        return false;
    }

    s_wave_buffer_count = LT_AUDIO_BUFFER_COUNT;
    DWORD bytes = (DWORD)(LT_AUDIO_CHUNK_SAMPLES * 4);
    for (int i = 0; i < s_wave_buffer_count; i++) {
        AudioWaveBuffer& b = s_wave_buffers[i];
        memset(&b, 0, sizeof(b));
        b.bytes = (unsigned char*)calloc(1, bytes);
        if (!b.bytes) {
            audio_set_status("audio buffer allocation failed");
            return false;
        }
        b.hdr.lpData = (LPSTR)b.bytes;
        b.hdr.dwBufferLength = bytes;
        mm = waveOutPrepareHeader(s_wave, &b.hdr, sizeof(b.hdr));
        if (mm != MMSYSERR_NOERROR) {
            audio_set_status("waveOutPrepareHeader failed (%u)", (unsigned)mm);
            return false;
        }
        b.prepared = true;
    }
    return true;
}

static bool audio_ensure_ready(float scene_time_seconds) {
    audio_sanitize_settings();

    Resource* shader = nullptr;
    if (!audio_shader_ready(&shader)) {
        audio_release_all();
        return false;
    }

    if (audio_settings_changed())
        audio_release_all();

    if (!s_audio_cb || !s_audio_out || !s_audio_out_uav || !s_audio_staging) {
        if (!audio_create_gpu_resources()) {
            audio_release_all();
            return false;
        }
    }

    if (!s_wave) {
        if (!audio_open_wave()) {
            audio_release_all();
            return false;
        }
    }

    s_cached_sample_rate = g_audio_settings.sample_rate;
    s_cached_duration_seconds = g_audio_settings.duration_seconds;
    s_cached_shader = g_audio_settings.shader;
    s_cached_loop = g_audio_settings.loop;

    if (!s_stream_started) {
        if (scene_time_seconds < 0.0f)
            scene_time_seconds = 0.0f;
        s_sample_cursor = (unsigned long long)(scene_time_seconds * (float)g_audio_settings.sample_rate + 0.5f);
        if (g_audio_settings.loop) {
            unsigned long long total = audio_total_samples();
            if (total > 0 && s_sample_cursor >= total)
                s_sample_cursor %= total;
        }
        s_stream_started = true;
        s_audio_finished = false;
    }

    (void)shader;
    return true;
}

static bool audio_generate_segment(unsigned char* dst, uint32_t sample_start, uint32_t sample_count) {
    Resource* shader = nullptr;
    if (!dst || sample_count == 0 || !audio_shader_ready(&shader) || !g_dx.ctx)
        return false;

    AudioCBData cb = {};
    cb.sample_start = sample_start;
    cb.sample_count = sample_count;
    cb.sample_rate = (uint32_t)g_audio_settings.sample_rate;
    cb.channels = 2;
    cb.time_seconds = (float)((double)sample_start / (double)g_audio_settings.sample_rate);
    cb.duration_seconds = g_audio_settings.duration_seconds;
    cb.master_volume = g_audio_settings.master_volume;
    cb.loop = g_audio_settings.loop ? 1u : 0u;

    D3D11_MAPPED_SUBRESOURCE ms = {};
    HRESULT hr = g_dx.ctx->Map(s_audio_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    if (FAILED(hr)) {
        audio_set_status("AudioCB map failed");
        return false;
    }
    memcpy(ms.pData, &cb, sizeof(cb));
    g_dx.ctx->Unmap(s_audio_cb, 0);

    ID3D11ShaderResourceView* null_srvs[MAX_SRV_SLOTS] = {};
    ID3D11UnorderedAccessView* null_uavs[MAX_UAV_SLOTS] = {};
    g_dx.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    g_dx.ctx->CSSetShaderResources(0, MAX_SRV_SLOTS, null_srvs);
    g_dx.ctx->CSSetUnorderedAccessViews(0, MAX_UAV_SLOTS, null_uavs, nullptr);

    if (g_dx.scene_cb)
        g_dx.ctx->CSSetConstantBuffers(0, 1, &g_dx.scene_cb);
    if (g_user_cb_buf)
        g_dx.ctx->CSSetConstantBuffers(2, 1, &g_user_cb_buf);
    g_dx.ctx->CSSetConstantBuffers(3, 1, &s_audio_cb);

    g_dx.ctx->CSSetUnorderedAccessViews(0, 1, &s_audio_out_uav, nullptr);
    g_dx.ctx->CSSetShader(shader->cs, nullptr, 0);
    UINT groups = (UINT)((sample_count + 255u) / 256u);
    if (groups < 1)
        groups = 1;
    g_dx.ctx->Dispatch(groups, 1, 1);

    ID3D11UnorderedAccessView* clear_uav = nullptr;
    ID3D11Buffer* clear_cb = nullptr;
    g_dx.ctx->CSSetShader(nullptr, nullptr, 0);
    g_dx.ctx->CSSetUnorderedAccessViews(0, 1, &clear_uav, nullptr);
    g_dx.ctx->CSSetConstantBuffers(3, 1, &clear_cb);

    g_dx.ctx->CopyResource(s_audio_staging, s_audio_out);

    D3D11_MAPPED_SUBRESOURCE read = {};
    hr = g_dx.ctx->Map(s_audio_staging, 0, D3D11_MAP_READ, 0, &read);
    if (FAILED(hr)) {
        audio_set_status("audio staging map failed");
        return false;
    }
    memcpy(dst, read.pData, (size_t)sample_count * 4u);
    g_dx.ctx->Unmap(s_audio_staging, 0);
    return true;
}

static bool audio_generate_chunk(unsigned char* dst) {
    if (!dst)
        return false;

    memset(dst, 0, (size_t)LT_AUDIO_CHUNK_SAMPLES * 4u);

    unsigned long long total = audio_total_samples();
    if (total == 0)
        return false;

    uint32_t generated = 0;
    while (generated < (uint32_t)LT_AUDIO_CHUNK_SAMPLES) {
        if (g_audio_settings.loop) {
            if (s_sample_cursor >= total)
                s_sample_cursor %= total;
        } else if (s_sample_cursor >= total) {
            break;
        }

        unsigned long long samples_to_end = total - s_sample_cursor;
        uint32_t want = (uint32_t)LT_AUDIO_CHUNK_SAMPLES - generated;
        uint32_t segment_count = want;
        if (samples_to_end < (unsigned long long)segment_count)
            segment_count = (uint32_t)samples_to_end;
        if (segment_count == 0)
            break;

        if (!audio_generate_segment(dst + (size_t)generated * 4u, (uint32_t)s_sample_cursor, segment_count))
            return false;

        s_sample_cursor += (unsigned long long)segment_count;
        generated += segment_count;

        if (!g_audio_settings.loop && s_sample_cursor >= total)
            break;
    }

    if (generated == 0)
        return false;
    s_audio_finished = false;
    return true;
}

static bool audio_submit_buffer(AudioWaveBuffer& b) {
    if (!s_wave || !b.prepared || !b.bytes)
        return false;

    if (!audio_generate_chunk(b.bytes))
        return false;

    MMRESULT mm = waveOutWrite(s_wave, &b.hdr, sizeof(b.hdr));
    if (mm != MMSYSERR_NOERROR) {
        audio_set_status("waveOutWrite failed (%u)", (unsigned)mm);
        audio_reset_wave_queue();
        return false;
    }
    b.queued = true;
    return true;
}

void audio_init() {
    s_audio_initialized = true;
    audio_set_status("disabled");
}

void audio_shutdown() {
    audio_release_all();
    s_audio_initialized = false;
}

const AudioSettings& audio_default_settings() {
    return k_audio_defaults;
}

void audio_reset_settings() {
    audio_stop();
    g_audio_settings = k_audio_defaults;
}

void audio_request_reset(float scene_time_seconds) {
    if (scene_time_seconds < 0.0f)
        scene_time_seconds = 0.0f;
    s_reset_requested = true;
    s_reset_seconds = scene_time_seconds;
    s_audio_finished = false;
}

void audio_stop() {
    audio_reset_wave_queue();
    if (g_audio_settings.enabled)
        audio_set_status("stopped");
    else
        audio_set_status("disabled");
}

void audio_update(float scene_time_seconds, bool scene_running) {
    if (!s_audio_initialized)
        return;

    audio_sanitize_settings();

    if (s_reset_requested) {
        audio_reset_wave_queue();
        s_sample_cursor = (unsigned long long)(s_reset_seconds * (float)g_audio_settings.sample_rate + 0.5f);
        if (g_audio_settings.loop) {
            unsigned long long total = audio_total_samples();
            if (total > 0 && s_sample_cursor >= total)
                s_sample_cursor %= total;
        }
        s_audio_finished = false;
        s_reset_requested = false;
    }

    if (!g_audio_settings.enabled) {
        audio_stop();
        return;
    }

    if (!scene_running) {
        audio_stop();
        audio_set_status("paused");
        return;
    }

    if (!audio_ensure_ready(scene_time_seconds))
        return;

    unsigned long long total = audio_total_samples();
    int queued = 0;
    for (int i = 0; i < s_wave_buffer_count; i++) {
        AudioWaveBuffer& b = s_wave_buffers[i];
        if (b.queued && (b.hdr.dwFlags & WHDR_DONE))
            b.queued = false;
        if (b.queued)
            queued++;
    }

    if (!g_audio_settings.loop && s_sample_cursor >= total) {
        if (queued > 0) {
            audio_set_status("finishing (%d queued)", queued);
        } else {
            s_audio_finished = true;
            audio_set_status("finished");
        }
        return;
    }

    for (int i = 0; i < s_wave_buffer_count; i++) {
        AudioWaveBuffer& b = s_wave_buffers[i];
        if (b.queued)
            continue;
        if (!g_audio_settings.loop && s_sample_cursor >= total)
            break;
        if (!audio_submit_buffer(b))
            return;
        queued++;
    }

    if (!g_audio_settings.loop && s_sample_cursor >= total)
        audio_set_status("finishing (%d queued)", queued);
    else
        audio_set_status("running (%d queued)", queued);
}

bool audio_running() {
    return s_wave != nullptr && s_stream_started && g_audio_settings.enabled && !s_audio_finished;
}

const char* audio_status() {
    return s_status;
}

int audio_queued_buffer_count() {
    int queued = 0;
    for (int i = 0; i < s_wave_buffer_count; i++)
        if (s_wave_buffers[i].queued && !(s_wave_buffers[i].hdr.dwFlags & WHDR_DONE))
            queued++;
    return queued;
}

float audio_latency_seconds() {
    if (g_audio_settings.sample_rate <= 0)
        return 0.0f;
    return (float)(audio_queued_buffer_count() * LT_AUDIO_CHUNK_SAMPLES) /
           (float)g_audio_settings.sample_rate;
}

unsigned long long audio_sample_cursor() {
    return s_sample_cursor;
}
