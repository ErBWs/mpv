/*
 * OHAudio audio output driver.
 *
 * Copyright (C) 2025 Bao Han <erbws@foxmail.com>
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ao.h"
#include "internal.h"
#include "common/msg.h"
#include "audio/format.h"
#include "options/m_option.h"
#include "osdep/threads.h"
#include "osdep/timer.h"

#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostreambuilder.h>

struct priv {
    OH_AudioStreamBuilder* builder;
    OH_AudioRenderer *renderer;
    int64_t last_timestamp;
    int64_t current_latency;
    int64_t target_latency;

    int encoding_type;
};

static void uninit(struct ao* ao)
{
    struct priv* p = ao->priv;

    if (p->renderer) {
        OH_AudioRenderer_Stop(p->renderer);
        OH_AudioRenderer_Release(p->renderer);
        p->renderer = NULL;
    }

    if (p->builder) {
        OH_AudioStreamBuilder_Destroy(p->builder);
        p->builder = NULL;
    }

    p->last_timestamp = 0;
    p->current_latency = p->target_latency;
}

static OH_AudioData_Callback_Result audio_on_write_callback(
    OH_AudioRenderer* renderer,
    void* userData,
    void* audioData,
    int32_t audioDataSize)
{
    struct ao* ao = userData;
    struct priv* p = ao->priv;

    int64_t now = mp_time_ns();
    int64_t frame, pos, timestamp;

    // OH_AudioRenderer_GetAudioTimestampInfo() cannot be called too frequently
    if (p->last_timestamp == 0 || now - p->last_timestamp > 500000000) {
        OH_AudioRenderer_GetFramesWritten(renderer, &frame);
        if (OH_AudioRenderer_GetAudioTimestampInfo(renderer, &pos, &timestamp) > 0)
            pos = frame;
        p->target_latency = frame - pos;
        p->last_timestamp = now;
    }

    // Fix video freeze caused by sudden change of latency
    if (p->target_latency - p->current_latency > 300)
        p->current_latency += 300;
    else if (p->current_latency - p->target_latency > 300)
        p->current_latency -= 300;
    else
        p->current_latency = p->target_latency;

    int sample = audioDataSize / (ao->channels.num * af_fmt_to_bytes(ao->format));
    int64_t delay = MP_TIME_S_TO_NS(sample) / ao->samplerate;
    delay += MP_TIME_S_TO_NS(p->current_latency) / ao->samplerate;

    ao_read_data(ao, &audioData, sample,
        mp_time_ns() + delay, NULL, true, true);

    return AUDIO_DATA_CALLBACK_RESULT_VALID;
}

static int init(struct ao* ao)
{
    struct priv* p = ao->priv;
    OH_AudioStream_Result res = 0;

    res = OH_AudioStreamBuilder_Create(&p->builder, AUDIOSTREAM_TYPE_RENDERER);

    if (res != AUDIOSTREAM_SUCCESS) {
        MP_ERR(ao, "Fail to create audio stream: error code %d.\n", res);
        goto error;
    }

    ao->channels.num = MPCLAMP(ao->channels.num, 1, 16);
    ao->samplerate = MPCLAMP(ao->samplerate, 8000, 192000);

    OH_AudioStreamBuilder_SetChannelCount(p->builder, ao->channels.num);
    OH_AudioStreamBuilder_SetSamplingRate(p->builder, ao->samplerate);
    OH_AudioStreamBuilder_SetEncodingType(p->builder, p->encoding_type);
    OH_AudioStreamBuilder_SetRendererInfo(p->builder, AUDIOSTREAM_USAGE_MOVIE);

    if (af_fmt_is_int(ao->format)) {
        if (af_fmt_to_bytes(ao->format) > 2)
            ao->format = AF_FORMAT_S32;
        else
            ao->format = af_fmt_from_planar(ao->format);
    } else {
        ao->format = AF_FORMAT_FLOAT;
    }

    switch (ao->format) {
    case AF_FORMAT_U8:
        OH_AudioStreamBuilder_SetSampleFormat(p->builder, AUDIOSTREAM_SAMPLE_U8);
        break;
    case AF_FORMAT_S16:
        OH_AudioStreamBuilder_SetSampleFormat(p->builder, AUDIOSTREAM_SAMPLE_S16LE);
        break;
    case AF_FORMAT_S32:
        OH_AudioStreamBuilder_SetSampleFormat(p->builder, AUDIOSTREAM_SAMPLE_S32LE);
        break;
    case AF_FORMAT_FLOAT:
        OH_AudioStreamBuilder_SetSampleFormat(p->builder, AUDIOSTREAM_SAMPLE_F32LE);
        break;
    default:
        OH_AudioStreamBuilder_SetSampleFormat(p->builder, AUDIOSTREAM_SAMPLE_S16LE);
        break;
    }

    OH_AudioRenderer_OnWriteDataCallback callback = audio_on_write_callback;
    OH_AudioStreamBuilder_SetRendererWriteDataCallback(p->builder, callback, ao);

    res = OH_AudioStreamBuilder_GenerateRenderer(p->builder, &p->renderer);

    if (res != AUDIOSTREAM_SUCCESS) {
        MP_ERR(ao, "Fail to generate audio renderer: error code %d.\n", res);
        goto error;
    }

    return 0;
error:
    uninit(ao);
    return -1;
}

static int control(struct ao *ao, enum aocontrol cmd, void *arg)
{
    struct priv *p = ao->priv;
    float volume;
    float *vol = arg;
    OH_AudioStream_Result res = 0;

    switch (cmd) {
    case AOCONTROL_GET_VOLUME:
        res = OH_AudioRenderer_GetVolume(p->renderer, &volume);
        if (res != AUDIOSTREAM_SUCCESS) {
            MP_ERR(ao, "Fail to get volume, error code %d.\n", res);
            return CONTROL_ERROR;
        }
        *vol = volume * 100;
        break;
    case AOCONTROL_SET_VOLUME:
        res = OH_AudioRenderer_SetVolume(p->renderer, *vol / 100);
        if (res != AUDIOSTREAM_SUCCESS) {
            MP_ERR(ao, "Fail to set volume, error code %d.\n", res);
            return CONTROL_ERROR;
        }
        break;
    default:
        return CONTROL_UNKNOWN;
    }
    return CONTROL_OK;
}

static void reset(struct ao *ao)
{
    struct priv *p = ao->priv;

    OH_AudioRenderer_Stop(p->renderer);
    OH_AudioRenderer_Flush(p->renderer);
}

static bool set_pause(struct ao *ao, bool paused)
{
    struct priv *p = ao->priv;

    if (paused) {
        OH_AudioRenderer_Pause(p->renderer);
    } else {
        OH_AudioRenderer_Start(p->renderer);
    }
    return true;
}

static void start(struct ao *ao)
{
    struct priv *p = ao->priv;

    OH_AudioRenderer_Start(p->renderer);
}

#define OPT_BASE_STRUCT struct priv

const struct ao_driver audio_out_ohaudio = {
    .description = "OHAudio audio output",
    .name      = "ohaudio",
    .init      = init,
    .control   = control,
    .uninit    = uninit,
    .reset     = reset,
    .set_pause = set_pause,
    .start     = start,

    .priv_size = sizeof(struct priv),
    .priv_defaults = &(const struct priv) {
        .encoding_type = AUDIOSTREAM_ENCODING_TYPE_RAW,
        .last_timestamp = 0,
        .current_latency = 0,
        .target_latency = 0,
    },
    .options = (const struct m_option[]) {
        {"encoding_type", OPT_INT(encoding_type),
            M_RANGE(0, 2)},
        {0}
    },
    .options_prefix = "ohaudio",
};
