/*
 * JACK Audio Connection Kit input device
 * Copyright (c) 2021 falkTX
 * Author: Filipe Coelho <falktx@falktx.com>
 * Based on jack.c by Olivier Guilyardi <olivier samalyse com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"
#include <semaphore.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>

#include "libavutil/fifo.h"
#include "libavutil/time.h"
#include "libavformat/avformat.h"
#include "libavformat/internal.h"
#include "avdevice.h"

/**
 * Maximum number of "packets" of audio to queue in ringbuffer
 */
#define RINGBUFFER_NUM_PACKETS 16

typedef struct JackData {
    AVClass*           class;
    jack_client_t*     client;
    sem_t              packet_count;
    jack_nframes_t     sample_rate;
    jack_nframes_t     buffer_size;
    jack_port_t **     ports;
    int                nports;
    AVFifoBuffer *     new_pkts;
    int                pkt_xrun;
    int                jack_xrun;
    jack_nframes_t     audio_pkt_size;
    float*             audio_pkt;
    jack_ringbuffer_t* ringbuffer;
} JackData;

static int process_callback(jack_nframes_t nframes, void *arg)
{
    /* Warning: this function runs in realtime. One mustn't allocate memory here
     * or do any other thing that could block. */

    int i, j;
    JackData *self = arg;
    float* audio_pkt = self->audio_pkt;
    float* buffer;

    if (!self->client) {
        return 0;
    }

    /* Check if audio is available */
    if (jack_ringbuffer_read_space(self->ringbuffer) < self->audio_pkt_size) {
        for (i = 0; i < self->nports; i++) {
            buffer = jack_port_get_buffer(self->ports[i], nframes);
            memset(buffer, 0, sizeof(float)*nframes);
        }
        self->pkt_xrun = 1;
        return 0;
    }

    /* Retrieve audio */
    jack_ringbuffer_read(self->ringbuffer, (char*)audio_pkt, self->audio_pkt_size);

    /* Copy and interleave audio data from the packet into the JACK buffer */
    for (i = 0; i < self->nports; i++) {
        buffer = jack_port_get_buffer(self->ports[i], nframes);

        for (j = 0; j < nframes; j++)
            buffer[j] = audio_pkt[j * self->nports + i];
    }

    sem_post(&self->packet_count);
    return 0;
}

static void shutdown_callback(void *arg)
{
    JackData *self = arg;
    self->client = NULL;
}

static int xrun_callback(void *arg)
{
    JackData *self = arg;
    self->jack_xrun = 1;
    return 0;
}

static int start_jack(AVFormatContext *context)
{
    JackData *self = context->priv_data;
    int i;

#ifdef __MOD_DEVICES__
    if (self->nports != 1) {
        av_log(context, AV_LOG_ERROR, "Mono layout only\n");
        return AVERROR(EIO);
    }
#endif

    /* Register as a JACK client, using the context url as client name. */
    self->client = jack_client_open(context->url, JackNullOption, NULL);
    if (!self->client) {
        av_log(context, AV_LOG_ERROR, "Unable to register as a JACK client\n");
        return AVERROR(EIO);
    }

    self->ports = av_malloc_array(self->nports, sizeof(*self->ports));
    if (!self->ports)
        return AVERROR(ENOMEM);

    self->sample_rate    = jack_get_sample_rate(self->client);
    self->buffer_size    = jack_get_buffer_size(self->client);
    self->audio_pkt_size = self->nports * self->buffer_size * sizeof(float);
    self->ringbuffer     = jack_ringbuffer_create(self->audio_pkt_size * RINGBUFFER_NUM_PACKETS);

    self->audio_pkt = av_malloc_array(self->audio_pkt_size, sizeof(float));
    if (!self->audio_pkt) {
        return AVERROR(ENOMEM);
    }

    sem_init(&self->packet_count, 0, 0);

    /* Register JACK ports */
    for (i = 0; i < self->nports; i++) {
#ifdef __MOD_DEVICES__
        char* str = context->url;
        self->ports[i] = jack_port_register(self->client, context->url,
                                            JACK_DEFAULT_AUDIO_TYPE,
                                            JackPortIsOutput|JackPortIsTerminal|JackPortIsPhysical, 0);
#else
        char str[16];
        snprintf(str, sizeof(str), "output_%d", i + 1);
        self->ports[i] = jack_port_register(self->client, str,
                                            JACK_DEFAULT_AUDIO_TYPE,
                                            JackPortIsOutput, 0);
#endif
        if (!self->ports[i]) {
            av_log(context, AV_LOG_ERROR, "Unable to register port %s:%s\n",
                   context->url, str);
            jack_client_close(self->client);
            return AVERROR(EIO);
        }
    }

    /* Register JACK callbacks */
    jack_set_process_callback(self->client, process_callback, self);
    jack_on_shutdown(self->client, shutdown_callback, self);
    jack_set_xrun_callback(self->client, xrun_callback, self);

    if (!jack_activate(self->client)) {
        av_log(context, AV_LOG_INFO,
               "JACK client registered and activated (rate=%dHz, buffer_size=%d frames)\n",
               self->sample_rate, self->buffer_size);
    } else {
        av_log(context, AV_LOG_ERROR, "Unable to activate JACK client\n");
        return AVERROR(EIO);
    }

    return 0;
}

static void stop_jack(JackData *self)
{
    if (self->client) {
        jack_deactivate(self->client);
        jack_client_close(self->client);
    }
    sem_destroy(&self->packet_count);
    av_freep(&self->audio_pkt);
    av_freep(&self->ports);
    jack_ringbuffer_free(self->ringbuffer);
}

static av_cold int audio_write_header(AVFormatContext *context)
{
    JackData *self = context->priv_data;
    AVStream *stream = NULL;
    int test;

    if (context->nb_streams != 1 || context->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        av_log(stream, AV_LOG_ERROR, "Only a single audio stream is supported.\n");
        return AVERROR(EINVAL);
    }
    stream = context->streams[0];

    if ((test = start_jack(context)))
        return test;

    if (self->sample_rate != stream->codecpar->sample_rate) {
        av_log(stream, AV_LOG_ERROR,
               "sample rate %d not available, must use %d\n",
               stream->codecpar->sample_rate, self->sample_rate);
        stop_jack(self);
        return AVERROR(EIO);
    }

    avpriv_set_pts_info(stream, 64, 1, 1000000);  /* 64 bits pts in us */
    return 0;
}

static int audio_write_packet(AVFormatContext *context, AVPacket *pkt)
{
    JackData *self = context->priv_data;
    struct timespec timeout = {0, 0};
    AVPacket pkt2;

    /* Check if there's enough space to send everything as-is */
    if (jack_ringbuffer_write_space(self->ringbuffer) >= pkt->size) {
        jack_ringbuffer_write(self->ringbuffer, pkt->data, pkt->size);

    /* not everything fits, keep writing and waiting until the entire packet is in the ringbuffer */
    } else {
        timeout.tv_sec = av_gettime() / 1000000 + 2;
        memcpy(&pkt2, pkt, sizeof(pkt2));

        while (pkt2.size) {
            if (pkt2.size >= self->audio_pkt_size) {
                /* write one pkt size chunk at a time */
                if (jack_ringbuffer_write_space(self->ringbuffer) >= self->audio_pkt_size) {
                    jack_ringbuffer_write(self->ringbuffer, pkt2.data, self->audio_pkt_size);
                    pkt2.data += self->audio_pkt_size;
                    pkt2.size -= self->audio_pkt_size;
                } else {
                    if (sem_timedwait(&self->packet_count, &timeout)) {
                        if (errno == ETIMEDOUT) {
                            av_log(context, AV_LOG_ERROR,
                                   "Input error: timed outzz when waiting for JACK process callback input\n");
                        } else {
                            char errbuf[128];
                            int ret = AVERROR(errno);
                            av_strerror(ret, errbuf, sizeof(errbuf));
                            av_log(context, AV_LOG_ERROR, "Error while waiting for audio packet: %s\n",
                                   errbuf);
                        }
                        if (!self->client) {
                            av_log(context, AV_LOG_ERROR, "Input error: JACK server is gone\n");
                        }

                        return AVERROR(EIO);
                    }
                }
            } else {
                /* final step, only a few samples remain, we just spin spin */
                /* FIXME would it be okay to do timed wait here? processing side will post */
                while (jack_ringbuffer_write_space(self->ringbuffer) < pkt2.size) {}
                jack_ringbuffer_write(self->ringbuffer, pkt2.data, pkt2.size);
                break;
            }
        }
    }

    if (self->pkt_xrun) {
        av_log(context, AV_LOG_WARNING, "Audio source packet underrun\n");
        self->pkt_xrun = 0;
    }

    if (self->jack_xrun) {
        av_log(context, AV_LOG_WARNING, "JACK output xrun\n");
        self->jack_xrun = 0;
    }

    return 0;
}

static int audio_write_trailer(AVFormatContext *context)
{
    JackData *self = context->priv_data;
    stop_jack(self);
    return 0;
}

#define OFFSET(x) offsetof(JackData, x)
static const AVOption options[] = {
    { "channels", "Number of audio channels.", OFFSET(nports), AV_OPT_TYPE_INT, { .i64 = 2 }, 1, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },
    { NULL },
};

static const AVClass jack_outdev_class = {
    .class_name     = "JACK outdev",
    .item_name      = av_default_item_name,
    .option         = options,
    .version        = LIBAVUTIL_VERSION_INT,
    .category       = AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT,
};

AVOutputFormat ff_jack_muxer = {
    .name           = "jack",
    .long_name      = NULL_IF_CONFIG_SMALL("JACK audio output"),
    .priv_data_size = sizeof(JackData),
    /* XXX: we make the assumption that the soundcard accepts this format */
    /* XXX: find better solution with "preinit" method, needed also in
       other formats */
    .audio_codec    = AV_NE(AV_CODEC_ID_PCM_F32BE, AV_CODEC_ID_PCM_F32LE),
    .video_codec    = AV_CODEC_ID_NONE,
    .write_header   = audio_write_header,
    .write_packet   = audio_write_packet,
    .write_trailer  = audio_write_trailer,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &jack_outdev_class,
};
