#include"Fonduempeg.h"

/*normal constructor, does most of the FFMpeg boilerplate stuff*/
DefaultInputStream::DefaultInputStream(AVCodecContext* output_codec_ctx)
{
    m_output_codec_ctx = output_codec_ctx;

    /*allocate an output frame for later use*/

    m_frame = av_frame_alloc();

    if (!m_frame)
    {
        cleanup();
        throw "Default input: error allocating an audio frame";
    }

    int nb_samples{};

    if (m_output_codec_ctx->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
    {
        nb_samples = DEFAULT_FRAME_SIZE;
    }

    else
        nb_samples = m_output_codec_ctx->frame_size;

    m_frame->format = m_output_codec_ctx->sample_fmt;
    av_channel_layout_copy(&m_frame->ch_layout, &m_output_codec_ctx->ch_layout);
    m_frame->sample_rate = m_output_codec_ctx->sample_rate;
    m_frame->nb_samples = nb_samples; 

    if (nb_samples)
    {
        if (av_frame_get_buffer(m_frame,0) < 0)
        {
            cleanup();
            throw "Default input: error allocating an audio buffer";
        }
    }


    /*allocate a temporary frame for filling with silence or whatever before resampling to the output format*/

    m_temp_frame = av_frame_alloc();

    if (!m_temp_frame)
    {
        cleanup();
        throw "Default input: error allocating a temporary audio frame";
    }

    m_temp_frame->format = AV_SAMPLE_FMT_S16;
    AVChannelLayout default_channel_layout = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&m_temp_frame->ch_layout, &default_channel_layout);
    m_temp_frame->sample_rate = m_output_codec_ctx->sample_rate;
    m_temp_frame->nb_samples = nb_samples;
    if (nb_samples)
    {
        if (av_frame_get_buffer(m_temp_frame,0) < 0)
        {
            cleanup();
            throw "Default input: error allocating a temporary audio buffer";
        }
    }


    /*allocate a resampling context*/

    m_swr_ctx = swr_alloc();
    if (!m_swr_ctx) 
    {
        cleanup();
        throw "Default input: error allocating a resampler context";
    }

    av_opt_set_chlayout  (m_swr_ctx, "in_chlayout",       &default_channel_layout,      0);
    av_opt_set_int       (m_swr_ctx, "in_sample_rate",     m_output_codec_ctx->sample_rate,    0);
    av_opt_set_sample_fmt(m_swr_ctx, "in_sample_fmt",      AV_SAMPLE_FMT_S16,     0);
    av_opt_set_chlayout  (m_swr_ctx, "out_chlayout",      &m_output_codec_ctx->ch_layout,      0);
    av_opt_set_int       (m_swr_ctx, "out_sample_rate",    m_output_codec_ctx->sample_rate,    0);
    av_opt_set_sample_fmt(m_swr_ctx, "out_sample_fmt",     m_output_codec_ctx->sample_fmt,     0);
 
    /* initialize the resampling context */
    if ((swr_init(m_swr_ctx)) < 0) 
    {
        cleanup();
        throw "Default input: error initialising the resampling context";

    }

    m_output_data_size = av_get_bytes_per_sample(m_output_codec_ctx->sample_fmt);
    if (m_output_data_size < 0) 
    {
        /* This should not occur, checking just for paranoia */
        cleanup();
        throw "Default input: failed to calculate data size";
    }

    /*work out how much audio is in the frame in units of clock ticks and subtracts a little bit
    * to ensure the loop always runs slightly too fast (too slow leads to dropouts)*/
    m_ticks_per_frame = (m_temp_frame->nb_samples * CLOCKS_PER_SEC)/m_temp_frame->sample_rate - DEFAULT_TIMING_OFFSET;






}
/*facilitates exiting elegantly*/
void DefaultInputStream::cleanup()
{
    av_frame_free(&m_frame);
    av_frame_free(&m_temp_frame);
    swr_free(&m_swr_ctx);
}
/*destructor*/
DefaultInputStream::~DefaultInputStream()
{
    cleanup();
}


bool DefaultInputStream::get_one_output_frame(DefaultSourceModes mode, SourceTimingModes timing)
{
    int i, j, v, fullscale;
    int16_t *q = (int16_t*)m_temp_frame->data[0];

    for (j = 0; j < m_temp_frame->nb_samples; j ++)
    {
        switch (+mode)
        {
            case +DefaultSourceModes::silence:
                v = 0;
                break;
            case +DefaultSourceModes::white_noise:
            /*fullscale = 100 gives quiet white noise*/
                fullscale=100;
                v = (static_cast<float>(std::rand())/RAND_MAX -0.5)*fullscale;
                break;
            default:
                v = 0;
                break;
        }
        
        for (i = 0; i < m_temp_frame->ch_layout.nb_channels; i ++)
        {
            *q++ = v;
        }
    }

    /*resample to achieve the output sample format and channel configuration*/

    m_ret = av_frame_make_writable(m_frame);
    /*since not changing the sample rate the number of samples shouldn't change*/
    m_ret = swr_convert(m_swr_ctx, m_frame->data, m_temp_frame->nb_samples,
                        (const uint8_t **)m_temp_frame->data, m_temp_frame->nb_samples);

    if (m_ret < 0)
    {
        printf("error resampling frame: %s\n", av_error_to_string(m_ret));
        cleanup();
        throw "Default input: error resampling frame";
    }

    switch(+timing)
    {   
        case +SourceTimingModes::realtime:
            /*work out how much processor time has passed since the last frame was decoded and set a sleep time accordingly*/   
            m_sleep_time = get_timespec_from_ticks(m_ticks_per_frame-(std::clock()-m_end_time));
            /*store the time just after the frame was decoded (for the benefit of the next iteration)*/
            m_end_time = std::clock();
            /*put the thread to sleep for the calculated amount of time*/
            nanosleep(&m_sleep_time, NULL);
            break;
        case +SourceTimingModes::freetime:
            m_end_time = std::clock();
            break;

        default:
            m_end_time = std::clock();
            break;
    }

    



    return true;
}

bool DefaultInputStream::get_one_output_frame(SourceTimingModes timing)
{
    DefaultSourceModes default_source_mode = DefaultSourceModes::white_noise;

    return DefaultInputStream::get_one_output_frame(default_source_mode, timing);
}
