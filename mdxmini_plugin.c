///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// MDXmini Playback Plugin
//
// Implements RVPlaybackPlugin interface for Sharp X68000 MDX music format using mdxmini library.
// MDX uses YM2151 (OPM) FM synthesis + ADPCM drums. Companion .PDX files provide PCM drum samples.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// C11 nullptr compatibility
#ifndef nullptr
#define nullptr ((void*)0)
#endif

#include <retrovert/io.h>
#include <retrovert/log.h>
#include <retrovert/metadata.h>
#include <retrovert/playback.h>
#include <retrovert/service.h>

#include "mdxmini.h"
#include "title_encoding.h"
#include "ym2151.h"

#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define MDX_SAMPLE_RATE 44100

RV_PLUGIN_USE_LOG_API();
RV_PLUGIN_USE_METADATA_API();
RV_PLUGIN_USE_IO_API();

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct MdxReplayerData {
    t_mdxmini mdx;
    int initialized;
    int length_sec;
    int elapsed_frames;
    bool scope_enabled;
} MdxReplayerData;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Extract the optional PDX filename from the MDX header. The name follows the
// title terminator (CR LF SUB) and is NUL-terminated.
static bool mdxmini_pdx_name(const uint8_t* data, uint64_t size, char* out, size_t out_size) {
    uint64_t pos = 0;
    size_t name_len = 0;

    if (data == nullptr || out == nullptr || out_size < 5) {
        return false;
    }

    while (pos + 2 < size && !(data[pos] == 0x0d && data[pos + 1] == 0x0a && data[pos + 2] == 0x1a)) {
        pos++;
    }
    if (pos + 3 >= size) {
        return false;
    }
    pos += 3;

    while (pos < size && data[pos] != 0 && name_len + 1 < out_size) {
        out[name_len++] = (char)data[pos++];
    }
    if (pos >= size || data[pos] != 0 || name_len == 0) {
        return false;
    }
    out[name_len] = '\0';

    if (name_len < 4 || strcasecmp(out + name_len - 4, ".pdx") != 0) {
        if (name_len + 4 >= out_size) {
            return false;
        }
        memcpy(out + name_len, ".pdx", 4);
        name_len += 4;
    }
    out[name_len] = '\0';
    return true;
}

static char* mdxmini_sidecar_url(const char* url, const char* pdx_name) {
    const char* last_sep = strrchr(url, '/');
    const char* last_backslash = strrchr(url, '\\');
    size_t prefix_len;
    size_t name_len = strlen(pdx_name);
    char* sidecar_url;

    if (last_backslash != nullptr && (last_sep == nullptr || last_backslash > last_sep)) {
        last_sep = last_backslash;
    }
    prefix_len = last_sep == nullptr ? 0 : (size_t)(last_sep - url) + 1;
    if (prefix_len > SIZE_MAX - name_len - 1) {
        return nullptr;
    }

    sidecar_url = malloc(prefix_len + name_len + 1);
    if (sidecar_url == nullptr) {
        return nullptr;
    }
    memcpy(sidecar_url, url, prefix_len);
    memcpy(sidecar_url + prefix_len, pdx_name, name_len + 1);
    return sidecar_url;
}

static int mdxmini_open_url(t_mdxmini* mdx, const char* url) {
    RVIoReadUrlResult mdx_file = rv_io_read_url_to_memory(url);
    RVIoReadUrlResult pdx_file = { nullptr, 0 };
    char pdx_name[MDX_MAX_PDX_FILENAME_LENGTH];
    char* pdx_url = nullptr;
    int result;

    if (mdx_file.data == nullptr || mdx_file.data_size == 0 || mdx_file.data_size > LONG_MAX) {
        rv_error("MDXmini: failed to read %s through RVIo", url);
        if (mdx_file.data != nullptr) {
            rv_io_free_url_to_memory(mdx_file.data);
        }
        return -1;
    }

    if (mdxmini_pdx_name(mdx_file.data, mdx_file.data_size, pdx_name, sizeof(pdx_name))) {
        pdx_url = mdxmini_sidecar_url(url, pdx_name);
        if (pdx_url != nullptr) {
            pdx_file = rv_io_read_url_to_memory(pdx_url);
            if (pdx_file.data != nullptr && (pdx_file.data_size == 0 || pdx_file.data_size > LONG_MAX)) {
                rv_error("MDXmini: invalid PDX size for %s", pdx_url);
                rv_io_free_url_to_memory(pdx_file.data);
                pdx_file = (RVIoReadUrlResult) { nullptr, 0 };
            }
        }
    }

    result = mdx_open_memory(mdx, mdx_file.data, (long)mdx_file.data_size, pdx_file.data,
                             (long)pdx_file.data_size);
    if (pdx_file.data != nullptr) {
        rv_io_free_url_to_memory(pdx_file.data);
    }
    rv_io_free_url_to_memory(mdx_file.data);
    free(pdx_url);
    return result;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static const char* mdxmini_plugin_supported_extensions(void) {
    return "mdx";
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void* mdxmini_plugin_create(const RVService* service_api) {
    MdxReplayerData* data = malloc(sizeof(MdxReplayerData));
    if (data == nullptr) {
        return nullptr;
    }
    memset(data, 0, sizeof(MdxReplayerData));

    return data;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int mdxmini_plugin_destroy(void* user_data) {
    MdxReplayerData* data = (MdxReplayerData*)user_data;

    if (data->initialized) {
        mdx_close(&data->mdx);
    }

    free(data);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int mdxmini_plugin_open(void* user_data, const char* url, uint32_t subsong, const RVService* service_api) {
    (void)subsong;
    (void)service_api;

    MdxReplayerData* data = (MdxReplayerData*)user_data;

    // Close previous if any
    if (data->initialized) {
        mdx_close(&data->mdx);
        data->initialized = 0;
    }

    memset(&data->mdx, 0, sizeof(t_mdxmini));
    if (mdxmini_open_url(&data->mdx, url) < 0) {
        rv_error("MDXmini: failed to open %s", url);
        return -1;
    }

    data->initialized = 1;

    // Set loop count to 2 for reasonable playback length
    mdx_set_max_loop(&data->mdx, 2);

    // Get song duration
    data->length_sec = mdx_get_length(&data->mdx);
    data->elapsed_frames = 0;

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void mdxmini_plugin_close(void* user_data) {
    MdxReplayerData* data = (MdxReplayerData*)user_data;

    if (data->initialized) {
        mdx_close(&data->mdx);
        data->initialized = 0;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVProbeResult mdxmini_plugin_probe_can_play(uint8_t* probe_data, uint64_t data_size, const char* url,
                                                   uint64_t total_size) {
    (void)probe_data;
    (void)data_size;
    (void)total_size;

    // MDX has no reliable magic bytes - detect by extension only
    if (url != nullptr) {
        const char* dot = strrchr(url, '.');
        if (dot != nullptr && strcasecmp(dot, ".mdx") == 0) {
            return RVProbeResult_Unsure;
        }
    }

    return RVProbeResult_Unsupported;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVReadInfo mdxmini_plugin_read_data(void* user_data, RVReadData dest) {
    MdxReplayerData* data = (MdxReplayerData*)user_data;
    RVAudioFormat format = { RVAudioStreamFormat_S16, 2, MDX_SAMPLE_RATE };

    if (!data->initialized) {
        return (RVReadInfo) { format, 0, RVReadStatus_Error};
    }

    // Check if song ended (based on duration)
    if (data->length_sec > 0 && data->elapsed_frames / MDX_SAMPLE_RATE >= data->length_sec) {
        return (RVReadInfo) { format, 0, RVReadStatus_Finished};
    }

    // Calculate how many S16 stereo frames fit in the output buffer
    uint32_t capacity_frames = dest.channels_output_max_bytes_size / (sizeof(int16_t) * 2);
    uint32_t max_frames = dest.info.frame_count < capacity_frames ? dest.info.frame_count : capacity_frames;

    // mdx_calc_sample outputs interleaved stereo S16 directly to output buffer
    mdx_calc_sample(&data->mdx, (int16_t*)dest.channels_output, (int)max_frames);

    data->elapsed_frames += (int)max_frames;

    // Check if song ended after rendering
    RVReadStatus status = RVReadStatus_Ok;
    if (data->length_sec > 0 && data->elapsed_frames / MDX_SAMPLE_RATE >= data->length_sec) {
        status = RVReadStatus_Finished;
    }

    return (RVReadInfo) { format, (uint32_t)max_frames, status};
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int64_t mdxmini_plugin_seek(void* user_data, int64_t ms) {
    (void)user_data;
    (void)ms;

    // mdxmini has no native seek - would require close+reopen+skip frames
    // Return -1 to indicate seek is not supported
    return -1;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int mdxmini_plugin_metadata(const char* url, const RVService* service_api) {
    (void)service_api;

    // Open the MDX file to extract metadata
    t_mdxmini mdx;
    memset(&mdx, 0, sizeof(t_mdxmini));

    if (mdxmini_open_url(&mdx, url) < 0) {
        return -1;
    }

    mdx_set_max_loop(&mdx, 2);

    RVMetadataId index = rv_metadata_create_url(url);

    // MDX stores titles as CP932/Shift-JIS; the metadata API expects UTF-8.
    char shift_jis_title[MDX_MAX_TITLE_LENGTH];
    char utf8_title[MDX_MAX_TITLE_LENGTH * 3 + 1];
    memset(shift_jis_title, 0, sizeof(shift_jis_title));
    mdx_get_title(&mdx, shift_jis_title);
    if (shift_jis_title[0] != '\0'
        && mdx_title_to_utf8(shift_jis_title, utf8_title, sizeof(utf8_title)) == 0) {
        rv_metadata_set_tag(index, RV_METADATA_TITLE_TAG, utf8_title);
    }

    // Set song type
    rv_metadata_set_tag(index, RV_METADATA_SONGTYPE_TAG, "MDX");
    rv_metadata_set_tag(index, RV_METADATA_AUTHORINGTOOL_TAG, "Sharp X68000");

    // Get duration
    int length = mdx_get_length(&mdx);
    if (length > 0) {
        rv_metadata_set_tag_f64(index, RV_METADATA_LENGTH_TAG, (double)length);
    }

    mdx_close(&mdx);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void mdxmini_plugin_event(void* user_data, uint8_t* event_data, uint64_t len) {
    (void)user_data;
    (void)event_data;
    (void)len;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void mdxmini_plugin_static_init(const RVService* service_api) {
    rv_init_io_api(service_api);
    rv_init_log_api(service_api);
    rv_init_metadata_api(service_api);
    mdx_set_rate(MDX_SAMPLE_RATE);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static bool mdxmini_plugin_get_structure(void* user_data, RVVizInfo* out) {
    MdxReplayerData* data = (MdxReplayerData*)user_data;
    if (data == nullptr || out == nullptr) {
        return false;
    }

    out->caps = RVVizCaps_Scope;
    out->scroll_mode = RVScrollMode_Synchronized;
    out->pattern_channel_count = 0;
    out->scope_channel_count = 8; // YM2151: 8 FM channels
    out->column_count = 0;
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t mdxmini_plugin_get_scope_channels(void* user_data, RVChannelDesc* out, uint32_t cap) {
    (void)user_data;
    if (out == nullptr) {
        return 0;
    }

    static const char* s_names[] = { "FM 1", "FM 2", "FM 3", "FM 4", "FM 5", "FM 6", "FM 7", "FM 8" };
    uint32_t count = 8;
    if (count > cap)
        count = cap;
    for (uint32_t i = 0; i < count; i++) {
        memset(out[i].name, 0, sizeof(out[i].name));
        snprintf((char*)out[i].name, sizeof(out[i].name), "%s", s_names[i]);
        out[i].scope_width = 0;
    }
    return count;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void mdxmini_plugin_set_scope_enabled(void* user_data, bool on) {
    MdxReplayerData* data = (MdxReplayerData*)user_data;
    if (data == nullptr) {
        return;
    }

    void* chip = YM2151GetLastChip();
    if (chip != nullptr) {
        YM2151EnableScope(chip, on ? 1 : 0);
    }
    data->scope_enabled = on;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t mdxmini_plugin_get_scope_samples(void* user_data, int32_t channel, float* out, uint32_t cap) {
    MdxReplayerData* data = (MdxReplayerData*)user_data;
    if (data == nullptr || !data->initialized || out == nullptr || !data->scope_enabled) {
        return 0;
    }

    void* chip = YM2151GetLastChip();
    if (chip == nullptr) {
        return 0;
    }

    return YM2151GetScopeData(chip, channel, out, cap);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVPlaybackPlugin g_mdxmini_plugin = {
    RV_PLAYBACK_PLUGIN_API_VERSION,
    "mdxmini",
    "0.0.1",
    "mdxmini 2.0.0",
    mdxmini_plugin_probe_can_play,
    mdxmini_plugin_supported_extensions,
    mdxmini_plugin_create,
    mdxmini_plugin_destroy,
    mdxmini_plugin_event,
    mdxmini_plugin_open,
    mdxmini_plugin_close,
    mdxmini_plugin_read_data,
    mdxmini_plugin_seek,
    mdxmini_plugin_metadata,
    mdxmini_plugin_static_init,
    nullptr, // settings_updated
    nullptr, // static_destroy

    // Visualization: scope-only (YM2151 FM channels, no pattern grid).
    mdxmini_plugin_get_structure,
    nullptr, // get_columns
    nullptr, // get_pattern_channels
    mdxmini_plugin_get_scope_channels,
    nullptr, // get_position
    nullptr, // get_channel_rows
    nullptr, // get_cells
    mdxmini_plugin_set_scope_enabled,
    mdxmini_plugin_get_scope_samples,
    nullptr, // get_vu
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_mdxmini_plugin;
}
