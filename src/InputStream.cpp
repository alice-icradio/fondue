#include "Fonduempeg.h"

InputStream::InputStream(std::string source_url, AVInputFormat* format, const AVCodecContext& output_codec_ctx, AVDictionary* options, 
                            SourceTimingModes timing_mode, DefaultSourceModes source_mode):
    m_source_url {source_url},
    m_output_codec_ctx {output_codec_ctx},
    m_options {options},
    m_timing_mode {timing_mode},
    m_source_mode {source_mode}
{
    
    m_default_frame_size = DEFAULT_FRAME_SIZE;
    m_frame = alloc_frame(&m_output_codec_ctx);
    m_output_frame_size = m_frame->nb_samples;   
    m_swr_ctx_xfade = alloc_resampler(&m_output_codec_ctx);

    

    /*open input file and deduce the right format context from the file*/
    if (avformat_open_input(&m_format_ctx, m_source_url.c_str(), format, &m_options) < 0)
    {
        throw "Input: couldn't open source";
    }

    /*retrieve stream information from the format context*/
    if (avformat_find_stream_info(m_format_ctx, NULL) < 0)
    {
        throw "Input: could not find stream information";
    }


    /*initialise the codec etc*/
    if (open_codec_context(AVMEDIA_TYPE_AUDIO) < 0)
    {
        throw "Input: could not open codec context";
    }

    av_dump_format(m_format_ctx, 0, m_source_url.c_str(), 0);

    m_pkt = av_packet_alloc();
    
    if (!m_pkt) 
    {
        throw "Input: could not allocate packet";
    }

    m_temp_frame = alloc_frame(m_input_codec_ctx);

        /* create resampling contexts */
    m_swr_ctx = alloc_resampler(m_input_codec_ctx, &m_output_codec_ctx);

        /* Create the FIFO buffer based on the specified output sample format. */
    if (!(m_queue = av_audio_fifo_alloc(m_output_codec_ctx.sample_fmt,
                                    m_output_codec_ctx.ch_layout.nb_channels, 1))) 
    {
    
        throw "Input: failed to allocate audio samples queue";
    }
   
    std::chrono::duration<double> sample_duration (1.0 / m_output_codec_ctx.sample_rate);
    m_loop_duration = (m_output_frame_size - DEFAULT_LOOP_TIME_OFFSET_SAMPLES) * sample_duration;
}

InputStream::InputStream(FFMPEGString &prompt_string, const AVCodecContext& output_codec_ctx, 
                        SourceTimingModes timing_mode, DefaultSourceModes source_mode):
                        
                        InputStream(prompt_string.url(), prompt_string.input_format(), output_codec_ctx, prompt_string.options(), timing_mode, source_mode)
{
    /*uses constructor delegation*/
}

   
InputStream::InputStream(const AVCodecContext& output_codec_ctx, DefaultSourceModes source_mode):
    m_output_codec_ctx {output_codec_ctx},
    m_source_mode {source_mode}
{
    
    m_timing_mode = SourceTimingModes::realtime;
    m_frame = alloc_frame(&m_output_codec_ctx);
    m_output_frame_size = m_frame->nb_samples;
    m_swr_ctx_xfade = alloc_resampler(&m_output_codec_ctx);
    m_source_valid = false; 
    AVChannelLayout default_channel_layout = AV_CHANNEL_LAYOUT_STEREO;

    /*allocate the null input frame*/
    m_temp_frame = av_frame_alloc();
    m_temp_frame->format = AV_SAMPLE_FMT_S16;
    av_channel_layout_copy(&m_temp_frame->ch_layout, &default_channel_layout);
    m_temp_frame->sample_rate = m_output_codec_ctx.sample_rate;
    m_temp_frame->nb_samples = m_output_frame_size;

    if (m_output_frame_size)
    {
        if (av_frame_get_buffer(m_temp_frame,0) < 0)
        {
            throw "Default input: error allocating a temporary audio buffer";
        }
    }

    /*allocate the resampling context*/
    m_swr_ctx = swr_alloc();
    if (!m_swr_ctx) 
    {
        throw "Default input: could not allocate a resampler context"; 
    }

    /*set the resampling options*/
    av_opt_set_chlayout  (m_swr_ctx, "in_chlayout",       &default_channel_layout,      0);
    av_opt_set_int       (m_swr_ctx, "in_sample_rate",     m_output_codec_ctx.sample_rate,    0);
    av_opt_set_sample_fmt(m_swr_ctx, "in_sample_fmt",      AV_SAMPLE_FMT_S16,     0);
    set_resampler_options(m_swr_ctx, &m_output_codec_ctx);

    /* initialize the resampling context */
    if ((swr_init(m_swr_ctx)) < 0) 
    {
        throw "Default input: failed to initialise the resampler context";
    }


    std::chrono::duration<double> sample_duration (1.0 / m_output_codec_ctx.sample_rate);
    m_loop_duration = (m_output_frame_size-DEFAULT_LOOP_TIME_OFFSET_SAMPLES) * sample_duration;

}

InputStream::InputStream()
{
}

InputStream::~InputStream()
{
    avformat_close_input(&m_format_ctx);
    avcodec_free_context(&m_input_codec_ctx);
    av_packet_free(&m_pkt);
    av_frame_free(&m_frame);
    av_frame_free(&m_temp_frame);
    swr_free(&m_swr_ctx);
    swr_free(&m_swr_ctx_xfade);
    av_audio_fifo_free(m_queue);
}

InputStream::InputStream(const InputStream& input_stream):
    m_source_url {input_stream.m_source_url},
    m_options {input_stream.m_options}, 
    m_output_codec_ctx {input_stream.m_output_codec_ctx},
    m_got_frame {input_stream.m_got_frame},
    m_ret {input_stream.m_ret},
    m_dst_nb_samples {input_stream.m_dst_nb_samples},
    m_default_frame_size {input_stream.m_default_frame_size},
    m_output_frame_size {input_stream.m_output_frame_size},
    m_actual_nb_samples {input_stream.m_actual_nb_samples},
    m_number_buffered_samples {input_stream.m_number_buffered_samples},
    m_loop_duration {input_stream.m_loop_duration},
    m_timing_mode {input_stream.m_timing_mode},
    m_source_valid {input_stream.m_source_valid},
    m_source_mode {input_stream.m_source_mode}

{
    if (input_stream.m_format_ctx)
    {
        m_format_ctx = avformat_alloc_context();
        *m_format_ctx = *(input_stream.m_format_ctx);
    }
    
    if (input_stream.m_input_codec_ctx)
    {
        const AVCodec* dec = avcodec_find_decoder(m_format_ctx->streams[0]->codecpar->codec_id);
        m_input_codec_ctx = avcodec_alloc_context3(dec);
        *m_input_codec_ctx = *(input_stream.m_input_codec_ctx);
        int ret{avcodec_open2(m_input_codec_ctx, dec, NULL)};
    }

    if (input_stream.m_pkt)
    {
         m_pkt = av_packet_alloc();
        *m_pkt = *(input_stream.m_pkt);
    }
    
    m_frame = av_frame_alloc();
    deepcopy_frame(m_frame, input_stream.m_frame);
    
    m_temp_frame = av_frame_alloc();
    deepcopy_frame(m_temp_frame, input_stream.m_temp_frame);

    m_swr_ctx = swr_alloc();
    m_swr_ctx_xfade = swr_alloc();
    deepcopy_swr_context(&m_swr_ctx, input_stream.m_swr_ctx);
    deepcopy_swr_context(&m_swr_ctx_xfade, input_stream.m_swr_ctx_xfade);
    int ret {swr_init(m_swr_ctx)};
    ret = swr_init(m_swr_ctx_xfade);

    if (input_stream.m_queue)
    {
        m_queue = av_audio_fifo_alloc(m_output_codec_ctx.sample_fmt,
                                    m_output_codec_ctx.ch_layout.nb_channels, 1);
        deepcopy_audio_fifo(input_stream.m_queue);
    }
   

}

InputStream& InputStream::operator=(const InputStream& input_stream)
{   
    if (&input_stream == this)
        return *this;

    m_source_url = input_stream.m_source_url;
    m_options = input_stream.m_options; 
    m_output_codec_ctx = input_stream.m_output_codec_ctx;
    m_got_frame = input_stream.m_got_frame;
    m_ret = input_stream.m_ret;
    m_dst_nb_samples = input_stream.m_dst_nb_samples;
    m_default_frame_size = input_stream.m_default_frame_size;
    m_output_frame_size = input_stream.m_output_frame_size;
    m_actual_nb_samples = input_stream.m_actual_nb_samples;
    m_number_buffered_samples = input_stream.m_number_buffered_samples;
    m_loop_duration = input_stream.m_loop_duration;
    m_timing_mode = input_stream.m_timing_mode;
    m_source_valid = input_stream.m_source_valid;
    m_source_mode = input_stream.m_source_mode;

    avformat_close_input(&m_format_ctx);
    if (input_stream.m_format_ctx)
    {
        m_format_ctx = avformat_alloc_context();
        *m_format_ctx = *(input_stream.m_format_ctx);
    }
    
    avcodec_free_context(&m_input_codec_ctx);
    if (input_stream.m_input_codec_ctx)
    {
        const AVCodec* dec = avcodec_find_decoder(m_format_ctx->streams[0]->codecpar->codec_id);
        m_input_codec_ctx = avcodec_alloc_context3(dec);
        *m_input_codec_ctx = *(input_stream.m_input_codec_ctx);
        int ret{avcodec_open2(m_input_codec_ctx, dec, NULL)};
    }

    av_packet_free(&m_pkt);
    if (input_stream.m_pkt)
    {
        m_pkt = av_packet_alloc();
        *m_pkt = *(input_stream.m_pkt);
    }
    
    av_frame_free(&m_frame);
    m_frame = av_frame_alloc();
    deepcopy_frame(m_frame, input_stream.m_frame);

    av_frame_free(&m_temp_frame);
    m_temp_frame = av_frame_alloc();
    deepcopy_frame(m_temp_frame, input_stream.m_temp_frame);

    swr_free(&m_swr_ctx);
    swr_free(&m_swr_ctx_xfade);
    m_swr_ctx = swr_alloc();
    m_swr_ctx_xfade = swr_alloc();
    deepcopy_swr_context(&m_swr_ctx, input_stream.m_swr_ctx);
    deepcopy_swr_context(&m_swr_ctx_xfade, input_stream.m_swr_ctx_xfade);
    int ret {swr_init(m_swr_ctx)};
    ret = swr_init(m_swr_ctx_xfade);

    av_audio_fifo_free(m_queue);
    if (input_stream.m_queue)
    {
        m_queue = av_audio_fifo_alloc(m_output_codec_ctx.sample_fmt,
                                        m_output_codec_ctx.ch_layout.nb_channels, 1);
        deepcopy_audio_fifo(input_stream.m_queue);
    }

    
    return *this;

}

InputStream::InputStream(InputStream&& input_stream) noexcept:
    m_source_url {input_stream.m_source_url},
    m_options {input_stream.m_options}, 
    m_output_codec_ctx {input_stream.m_output_codec_ctx},
    m_got_frame {input_stream.m_got_frame},
    m_ret {input_stream.m_ret},
    m_dst_nb_samples {input_stream.m_dst_nb_samples},
    m_default_frame_size {input_stream.m_default_frame_size},
    m_output_frame_size {input_stream.m_output_frame_size},
    m_actual_nb_samples {input_stream.m_actual_nb_samples},
    m_number_buffered_samples {input_stream.m_number_buffered_samples},
    m_loop_duration {input_stream.m_loop_duration},
    m_timing_mode {input_stream.m_timing_mode},
    m_source_valid {input_stream.m_source_valid},
    m_source_mode {input_stream.m_source_mode},
    m_format_ctx {input_stream.m_format_ctx},
    m_input_codec_ctx {input_stream.m_input_codec_ctx},
    m_frame {input_stream.m_frame},
    m_temp_frame {input_stream.m_temp_frame},
    m_pkt {input_stream.m_pkt},
    m_swr_ctx {input_stream.m_swr_ctx},
    m_swr_ctx_xfade {input_stream.m_swr_ctx_xfade},
    m_queue {input_stream.m_queue}

{
    input_stream.m_format_ctx = nullptr;
    input_stream.m_input_codec_ctx = nullptr;
    input_stream.m_frame = nullptr;
    input_stream.m_temp_frame = nullptr;
    input_stream.m_pkt = nullptr;
    input_stream.m_swr_ctx = nullptr;
    input_stream.m_swr_ctx_xfade = nullptr;
    input_stream.m_queue = nullptr;    
}

InputStream& InputStream::operator=(InputStream&& input_stream) noexcept
{
    if (&input_stream == this)
        return *this;

    m_source_url = input_stream.m_source_url;
    m_options = input_stream.m_options; 
    m_output_codec_ctx = input_stream.m_output_codec_ctx;
    m_got_frame = input_stream.m_got_frame;
    m_ret = input_stream.m_ret;
    m_dst_nb_samples = input_stream.m_dst_nb_samples;
    m_default_frame_size = input_stream.m_default_frame_size;
    m_output_frame_size = input_stream.m_output_frame_size;
    m_actual_nb_samples = input_stream.m_actual_nb_samples;
    m_number_buffered_samples = input_stream.m_number_buffered_samples;
    m_loop_duration = input_stream.m_loop_duration;
    m_timing_mode = input_stream.m_timing_mode;
    m_source_valid = input_stream.m_source_valid;
    m_source_mode = input_stream.m_source_mode;

    avformat_close_input(&m_format_ctx);
    m_format_ctx = input_stream.m_format_ctx;
    input_stream.m_format_ctx = nullptr;

    avcodec_free_context(&m_input_codec_ctx);
    m_input_codec_ctx = input_stream.m_input_codec_ctx;
    input_stream.m_input_codec_ctx = nullptr;

    av_frame_free(&m_frame);
    m_frame = input_stream.m_frame;
    input_stream.m_frame = nullptr;
    
    av_frame_free(&m_temp_frame);
    m_temp_frame = input_stream.m_temp_frame;
    input_stream.m_temp_frame = nullptr;

    av_packet_free(&m_pkt);
    m_pkt = input_stream.m_pkt;
    input_stream.m_pkt = nullptr;

    swr_free(&m_swr_ctx);
    m_swr_ctx = input_stream.m_swr_ctx;
    input_stream.m_swr_ctx = nullptr;

    swr_free(&m_swr_ctx_xfade);
    m_swr_ctx_xfade = input_stream.m_swr_ctx_xfade;
    input_stream.m_swr_ctx_xfade = nullptr;

    av_audio_fifo_free(m_queue);
    m_queue = input_stream.m_queue;
    input_stream.m_queue = nullptr;    

    return *this;

}

int InputStream::resample_one_input_frame()
{
    m_dst_nb_samples = swr_get_out_samples(m_swr_ctx, m_temp_frame->nb_samples);
    m_frame -> nb_samples = m_dst_nb_samples;

    av_assert0(m_dst_nb_samples == m_frame->nb_samples);
    m_ret=av_frame_make_writable(m_frame);
    m_ret=swr_convert(m_swr_ctx, m_frame->data, m_dst_nb_samples, 
                        (const uint8_t **)m_temp_frame->data, m_temp_frame->nb_samples);
    
    if (m_ret < 0)
    {   
        return m_ret;
    }
    m_actual_nb_samples = m_ret;
    m_frame->nb_samples=m_ret;
    av_frame_unref(m_temp_frame);
    return m_ret;
}

int InputStream::resample_one_input_frame(SwrContext* swr_ctx)
{
    /*note CANNOT deal with sample rate changes, should only be used with crossfade*/
    m_ret=swr_convert(swr_ctx, m_frame->data, m_frame->nb_samples, 
                        (const uint8_t **)m_frame->data, m_frame->nb_samples);
    
    return m_ret;
}


bool InputStream::get_one_output_frame()
{
    /*check integrity of InputStream object, if invalid, just do nothing*/
    if (!m_temp_frame || !m_frame || !m_swr_ctx)
        //throw "InputStream object default initialised and therefore unable to handle audio";
        return false;

    /*if source is invalid, synthesise audio*/
    if (!m_source_valid)
    {
        int i, j, v, fullscale;
        m_ret = av_frame_make_writable(m_frame);
        m_ret = av_frame_make_writable(m_temp_frame);
        int16_t *q = (int16_t*)m_temp_frame->data[0];

        for (j = 0; j < m_temp_frame->nb_samples; j ++)
        {
            switch (+m_source_mode)
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
        /*since not changing the sample rate the number of samples shouldn't change*/
        m_ret = swr_convert(m_swr_ctx, m_frame->data, m_temp_frame->nb_samples,
                        (const uint8_t **)m_temp_frame->data, m_temp_frame->nb_samples);

        if (m_ret < 0)
        {
            printf("error resampling frame: %s\n", av_error_to_string(m_ret));
            throw "Default input: error resampling frame";
        }
        return true;
    }

    /*if enough samples are ready, copy one frame's worth to m_frame*/
    if (av_audio_fifo_size(m_queue) >= m_output_frame_size)
    {
        m_frame->nb_samples = m_output_frame_size;
        m_ret=av_frame_make_writable(m_frame);

        /*insert the correct number of samples from the queue into the output frame*/
        if (av_audio_fifo_read(m_queue, (void **)m_frame->data, m_frame->nb_samples) < m_frame->nb_samples) 
        {
            throw "Could not read data from FIFO";
        }
        return true;
    }

    else
    {
        while (av_audio_fifo_size(m_queue) <= m_output_frame_size)
        {
            /*request a new packet from the input*/
            m_ret = av_read_frame(m_format_ctx, m_pkt);
            if (m_ret < 0)
            {
                if (m_ret == AVERROR_EOF)
                    throw "no more packets, reached end of input";
                throw "error reading packet";
            }

            /*skip the packet if it's not an audio packet*/
            if (m_pkt->stream_index != m_stream_index)
            {
                av_packet_unref(m_pkt);
                continue;
            }

            /*send the packet to the decoder*/
            m_ret = avcodec_send_packet(m_input_codec_ctx, m_pkt);
            if (m_ret < 0)
            {
                throw "error submitting a packet for decoding";
            }

            av_packet_unref(m_pkt);

            /*get all the raw frames out of the packet. there may be
            * more than one for some codecs*/
           while (m_ret >= 0)
           {
                m_ret = avcodec_receive_frame(m_input_codec_ctx, m_temp_frame);
                if (m_ret < 0)
                {
                    //special cases if no frame was available but otherwise no errors
                    if (m_ret == AVERROR_EOF || m_ret == AVERROR(EAGAIN))
                        continue;
                    throw "error during decoding";
                }
                /*resample the frame into the output sample format and sample rate*/
                if (resample_one_input_frame() < 0)
                    throw "could not resample input frame";
                
                /*add all samples from the frame to the FIFO*/
                if (av_audio_fifo_write(m_queue, (void**)m_frame->data, m_frame->nb_samples) < m_frame->nb_samples)
                    throw "could not write the decoded frame to the fifo";
           }
        }

        m_frame->nb_samples = m_output_frame_size;
        m_ret=av_frame_make_writable(m_frame);

        /*insert the correct number of samples from the queue into the output frame*/
        if (av_audio_fifo_read(m_queue, (void **)m_frame->data, m_frame->nb_samples) < m_frame->nb_samples) 
        {
            throw "Could not read data from FIFO";
        }
        return true;

    }
  
}

bool InputStream::crossfade_frame(AVFrame* new_input_frame, int& fade_time_remaining, int fade_time)
{
    /*uses pointer arithmetic to linearly fade between two frames, requires AV_SAMPLE_FMT_FLTP*/
    int i , j; 
    float *q , *v;
    get_one_output_frame();

    /*a value between zero and one representing how far through the fade we are currently*/
    float non_dimensional_fade_time = 1 - static_cast<float>(fade_time_remaining)/fade_time;

    float non_dimensional_fade_time_increment = 1 / (static_cast<float>(new_input_frame->sample_rate/1000)*fade_time);

    for (i = 0; i < m_frame->ch_layout.nb_channels; i++)
    {
        q = (float*)m_frame->data[i];
        v = (float*)new_input_frame->data[i];

        for (j = 0; j < m_frame->nb_samples; j++)
        {
            *q = *q *(1-non_dimensional_fade_time) + *v * non_dimensional_fade_time; 
            q++;
            v++;
        }
    }

    resample_one_input_frame(m_swr_ctx_xfade); 
    fade_time_remaining -= get_frame_length_milliseconds();
    return true;
    
}

int InputStream::open_codec_context(enum AVMediaType type)
{
    int ret;
    AVStream *st{};
    const AVCodec* dec{};
 
    
        /*determine the stream index of the audio stream*/
        m_stream_index = av_find_best_stream(m_format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
        st = m_format_ctx->streams[m_stream_index];
 
        /* find decoder for the stream */
        dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            fprintf(stderr, "Failed to find %s codec\n",
                    av_get_media_type_string(type));
            return AVERROR(EINVAL);
        }
 
        /* Allocate a codec context for the decoder */
        m_input_codec_ctx = avcodec_alloc_context3(dec);
        if (!m_input_codec_ctx) {
            fprintf(stderr, "Failed to allocate the %s codec context\n",
                    av_get_media_type_string(type));
            return AVERROR(ENOMEM);
        }
 
        /* Copy codec parameters from input stream to codec context */
        if ((ret = avcodec_parameters_to_context(m_input_codec_ctx, st->codecpar)) < 0) {
            fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                    av_get_media_type_string(type));
            return ret;
        }
 
        /* Init the decoders */
        if ((ret = avcodec_open2(m_input_codec_ctx, dec, NULL)) < 0) {
            fprintf(stderr, "Failed to open %s codec\n",
                    av_get_media_type_string(type));
            return ret;
        }
    return 0;
}

AVFrame* InputStream::alloc_frame(AVCodecContext* codec_context)
{
    AVFrame* frame = av_frame_alloc();
    int nb_samples;
    if (!frame) 
    {
        throw "Input: error allocating an audio frame";
    }

    if (codec_context->codec)
    {
        if (codec_context->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
            codec_context->frame_size = DEFAULT_FRAME_SIZE;
    }
    
    nb_samples = codec_context->frame_size;
 
    frame->format = codec_context -> sample_fmt;
    av_channel_layout_copy(&frame->ch_layout, &codec_context->ch_layout);
    frame->sample_rate = codec_context -> sample_rate;
    frame->nb_samples = nb_samples;
 
    if (nb_samples) {
        if (av_frame_get_buffer(frame, 0) < 0) 
        {
            throw "Input: error allocating an audio buffer";
        }
    }
 
    return frame;
}

int InputStream::flush_resampler()
{
    m_dst_nb_samples = swr_get_out_samples(m_swr_ctx, m_temp_frame->nb_samples);
    m_frame -> nb_samples = m_dst_nb_samples;

    av_assert0(m_dst_nb_samples == m_frame->nb_samples);
    m_ret=av_frame_make_writable(m_frame);
    m_ret=swr_convert(m_swr_ctx, m_frame->data, m_dst_nb_samples, 
                        0, 0);
    
    if (m_ret < 0)
    {   
        return m_ret;
    }
    m_actual_nb_samples = m_ret;
    m_frame->nb_samples=m_ret;

    if (av_audio_fifo_write(m_queue, (void**)m_frame->data, m_frame->nb_samples) < m_frame->nb_samples)
        throw "could not write the flushed samples to the fifo";

    return m_ret;
}

bool InputStream::empty_queue()
{
    /*if enough samples are available, copy one frame's worth to m_frame*/
    if (av_audio_fifo_size(m_queue) >= m_output_frame_size)
    {
        m_frame->nb_samples = m_output_frame_size;
        m_ret=av_frame_make_writable(m_frame);

        /*insert the correct number of samples from the queue into the output frame*/
        if (av_audio_fifo_read(m_queue, (void **)m_frame->data, m_frame->nb_samples) < m_frame->nb_samples) 
        {
            throw "Could not read data from FIFO";
        }
        return true;
    }
    else
    {
        return false;
    }
}

void InputStream::clear_queue()
{
    av_audio_fifo_reset(m_queue);
}

void InputStream::resample_queue(const AVSampleFormat& old_sample_fmt, const AVSampleFormat& new_sample_fmt)
{
    if (av_audio_fifo_size(m_queue) == 0)
    {
        clear_queue();
        return;
    }

    AVFrame* temp_frame = av_frame_alloc();
    temp_frame->format = m_output_codec_ctx.sample_fmt;
    av_channel_layout_copy(&temp_frame->ch_layout, &m_output_codec_ctx.ch_layout);
    temp_frame->sample_rate = m_output_codec_ctx.sample_rate;
    temp_frame->nb_samples = av_audio_fifo_size(m_queue);
 
    if (temp_frame->nb_samples) {
        if (av_frame_get_buffer(temp_frame, 0) < 0) 
        {
            throw "Input: error allocating an audio buffer";
        }
    }

    if (av_audio_fifo_read(m_queue, (void **)temp_frame->data, temp_frame->nb_samples) < temp_frame->nb_samples) 
    {
        throw "Could not read data from FIFO";
    }

    struct SwrContext* temp_swr_ctx = swr_alloc();

    av_opt_set_chlayout(temp_swr_ctx, "in_chlayout", &temp_frame->ch_layout, 0);
    av_opt_set_chlayout(temp_swr_ctx, "out_chlayout", &temp_frame->ch_layout, 0);
    av_opt_set_int(temp_swr_ctx, "in_sample_rate", temp_frame->sample_rate, 0);
    av_opt_set_int(temp_swr_ctx, "out_sample_rate", temp_frame->sample_rate, 0);
    av_opt_set_sample_fmt(temp_swr_ctx, "in_sample_fmt", old_sample_fmt, 0);
    av_opt_set_sample_fmt(temp_swr_ctx, "out_sample_fmt", new_sample_fmt, 0);

    swr_init(temp_swr_ctx);

    swr_convert(temp_swr_ctx, temp_frame->data, temp_frame->nb_samples, 
                        (const uint8_t **)temp_frame->data, temp_frame->nb_samples);

    int channels {};
    if (new_sample_fmt == AV_SAMPLE_FMT_FLTP)
        channels = 2;
    else
        channels = m_output_codec_ctx.ch_layout.nb_channels;

    av_audio_fifo_free(m_queue);
    m_queue = av_audio_fifo_alloc(new_sample_fmt, channels, temp_frame->nb_samples);

    if (av_audio_fifo_write(m_queue, (void**)temp_frame->data, temp_frame->nb_samples) < temp_frame->nb_samples)
        throw "could not write the resampled samples to the fifo";

    av_frame_free(&temp_frame);
    swr_free(&temp_swr_ctx);
}

void InputStream::init_crossfade()
{
    if (!m_swr_ctx)
        throw "Resampling context not allocated";
        
    set_resampler_options(m_swr_ctx);
    /* initialize the resampling context */
    if ((swr_init(m_swr_ctx)) < 0) 
    {
        throw "crossfading: failed to initialise the resampler context";
    }
    if (av_audio_fifo_size(m_queue) == 0)
    {
        int channels = 2;
        av_audio_fifo_free(m_queue);
        m_queue = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, channels, 1);
    }
}
void InputStream::end_crossfade()
{
    set_resampler_options(m_swr_ctx, &m_output_codec_ctx);
    if ((swr_init(m_swr_ctx)) < 0) 
    {
        throw "end crossfading: failed to initialise the resampler context";
    }
    if (av_audio_fifo_size(m_queue) == 0)
    {
        int channels = m_output_codec_ctx.ch_layout.nb_channels;
        AVSampleFormat fmt = m_output_codec_ctx.sample_fmt;
        av_audio_fifo_free(m_queue);
        m_queue = av_audio_fifo_alloc(fmt, channels, 1);
    }
}

void InputStream::set_resampler_options(SwrContext* swr_ctx, AVCodecContext* input_codec_ctx, AVCodecContext* output_codec_ctx)
{
    av_opt_set_chlayout  (swr_ctx, "in_chlayout",       &input_codec_ctx->ch_layout,      0);
    av_opt_set_int       (swr_ctx, "in_sample_rate",     input_codec_ctx->sample_rate,    0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt",      input_codec_ctx->sample_fmt,     0);
    av_opt_set_chlayout  (swr_ctx, "out_chlayout",      &output_codec_ctx->ch_layout,      0);
    av_opt_set_int       (swr_ctx, "out_sample_rate",    output_codec_ctx->sample_rate,    0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt",     output_codec_ctx->sample_fmt,     0);
}

void InputStream::set_resampler_options(SwrContext* swr_ctx)
{
    AVChannelLayout default_channel_layout = AV_CHANNEL_LAYOUT_STEREO;
    av_opt_set_chlayout  (swr_ctx, "out_chlayout",      &default_channel_layout,      0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt",     AV_SAMPLE_FMT_FLTP,     0);
}

void InputStream::set_resampler_options(SwrContext* swr_ctx, AVCodecContext* output_codec_ctx)
{
    av_opt_set_chlayout  (swr_ctx, "out_chlayout",      &output_codec_ctx->ch_layout,      0);
    av_opt_set_int       (swr_ctx, "out_sample_rate",    output_codec_ctx->sample_rate,    0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt",     output_codec_ctx->sample_fmt,     0);
}


struct SwrContext* InputStream::alloc_resampler (AVCodecContext* input_codec_ctx, AVCodecContext* output_codec_ctx)
{
    /*create the resampling context*/
    struct SwrContext* swr_ctx = swr_alloc();
    if (!swr_ctx) {
        throw "Input: could not allocate a resampler context";
        
    }

    
    set_resampler_options(swr_ctx, input_codec_ctx, output_codec_ctx);

    /* initialize the resampling context */
    if ((swr_init(swr_ctx)) < 0) 
    {
        throw "Input: failed to initialise the resampler context";
    }

    return swr_ctx;

}

struct SwrContext* InputStream::alloc_resampler (AVCodecContext* output_codec_ctx)
{
    /*create the resampling context*/
    struct SwrContext* swr_ctx = swr_alloc();
    if (!swr_ctx) 
    {
        throw "Input: could not allocate a resampler context";       
    }

    AVChannelLayout default_channel_layout = AV_CHANNEL_LAYOUT_STEREO;
    av_opt_set_chlayout  (swr_ctx, "in_chlayout",       &default_channel_layout,      0);
    av_opt_set_int       (swr_ctx, "in_sample_rate",     output_codec_ctx->sample_rate,    0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt",      AV_SAMPLE_FMT_FLTP,     0);
    
    set_resampler_options(swr_ctx, output_codec_ctx);
    

    /* initialize the resampling context */
    if ((swr_init(swr_ctx)) < 0) 
    {
        throw "Input: failed to initialise the resampler context";
    }

    return swr_ctx;

}

int InputStream::get_frame_length_milliseconds()
{
    int rate = m_frame->sample_rate;
    int number = m_frame->nb_samples;
    return number/(rate/1000);
}

void InputStream::sleep(std::chrono::_V2::steady_clock::time_point &end_time) const
{
    fondue_sleep(end_time, m_loop_duration, m_timing_mode);
}

void InputStream::deepcopy_swr_context(struct SwrContext** dst, struct SwrContext* src)
{
    AVChannelLayout in_chlayout{};
    int64_t in_sample_rate{};
    AVSampleFormat in_sample_fmt{};
    AVChannelLayout out_chlayout{};
    int64_t out_sample_rate{};
    AVSampleFormat out_sample_fmt{};

    
    av_opt_get_chlayout(src, "in_chlayout", 0, &in_chlayout);
    av_opt_get_int(src, "in_sample_rate", 0, &in_sample_rate);
    av_opt_get_sample_fmt(src, "in_sample_fmt", 0, &in_sample_fmt);
    av_opt_get_chlayout(src, "out_chlayout", 0, &out_chlayout);
    av_opt_get_int(src, "out_sample_rate", 0, &out_sample_rate);
    av_opt_get_sample_fmt(src, "out_sample_fmt", 0, &out_sample_fmt);

    av_opt_set_chlayout  (*dst, "in_chlayout",       &in_chlayout,      0);
    av_opt_set_int       (*dst, "in_sample_rate",     in_sample_rate,    0);
    av_opt_set_sample_fmt(*dst, "in_sample_fmt",      in_sample_fmt,     0);
    av_opt_set_chlayout  (*dst, "out_chlayout",      &out_chlayout,      0);
    av_opt_set_int       (*dst, "out_sample_rate",    out_sample_rate,    0);
    av_opt_set_sample_fmt(*dst, "out_sample_fmt",     out_sample_fmt,     0);

}

void InputStream::deepcopy_audio_fifo(AVAudioFifo* src)
{
    int queue_length {av_audio_fifo_size(src)};
    AVFrame* temp_frame = av_frame_alloc();
    temp_frame->nb_samples = queue_length;
    temp_frame->sample_rate = m_output_codec_ctx.sample_rate;
    temp_frame->format = m_output_codec_ctx.sample_fmt;
    av_frame_get_buffer(temp_frame, 0);
    av_audio_fifo_peek(src, (void**)temp_frame->data, queue_length);
    int x {av_audio_fifo_realloc(m_queue, queue_length)};
    x = av_audio_fifo_write(m_queue, (void**)temp_frame->data, queue_length);
    av_frame_free(&temp_frame);
}

void InputStream::deepcopy_frame(AVFrame* &dst, AVFrame* src)
{
    av_frame_copy_props(dst, src);
    dst->nb_samples = src->nb_samples;
    dst->format = src->format;
    av_channel_layout_copy(&dst->ch_layout, &src->ch_layout);
    av_frame_get_buffer(dst, 0);
    av_frame_copy(dst, src);
}









    
