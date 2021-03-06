#include <stdlib.h>
#include <stdio.h>
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/time.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include "videoutils.h"


#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1025)
#define AV_NOSYNC_THRESHOLD 10.0
#define AV_SYNC_THRESHOLD_MIN 0.04
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated(means add diff to delay, instead to 2 * delay) to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1

static uint32_t FF_QUIT_EVENT = 0;
static uint32_t FF_REFRESH_EVENT = 0;
static uint32_t FF_ALLOC_EVENT = 0;

static int audio_out_linesize = -1;
static AVFrame *audioFrame = NULL;
static int audio_channels = 0;
static uint8_t *out_buffer = NULL;
static SDL_Window *sdlWindow = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *texture = NULL;
static SDL_mutex *sdlWindow_alloc_mutex = NULL;
static SDL_cond *sdlWindow_alloc_cond = NULL;

VideoState *global_video_state;

int audio_decode_frame(VideoState *vs, uint8_t *audio_buf, int buf_size, double *pts_ptr);
void audio_callback(void *userdata, Uint8 *stream, int len);
void clearAtExit(void);
int decode_thread(void *userdata);
void video_refresh_timer(void *userdata);
static void schedule_refresh(VideoState *vs, int delay);
static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque);
int decode_interrupt_cb(void *);
int stream_component_open(VideoState *vs, enum AVMediaType type);
void stream_component_close(VideoState *vs, enum AVMediaType type);
int allocate_sdlwindow(void *userdata);
double get_audio_clock(VideoState *vs);
double synchronize_video(VideoState *vs, AVFrame *src_frame, double pts);
void stream_seek(VideoState *is, int64_t pos, int rel);
int open_codec_context(VideoState *vs, int *stream_idx, AVCodecContext **dec_ctx, enum AVMediaType type);

int main (int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage:./tutorial-sdl2-player videoFileName\n");
        return -1;
    }
    if (SDL_Init(SDL_INIT_EVERYTHING) != 0) {
        fprintf(stderr, "SDL_Init Error:%s", SDL_GetError());
        return -1;
    }
    atexit(clearAtExit);

    sdlWindow_alloc_mutex = SDL_CreateMutex();
    sdlWindow_alloc_cond = SDL_CreateCond();

    FF_QUIT_EVENT = SDL_RegisterEvents(3);
    if (FF_QUIT_EVENT == ((Uint32) - 1))
    {
        //not enough user-defined events left, reset to zero
        FF_QUIT_EVENT = 0;
    }
    FF_REFRESH_EVENT = FF_QUIT_EVENT + 1;
    FF_ALLOC_EVENT = FF_QUIT_EVENT + 2;

    //video state init
    VideoState *vs;
    vs = av_mallocz(sizeof(VideoState));
    av_strlcpy(vs->filename, argv[1], sizeof(vs->filename));
    vs->pictq_mutex = SDL_CreateMutex();
    vs->pictq_cond = SDL_CreateCond();
    vs->quit = 0;
    vs->videoStreamIndex = -1;
    vs->audioStreamIndex = -1;
    int pictq_index = 0;
    for (pictq_index = 0; pictq_index < VIDEO_PICTURE_QUEUE_SIZE; pictq_index++) {
        vs->pict_q[pictq_index].pictYUV = NULL;
    }
    //flush packet init
    av_init_packet(&flush_pkt);
    flush_pkt.data= (unsigned char *)("FLUSH");

    schedule_refresh(vs, 40);

    vs->parse_tid = SDL_CreateThread(decode_thread, "decode_thread", vs);
    if (!vs->parse_tid) {
        av_free(vs);
        return -1;
    }

    SDL_Event event;
    SDL_zero(event);
    for(;;) {
        double incr,pos;
        incr=pos=0;
        SDL_WaitEvent(&event);
        if (FF_REFRESH_EVENT == event.type) {
                video_refresh_timer(event.user.data1);
        } else if (FF_QUIT_EVENT == event.type) {
                vs->quit = 1;
                break;
        } else if (SDL_QUIT == event.type) {
            fprintf(stderr, "receive SDL QUIT event");
            vs->quit = 1;
            break;
        } else if (FF_ALLOC_EVENT == event.type) {
            if (allocate_sdlwindow(event.user.data1) < 0) {
                vs->quit = 1;
                break;
            }
        } else if (SDL_KEYDOWN == event.type) {
            switch(event.key.keysym.sym) {
                case SDLK_LEFT:
                    incr = -10.0;
                    goto do_seek;
                case SDLK_RIGHT:
                    incr = 10.0;
                    goto do_seek;
                case SDLK_UP:
                    incr = 60.0;
                    goto do_seek;
                case SDLK_DOWN:
                    incr = -60.0;
                    goto do_seek;
                do_seek:
                    if (global_video_state) {
                        pos = get_audio_clock(global_video_state);
                        pos += incr;
                        stream_seek(global_video_state,
                                        (int64_t)(pos * AV_TIME_BASE), incr);
                    }
                    break;
                default:
                    break;
                }
            
        }
    }

    exit:
        SDL_CondBroadcast(vs->pictq_cond);
        SDL_CondBroadcast(vs->audioq.cond);
        SDL_CondBroadcast(vs->videoq.cond);
        SDL_CondBroadcast(sdlWindow_alloc_cond);
        SDL_WaitThread(vs->parse_tid, NULL);
    
    if (texture) {
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(sdlWindow);
    }
    SDL_Quit();

	return 0;
}

int decode_thread(void *userdata) {
    VideoState *vs = (VideoState *)userdata;
    global_video_state = vs;
    int ret = 0;
    //deprecated since ffmpeg 4.0.
	//av_register_all(); //Do Nothing. You can just omit this function call in ffmpeg 4.0 and later.

    if (avformat_open_input(&(vs->formatCtx), vs->filename, NULL, NULL) < 0) {
        ret = -1;
        goto fail;
    }
    vs->formatCtx->interrupt_callback.callback = decode_interrupt_cb;
    vs->formatCtx->interrupt_callback.opaque = vs;

    if (avformat_find_stream_info(vs->formatCtx, NULL) < 0) {
        ret = -1;
        goto fail;
    }
    av_dump_format(vs->formatCtx, 0, vs->filename, 0);
    
    if (open_codec_context(vs, &(vs->videoStreamIndex), &(vs->videoCodecCtx), AVMEDIA_TYPE_VIDEO) < 0) {
        ret = -1;
        goto fail;
    }
    if (open_codec_context(vs, &(vs->audioStreamIndex), &(vs->audioCodecCtx), AVMEDIA_TYPE_AUDIO) < 0) {
        ret = -1;
        goto fail;
    }
    if (stream_component_open(vs, AVMEDIA_TYPE_VIDEO) < 0) {
        ret = -1;
        goto fail;
    }
    if (stream_component_open(vs, AVMEDIA_TYPE_AUDIO) < 0) {
        ret = -1;
        goto fail;
    }
    
    SDL_Event alloc_event;
    alloc_event.type = FF_ALLOC_EVENT;
    alloc_event.user.data1 = vs;
    SDL_PushEvent(&alloc_event);
    SDL_LockMutex(sdlWindow_alloc_mutex);
    SDL_CondWait(sdlWindow_alloc_cond, sdlWindow_alloc_mutex);
    SDL_UnlockMutex(sdlWindow_alloc_mutex);

    AVPacket packet;
    av_init_packet(&packet);
    packet.data = NULL;
    packet.size = 0;

    for(;;) {
        if (vs->quit) {
            break;
        }

        if (vs->seek_req) {
            int stream_index = -1;
            int64_t seek_target = vs->seek_pos;

            if (vs->videoStreamIndex >= 0) {
                stream_index = vs->videoStreamIndex;
            } else if (vs->audioStreamIndex >= 0) {
                stream_index = vs->audioStreamIndex;
            }

            if (stream_index >= 0) {
                seek_target = av_rescale_q(seek_target, AV_TIME_BASE_Q, vs->formatCtx->streams[stream_index]->time_base);
            }

            if (av_seek_frame(vs->formatCtx, stream_index, seek_target, vs->seek_flags) < 0) {
                fprintf(stderr, "%s: error while seeking \n", vs->filename);
            } else {
                if (vs->audioStreamIndex >= 0) {
                    packet_queue_flush(&vs->audioq);
                    packet_queue_put(&vs->audioq, &flush_pkt);
                }

                if (vs->videoStreamIndex >= 0) {
                    packet_queue_flush(&vs->videoq);
                    packet_queue_put(&vs->videoq, &flush_pkt);
                }
            }
            vs->seek_req = 0;
        }

        if (vs->audioq.size > MAX_AUDIOQ_SIZE ||
                vs->videoq.size > MAX_VIDEOQ_SIZE) {
            SDL_Delay(10);
            continue;
        }
        

        if (av_read_frame(vs->formatCtx, &packet) < 0) {
            if (vs->formatCtx->pb->error == 0) {
                SDL_Delay(100);
                continue;
            } else {
                break;
            }
            
        }

        if (packet.stream_index == vs->videoStreamIndex) {
            packet_queue_put(&vs->videoq, &packet);
        } else if (packet.stream_index == vs->audioStreamIndex) {
            packet_queue_put(&vs->audioq, &packet);
        } else {
            av_packet_unref(&packet);
        }

       
    }

    while(!vs->quit) {
        SDL_Delay(100);
    }

    ret = 0;

    fail:
        if (vs->audio_stm) {
            stream_component_close(vs, AVMEDIA_TYPE_AUDIO);
        }

        if (vs->video_stm) {
            stream_component_close(vs, AVMEDIA_TYPE_VIDEO);
        }

        if (vs->formatCtx) {
            avformat_close_input(&vs->formatCtx);
        }

        if (ret != 0) {
            SDL_Event event;
            event.type = FF_QUIT_EVENT;
            event.user.data1 = vs;
            SDL_PushEvent(&event);
        }
        return ret;
}

int open_codec_context(VideoState *vs, int *stream_idx, AVCodecContext **dec_ctx, enum AVMediaType type)
{
    AVFormatContext *fmt_ctx = vs->formatCtx;
    int ret, stream_index;
    AVStream *st;
    AVCodec *dec = NULL;
    AVDictionary *opts = NULL;

    ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not find %s stream in input file '%s'\n",
                av_get_media_type_string(type), vs->filename);
        return ret;
    } else {
        stream_index = ret;
        st = fmt_ctx->streams[stream_index];

        /* find decoder for the stream */
        dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            fprintf(stderr, "Failed to find %s codec\n",
                    av_get_media_type_string(type));
            return AVERROR(EINVAL);
        }

        /* Allocate a codec context for the decoder */
        *dec_ctx = avcodec_alloc_context3(dec);
        if (!*dec_ctx) {
            fprintf(stderr, "Failed to allocate the %s codec context\n",
                    av_get_media_type_string(type));
            return AVERROR(ENOMEM);
        }

        /* Copy codec parameters from input stream to output codec context */
        if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
            fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                    av_get_media_type_string(type));
            return ret;
        }

        /* Init the decoders, with or without reference counting */
        //av_dict_set(&opts, "refcounted_frames", refcount ? "1" : "0", 0);
        if ((ret = avcodec_open2(*dec_ctx, dec, &opts)) < 0) {
            fprintf(stderr, "Failed to open %s codec\n",
                    av_get_media_type_string(type));
            return ret;
        }
        *stream_idx = stream_index;
    }

    return 0;
}

int queue_picture(VideoState *vs, AVFrame *pFrame, double pts) {
    VideoPicture *vp;
    int dst_pix_fmt;
    AVPicture pict;

    SDL_LockMutex(vs->pictq_mutex);
    while(vs->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !vs->quit) {
        SDL_CondWait(vs->pictq_cond, vs->pictq_mutex);
    }

    SDL_UnlockMutex(vs->pictq_mutex);

    if (vs->quit) {
        return -1;
    }

    vp = &vs->pict_q[vs->pictq_windex];
    if (NULL == vp->pictYUV ||
        vp->width != vs->video_stm->codecpar->width ||
        vp->height != vs->video_stm->codecpar->height) {
        if (NULL != vp->pictYUV) {
            av_frame_free(&(vp->pictYUV));
            vp->pictYUV = NULL;
        }
        vp->pictYUV = av_frame_alloc(); // or av_frame_alloc()
        if (NULL == vp->pictYUV) {
            return -1;
        }
        AVCodecParameters *videoCodecPar = vs->video_stm->codecpar;
        vp->pictYUV->width = videoCodecPar->width;
        vp->pictYUV->height = videoCodecPar->height;
        vp->pictYUV->format = AV_PIX_FMT_YUV420P;
        /*The following fields must be set on frame before calling this function:
                format (pixel format for video, sample format for audio)
                width and height for video
                nb_samples and channel_layout for audio
        */
        if (av_frame_get_buffer(vp->pictYUV, 0) < 0) { //Allocate new buffer for frame failed
            av_frame_free(&(vp->pictYUV));
            vp->pictYUV = NULL;
        } 
    }
    if (vp->pictYUV) {
        sws_scale(
                    vs->sws_ctx,
                    (uint8_t const * const *)pFrame->data,
                    pFrame->linesize,
                    0,
                    vs->video_stm->codecpar->height,
                    vp->pictYUV->data,
                    vp->pictYUV->linesize
                );
        vp->pts = pts;
        //av_frame_copy ? copy meta data?
    }
    vp->width = vs->video_stm->codecpar->width;
    vp->height = vs->video_stm->codecpar->height;
    if (++vs->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
        vs->pictq_windex = 0;
    }

    SDL_LockMutex(vs->pictq_mutex);
    vs->pictq_size++;
    SDL_UnlockMutex(vs->pictq_mutex);
    return 0;

}

int video_thread(void *userdata) {
    VideoState *vs = (VideoState *)userdata;
    AVCodecContext *videoCodecCtx = vs->videoCodecCtx;
    AVPacket pkt1, *packet = &pkt1;
    int got_frame = 0;
    
    double pts = 0;
    AVFrame *frame;
    frame = av_frame_alloc();
    for (;;) {
        if (packet_queue_get(&vs->videoq, packet, 1, &vs->quit) < 0) {
            //means we need to quit getting packets
            break;
        }

        if (packet->data == flush_pkt.data) {
            avcodec_flush_buffers(vs->videoCodecCtx);
            continue;
        }
        pts = 0;

        int ret = avcodec_send_packet(videoCodecCtx, packet);
        if (ret < 0) {
            fprintf(stderr, "Error sending a packet for decoding\n");
            continue;
        }

        while (ret >= 0) {
            ret = avcodec_receive_frame(videoCodecCtx, frame);
            if (ret == 0) {
                if ((pts = frame->best_effort_timestamp) == AV_NOPTS_VALUE) {
                    pts = 0;
                }
                pts *= av_q2d(vs->video_stm->time_base);
                pts = synchronize_video(vs, frame, pts);
                if (queue_picture(vs, frame, pts) < 0) {
                    goto fail;
                }
            } else if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                fprintf(stderr, "Error during decoding\n");
                break;
            }
        }
        // avcodec_decode_video2(videoCodecCtx, frame, &got_frame, packet);
        // if (packet->dts == AV_NOPTS_VALUE &&
        //         frame->opaque && *(uint64_t *)frame->opaque != AV_NOPTS_VALUE) {
        //     pts = *(uint64_t *)frame->opaque;
        // } else if (packet->dts != AV_NOPTS_VALUE) {
        //     pts = packet->dts;
        // } else {
        //     pts = 0;
        // }
        av_packet_unref(packet);

    }

    fail:
        av_frame_free(&frame);
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = vs;
        SDL_PushEvent(&event);
    return 0;
}

void stream_component_close(VideoState *vs, enum AVMediaType type) {
    switch(type) {
        case AVMEDIA_TYPE_AUDIO:
            vs->audioStreamIndex = -1;
            vs->audio_stm = NULL;
            avcodec_free_context(&(vs->audioCodecCtx));
            swr_close(vs->swr_ctx);
            vs->audio_buf_size = 0;
            vs->audio_buf_index = 0;
            if (NULL != out_buffer) {
                free(out_buffer);
            }
            
            SDL_CloseAudio();
            break;
        case AVMEDIA_TYPE_VIDEO:
            vs->videoStreamIndex = -1;
            vs->video_stm = NULL;
            avcodec_free_context(&(vs->videoCodecCtx));
            //sws_close(vs->sws_ctx);
            SDL_WaitThread(vs->video_tid, NULL);
            break;
        default:
            break;
    }
}

int stream_component_open(VideoState *vs, enum AVMediaType type) {
    AVFormatContext *formatCtx = vs->formatCtx;
    AVCodecContext *codecCtx = NULL;

    switch (type) {
        case  AVMEDIA_TYPE_AUDIO:
            vs->audio_stm = formatCtx->streams[vs->audioStreamIndex];
            codecCtx = vs->audioCodecCtx;
            vs->audio_buf_size = 0;
            vs->audio_buf_index = 0;
            if (audio_out_linesize == -1) {
                int out_buffer_size = av_samples_get_buffer_size(&audio_out_linesize, codecCtx->channels,codecCtx->frame_size,codecCtx->sample_fmt, 1);
                out_buffer = malloc(out_buffer_size);
                fprintf(stdout, "get samples buffer size:%d outlinesize:%d\n", out_buffer_size, audio_out_linesize);
            }
            vs->swr_ctx = swr_alloc();
            vs->swr_ctx = swr_alloc_set_opts(vs->swr_ctx, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, codecCtx->sample_rate, codecCtx->channel_layout, codecCtx->sample_fmt, codecCtx->sample_rate, 0, NULL);
            swr_init(vs->swr_ctx);
            SDL_AudioSpec wanted_spec, haved_spec;
            SDL_zero(wanted_spec);
            SDL_zero(haved_spec);
            audio_channels = codecCtx->channels;
            wanted_spec.freq = codecCtx->sample_rate;
            wanted_spec.format = AUDIO_S16SYS;
            wanted_spec.channels = codecCtx->channels;
            wanted_spec.silence = 0;
            wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
            wanted_spec.callback = audio_callback;
            wanted_spec.userdata = vs;

            if (SDL_OpenAudio(&wanted_spec, &haved_spec) < 0) {
                fprintf(stderr, "Failed to open audio: %s\n", SDL_GetError());
                return -1;
            } else {
                if (haved_spec.format != wanted_spec.format) {
                    fprintf(stderr, "Can't get Signed16 SYS audio format.\n");
                }
            }

            packet_queue_init(&vs->audioq);

            SDL_PauseAudio(0);

            break;
        case AVMEDIA_TYPE_VIDEO:
            codecCtx = vs->videoCodecCtx;

            vs->frame_timer = (double)av_gettime_relative() / 1000000.0;
            vs->frame_last_delay = 40e-3;

            vs->video_stm = formatCtx->streams[vs->videoStreamIndex];
            
            vs->sws_ctx = sws_getContext(
                codecCtx->width,
                codecCtx->height,
                codecCtx->pix_fmt,
                codecCtx->width,
                codecCtx->height,
                AV_PIX_FMT_YUV420P,
                SWS_BILINEAR,
                NULL,
                NULL,
                NULL
            );
            packet_queue_init(&vs->videoq);
            vs->video_tid = SDL_CreateThread(video_thread, "video_thread", vs);
            break;
        default:
            break;
    }
    return 0;
}


void audio_callback(void *userdata, Uint8 *stream, int len) {
    VideoState *vs = (VideoState *)userdata;
    int len1, audio_decoded_size;
    double pkt_pts;

    SDL_memset(stream, 0, len);

    while (len > 0) {
        if (vs->audio_buf_index >= vs->audio_buf_size) {
            audio_decoded_size = audio_decode_frame(vs, vs->audio_buf, sizeof(vs->audio_buf), &pkt_pts);
            if (audio_decoded_size < 0) {
                vs->audio_buf_size = 1024;
                memset(vs->audio_buf,0,vs->audio_buf_size);
            } else {
                vs->audio_buf_size = audio_decoded_size;
            }
            vs->audio_buf_index = 0;
        }

        len1 = vs->audio_buf_size - vs->audio_buf_index;
        if (len1 > len) {
            len1 = len;
        }
        SDL_MixAudio(stream, (uint8_t *)vs->audio_buf + vs->audio_buf_index, len1, SDL_MIX_MAXVOLUME / 2);
        //memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);
        len -= len1;
        stream += len1;
        vs->audio_buf_index += len1;
    }
}

int audio_decode_frame(VideoState *vs, uint8_t *audio_buf, int buf_size, double *pts_ptr) {
    static AVPacket packet, *audioPkt = &packet;
    double pts;
    int data_size = 0;

    AVCodecContext *audioCodecCtx = vs->audioCodecCtx;

    if (audioFrame == NULL) {
        audioFrame = av_frame_alloc();
    }

    av_init_packet(audioPkt);
    audioPkt->data = NULL;
    audioPkt->size = 0;
    for(;;) {
        if (audioPkt->data) {
            av_packet_unref(audioPkt);
        }

        if (vs->quit) {
            return -1;
        }

        if (packet_queue_get(&vs->audioq, audioPkt, 1, &vs->quit) < 0) {
            return -1;
        }
        if (audioPkt->data == flush_pkt.data) {
            avcodec_flush_buffers(vs->audioCodecCtx);
            continue;
        }
        
        if (audioPkt->pts != AV_NOPTS_VALUE) {
            vs->audio_clock = av_q2d(vs->audio_stm->time_base) * audioPkt->pts;
        }

        int ret = avcodec_send_packet(audioCodecCtx, audioPkt);
        if (ret < 0) {
            fprintf(stderr, "Error sending a audio packet for decoding\n");
            continue;
        }

        while (ret >= 0) {
            ret = avcodec_receive_frame(audioCodecCtx, audioFrame);
            if (ret == 0) {
                int out_nb_samples = audioFrame->nb_samples;
                int out_len;
                if (audioFrame->format != AV_SAMPLE_FMT_S16) {
                    out_nb_samples = swr_convert(vs->swr_ctx, &out_buffer, audio_out_linesize, (const uint8_t **)audioFrame->data, audioFrame->nb_samples);
                    out_len = av_samples_get_buffer_size(NULL, audioCodecCtx->channels, out_nb_samples, AV_SAMPLE_FMT_S16, 1);
                    memcpy(audio_buf, out_buffer, out_len);
                } else {
                    out_len = av_samples_get_buffer_size(NULL, audioCodecCtx->channels, out_nb_samples, AV_SAMPLE_FMT_S16, 1);
                    memcpy(audio_buf, audioFrame->data[0], out_len);
                }
                audio_buf += out_len;
                data_size += out_len;
                // int out_len = out_nb_samples * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16)*audio_channels;
                

                pts = vs->audio_clock;
                *pts_ptr = pts;
                n = 2 * vs->audio_stm->codecpar->channels;
                vs->audio_clock += (double)out_len / (double)(n * vs->audio_stm->codecpar->sample_rate);
            } else if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                fprintf(stderr, "Error during decoding\n");
                break;
            }
        }
        return data_size;

    }
}

void video_display (VideoState *vs) {
    SDL_Rect rect;
    VideoPicture *vp;
    AVPicture pict;
    float aspect_ratio;
    int w, h, x, y;
    int screenW, screenH;
    int i;

    vp = &vs->pict_q[vs->pictq_rindex];
    if (vp->pictYUV) {
        if (vs->video_stm->codecpar->sample_aspect_ratio.num == 0) {
            aspect_ratio = 0;
        } else {
            aspect_ratio = av_q2d(vs->video_stm->codecpar->sample_aspect_ratio) *
                vs->video_stm->codecpar->width / vs->video_stm->codecpar->height;
        }

        if (aspect_ratio <= 0.0) {
            aspect_ratio = (float)vs->video_stm->codecpar->width / (float)vs->video_stm->codecpar->height;
        }

        SDL_GetWindowSize(sdlWindow, &screenW, &screenH);
        h = screenH;
        w = ((int)rint(h * aspect_ratio)) & -3;
        if (w > screenW) {
            w = screenW;
            h = ((int)rint(w / aspect_ratio)) & -3;
        }

        x = (screenW - w) / 2;
        y = (screenH - h) / 2;

        rect.x = x;
        rect.y = y;
        rect.w = w;
        rect.h = h;
        SDL_UpdateYUVTexture(texture, &rect, 
                vp->pictYUV->data[0], vp->pictYUV->linesize[0],
                vp->pictYUV->data[1], vp->pictYUV->linesize[1],
                vp->pictYUV->data[2], vp->pictYUV->linesize[2]);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
    }
}

void video_refresh_timer(void *userdata) {
    VideoState *vs = (VideoState *)userdata;
    VideoPicture *vp = NULL;
    double actual_delay, delay, sync_threshold, ref_clock, diff;

    if (vs->video_stm) {
        if(vs->pictq_size == 0) {
            schedule_refresh(vs, 10);
        } else {
            vp = &vs->pict_q[vs->pictq_rindex];
            delay = vp->pts - vs->frame_last_pts;
            //use the previous pts and this pts to predict next frame's pts (usually the delay is 1/framerate--by joe)
            if (delay <= 0 || delay >= 1.0) {
                delay = vs->frame_last_delay;
            }
            //save for next time
            vs->frame_last_delay = delay;
            vs->frame_last_pts = vp->pts;


            //update delay to sync to audio
            ref_clock = get_audio_clock(vs);
            diff = vp->pts - ref_clock;

            // sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
            // min < delay < max, then delay
            // delay < min, then min
            // delay > max, then max
            sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
            if (fabs(diff) < AV_NOSYNC_THRESHOLD) {
                if (diff <= -sync_threshold) { // video play slower than audio, need to get video faster, so the video delay need to be smaller
                    delay = FFMAX(0, delay + diff);
                } else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD) {//video faster then audio, longer the delay to make video wait audio, as the delay is too big than normal video fram duration, we use delay+diff instead 2*delay, to in case delay too big
                    delay = delay + diff;
                } else if (diff >= sync_threshold) {//video faster than audio, and the delay is not too big, so directly double the delay
                    delay = 2 * delay;
                }
            }

            vs->frame_timer += delay;
            // fprintf(stdout, "refresh timer delay:%f, vp pts:%f, vs->frame_timer:%f, ref_clock:%f\n", delay, vp->pts, vs->frame_timer, ref_clock);

            //compute the real delay
            actual_delay = vs->frame_timer - (double)(av_gettime_relative() / 1000000.0);
            if (actual_delay < 0.010) { //if delay too small, give it a min value
                actual_delay = 0.010;
            }
            //fprintf(stdout, "refresh timer actual_delay:%d\n", (int)(actual_delay * 1000 + 0.5));
            // 根据延时时间刷新视频，从而实现视频同步音频
            schedule_refresh(vs, (int)(actual_delay * 1000 + 0.5));

            // schedule_refresh(vs, 40);
            //show the picture
            video_display(vs);


            // update the read index to next picture
            if (++vs->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
                vs->pictq_rindex = 0;
            }

            SDL_LockMutex(vs->pictq_mutex);
            vs->pictq_size--;
            SDL_CondSignal(vs->pictq_cond);
            SDL_UnlockMutex(vs->pictq_mutex);
        }
    } else {
        schedule_refresh(vs, 100);
    }
}

static Uint32 sdl_refresh_timer_cb (Uint32 interval, void *opaque) {
    SDL_Event event;
    SDL_zero(event);

    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0;
}

static void schedule_refresh(VideoState *vs, int delay) {
    SDL_AddTimer(delay, sdl_refresh_timer_cb, vs);
}

void clearAtExit(void) {
    if (audioFrame) {
        av_frame_free(&audioFrame);
    }
}

int decode_interrupt_cb(void *data) {
    return (global_video_state && global_video_state->quit != 0) ? 1 : 0;
}

int allocate_sdlwindow(void *userdata) {
    VideoState *vs = (VideoState *)userdata;

    int ret = -1;
    if (vs->video_stm) {
        sdlWindow = SDL_CreateWindow("sdl-ffmpeg player",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        vs->video_stm->codecpar->width, vs->video_stm->codecpar->height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    renderer = SDL_CreateRenderer(sdlWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (NULL == renderer) {
        fprintf(stderr, "SDL_CreateRenderer error:%s\n",SDL_GetError());
        return -1;
    }
    texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_YV12,
        SDL_TEXTUREACCESS_STREAMING,
        vs->video_stm->codecpar->width, vs->video_stm->codecpar->height);
    if (NULL == texture) {
        fprintf(stderr, "SDL_CreateTexture error:%s\n",SDL_GetError());
        return -1;
    }
        SDL_CondSignal(sdlWindow_alloc_cond);
        ret = 0;
    }

    return ret;
}

double synchronize_video(VideoState *vs, AVFrame *src_frame, double pts) {
    double frame_delay;

    if (pts != 0) {
        vs->video_clock = pts;
    } else {
        pts = vs->video_clock;
    }

    frame_delay = av_q2d(vs->video_stm->time_base);
    frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
    vs->video_clock += frame_delay;
    return pts;
    
}

double get_audio_clock(VideoState *vs) {
    double pts;
    int hw_buf_size, bytes_per_sec, n;

    pts = vs->audio_clock; //updated in the audio thread(audio_decode_frame)
    hw_buf_size = vs->audio_buf_size - vs->audio_buf_index;
    bytes_per_sec = 0;
    n = vs->audio_stm->codecpar->channels * 2; // ?? why multiply 2 here?
    if (vs->audio_stm) {
        bytes_per_sec = vs->audio_stm->codecpar->sample_rate * n;
    }

    if (bytes_per_sec) {
        pts -= (double)hw_buf_size / bytes_per_sec;
    }

    return pts;


}


void stream_seek(VideoState *is, int64_t pos, int rel)
{
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_flags = rel < 0 ? AVSEEK_FLAG_BACKWARD : 0;
        is->seek_req = 1;
    }
}
