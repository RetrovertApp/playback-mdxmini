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

#include <retrovert/log.h>
#include <retrovert/metadata.h>
#include <retrovert/playback.h>
#include <retrovert/service.h>

#include "mdxmini.h"
#include "ym2151.h"

#include <stdlib.h>
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

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct MdxReplayerData {
    t_mdxmini mdx;
    int initialized;
    int length_sec;
    int elapsed_frames;
    bool scope_enabled;
} MdxReplayerData;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Extract the directory path from a URL/filepath

static void extract_directory(const char* path, char* dir, size_t dir_size) {
    const char* last_sep = strrchr(path, '/');
#ifdef _WIN32
    const char* last_bsep = strrchr(path, '\\');
    if (last_bsep != nullptr && (last_sep == nullptr || last_bsep > last_sep)) {
        last_sep = last_bsep;
    }
#endif
    if (last_sep != nullptr) {
        size_t len = (size_t)(last_sep - path);
        if (len >= dir_size) {
            len = dir_size - 1;
        }
        memcpy(dir, path, len);
        dir[len] = '\0';
    } else {
        dir[0] = '.';
        dir[1] = '\0';
    }
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

    // Extract directory for PDX sample loading
    char dir[2048];
    extract_directory(url, dir, sizeof(dir));

    // mdx_open takes a filesystem path directly
    memset(&data->mdx, 0, sizeof(t_mdxmini));
    if (mdx_open(&data->mdx, (char*)url, dir) < 0) {
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
    uint32_t max_frames = dest.channels_output_max_bytes_size / (sizeof(int16_t) * 2);

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

    char dir[2048];
    extract_directory(url, dir, sizeof(dir));

    if (mdx_open(&mdx, (char*)url, dir) < 0) {
        return -1;
    }

    mdx_set_max_loop(&mdx, 2);

    RVMetadataId index = rv_metadata_create_url(url);

    // Get title (may be Shift-JIS encoded)
    char title[256];
    memset(title, 0, sizeof(title));
    mdx_get_title(&mdx, title);
    if (title[0] != '\0') {
        rv_metadata_set_tag(index, RV_METADATA_TITLE_TAG, title);
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
    rv_init_log_api(service_api);
    rv_init_metadata_api(service_api);
    mdx_set_rate(MDX_SAMPLE_RATE);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t mdxmini_plugin_get_scope_data(void* user_data, int channel, float* buffer, uint32_t num_samples) {
    MdxReplayerData* data = (MdxReplayerData*)user_data;
    if (data == nullptr || !data->initialized || buffer == nullptr) {
        return 0;
    }

    void* chip = YM2151GetLastChip();
    if (chip == nullptr) {
        return 0;
    }

    if (!data->scope_enabled) {
        YM2151EnableScope(chip, 1);
        data->scope_enabled = true;
    }

    return YM2151GetScopeData(chip, channel, buffer, num_samples);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t mdxmini_plugin_get_scope_channel_names(void* user_data, const char** names, uint32_t max_channels) {
    (void)user_data;
    static const char* s_names[] = { "FM 1", "FM 2", "FM 3", "FM 4", "FM 5", "FM 6", "FM 7", "FM 8" };
    uint32_t count = 8;
    if (count > max_channels)
        count = max_channels;
    for (uint32_t i = 0; i < count; i++)
        names[i] = s_names[i];
    return count;
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
    nullptr, // get_tracker_info
    nullptr, // get_pattern_cell
    nullptr, // get_pattern_num_rows
    mdxmini_plugin_get_scope_data,
    nullptr, // static_destroy
    mdxmini_plugin_get_scope_channel_names,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_mdxmini_plugin;
}
