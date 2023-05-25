#include "ffmpeg_rtmp.h"
#include <QStandardPaths>

void ffmpegLogCallback(void *avcl, int level, const char *fmt, va_list vl)
{
    if (level <= av_log_get_level()) // Only log messages with a level equal to or higher than the current FFmpeg log level
    {
        char message[1024];
        vsnprintf(message, sizeof(message), fmt, vl);
        qDebug() << message;
    }
}

ffmpeg_rtmp::ffmpeg_rtmp(QObject *parent)
    : QThread{parent}
{
    // Enable FFmpeg logging
    av_log_set_level(AV_LOG_ERROR);
    //    av_log_set_callback(ffmpegLogCallback);
    //tTM/2!**

    const char* ffmpegVersion = av_version_info();
    std::cout << "FFmpeg version: " << ffmpegVersion << std::endl;

    m_devices = new QMediaDevices(this);

    out_filename = QString("%1/output.mp4").arg(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
    avformat_network_init();
}

void ffmpeg_rtmp::stop()
{
    m_stop = true;
}

void ffmpeg_rtmp::setUrl()
{
    bool found = false;
    foreach(QNetworkInterface interface, QNetworkInterface::allInterfaces())
    {
        if (interface.flags().testFlag(QNetworkInterface::IsUp) && !interface.flags().testFlag(QNetworkInterface::IsLoopBack))
            foreach (QNetworkAddressEntry entry, interface.addressEntries())
            {
                //qDebug() << entry.ip().toString() + " " + interface.hardwareAddress()  + " " + interface.humanReadableName();
                if ( !found && interface.hardwareAddress() != "00:00:00:00:00:00" && entry.ip().toString().contains(".")
                     && !interface.humanReadableName().contains("VM") && !interface.hardwareAddress().startsWith("00:") && interface.hardwareAddress() != "")
                {
                    in_filename  = "rtmp://" + entry.ip().toString() + ":8889/live";
                    qDebug() << in_filename;
                    emit sendUrl(in_filename);
                    found = true;
                }
            }
    }
}


int ffmpeg_rtmp::prepare_ffmpeg()
{
    // Open the RTMP stream
    AVDictionary *format_opts = NULL;
    av_dict_set(&format_opts, "timeout", "10", 0);

    if (avformat_open_input(&inputContext, in_filename.toStdString().c_str() , nullptr, &format_opts) != 0) {
        // Error handling
        qDebug() << "error avformat_open_input";
        return false;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(inputContext, nullptr) < 0) {
        // Error handling
        qDebug() << "error avformat_find_stream_info";
        return false;
    }

    // Create the output file context
    if (avformat_alloc_output_context2(&outputContext, nullptr, nullptr, out_filename.toStdString().c_str()) < 0) {
        // Error handling
        qDebug() << "error avformat_alloc_output_context2";
        return false;
    }

    // Iterate through input streams and copy all streams to output
    for (unsigned int i = 0; i < inputContext->nb_streams; ++i) {
        inputStream = inputContext->streams[i];
        outputStream = avformat_new_stream(outputContext, nullptr);
        if (inputContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            vid_stream = inputContext->streams[i];
            video_idx = i;
            qDebug() << "video_idx : " << video_idx;
        }
        else if (inputContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            aud_stream = inputContext->streams[i];;
            audio_idx = i;
            qDebug() << "audio_idx : " << audio_idx;
        }
        if (!outputStream) {
            // Error handling
            return false;
        }
        avcodec_parameters_copy(outputStream->codecpar, inputStream->codecpar);
    }

    // Open the output file for writing
    if (avio_open(&outputContext->pb, out_filename.toStdString().c_str(), AVIO_FLAG_WRITE) < 0) {
        // Error handling
        qDebug() << "error avio_open";
        return false ;
    }

    // Write the output file header
    if (avformat_write_header(outputContext, nullptr) < 0) {
        // Error handling
        qDebug() << "error avformat_write_header";
        return false;
    }

    auto video_codec = avcodec_find_decoder(vid_stream->codecpar->codec_id);
    if (!video_codec) {
        qDebug() << "error video avcodec_find_decoder";
        return false;
    }
    videoCodecContext = avcodec_alloc_context3(video_codec);

    if(avcodec_parameters_to_context(videoCodecContext, vid_stream->codecpar)<0)
        std::cout << 512;

    if (avcodec_open2(videoCodecContext, video_codec, nullptr)<0) {
        std::cout << 5;
        return false;
    }

    auto audio_codec = avcodec_find_decoder(aud_stream->codecpar->codec_id);
    if (!audio_codec) {
        qDebug() << "error audio avcodec_find_decoder";
        return false;
    }
    audioCodecContext = avcodec_alloc_context3(audio_codec);
    /* put sample parameters */

    if(avcodec_parameters_to_context(audioCodecContext, aud_stream->codecpar)<0)
        std::cout << 512;

    if (avcodec_open2(audioCodecContext, audio_codec, nullptr)<0) {
        std::cout << 5;
        return false;
    }

    video_frame = av_frame_alloc();
    if (!video_frame) {
        qDebug() << "error video av_frame_alloc";
        avcodec_free_context(&videoCodecContext);
        return false;
    }

    audio_frame = av_frame_alloc();
    if (!audio_frame) {
        qDebug() << "error audio av_frame_alloc";
        avcodec_free_context(&audioCodecContext);
        return false;
    }

    return true;
}

void ffmpeg_rtmp::set_parameters()
{
    int videoWidth = 0;
    int videoHeight = 0;

    AVCodecParameters* codecVideoParams = inputContext->streams[video_idx]->codecpar;
    AVCodecID codecVideoId = codecVideoParams->codec_id;
    auto codecVideoName = avcodec_get_name(codecVideoId);
    videoWidth = codecVideoParams->width;
    videoHeight = codecVideoParams->height;

    AVCodecParameters* codecAudioParams = inputContext->streams[audio_idx]->codecpar;
    AVCodecID codecAudioId = codecAudioParams->codec_id;
    auto codecAudioName = avcodec_get_name(codecAudioId);

    info = "Video Codec: " + QString(codecVideoName);
    emit sendInfo(info);

    info = "Video width: " + QString::number(videoWidth) + " Video height: " + QString::number(videoHeight);
    emit sendInfo(info);

    // Get pixel format name
    const char* pixelFormatName = av_get_pix_fmt_name(static_cast<AVPixelFormat>(codecVideoParams->format));
    if (!pixelFormatName) {
        info = "Unknown pixel format";
    } else {
        QString pixelFormat = QString::fromUtf8(pixelFormatName);
        info = "Video Pixel Format: " + pixelFormat;
    }
    emit sendInfo(info);

    auto deviceInfo = m_devices->defaultAudioOutput();
    QAudioFormat format = deviceInfo.preferredFormat();
    format.setSampleRate(codecAudioParams->sample_rate);
    format.setChannelCount(codecAudioParams->ch_layout.nb_channels);

    m_audioOutput.reset(new QAudioSink(deviceInfo, format));
    qreal initialVolume = QAudio::convertVolume(m_audioOutput->volume(),
                                                QAudio::LinearVolumeScale,
                                                QAudio::LogarithmicVolumeScale);

    info = "Audio Device: " + deviceInfo.description() + " Volume: " + QString::number(initialVolume);
    emit sendInfo(info);
    info = "Audio Codec: " + QString(codecAudioName) + " sr: " + QString::number(codecAudioParams->sample_rate) + " ch: " + QString::number(codecAudioParams->ch_layout.nb_channels);
    emit sendInfo(info);

    m_ioAudioDevice = m_audioOutput->start();
}

void ffmpeg_rtmp::run()
{
    m_stop = false;

    emit sendInfo("Trying to start Rtmp stream server.");

    if (!prepare_ffmpeg())
    {
        emit sendConnectionStatus(false);
        return;
    }

    // Print the video codec
    if (video_idx != -1 && audio_idx != -1) {
        set_parameters();

    } else {
        info = "Video or Audio stream not found ";
        emit sendInfo(info);
        return;
    }

    emit sendConnectionStatus(true);

    // Read packets from the input stream and write to the output file
    AVPacket* packet = av_packet_alloc();

    while (!m_stop)
    {
        int ret = av_read_frame(inputContext, packet);
        if(ret < 0)
        {
            std::cout << "there is no packet!" << ret << std::endl;
            break;
        }

        //for audio
        if (packet->stream_index == audio_idx)
        {
            //            QByteArray audioData(reinterpret_cast<const char*>(packet->data), packet->size);
            //            m_ioAudioDevice->write(audioData);
            int ret = avcodec_send_packet(audioCodecContext, packet);
            if (ret < 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                std::cout << "audio avcodec_send_packet: " << ret << std::endl;
                break;
            }
            while (ret  >= 0) {
                ret = avcodec_receive_frame(audioCodecContext, audio_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    //std::cout << "audio avcodec_receive_frame: " << ret << std::endl;
                    break;
                }

//                SwrContext *resample_context = NULL;
//                swr_alloc_set_opts2(&resample_context,
//                                                &audioCodecContext->ch_layout,
//                                                AV_SAMPLE_FMT_FLTP, 44100,
//                                                &audioCodecContext->ch_layout,
//                                                AV_SAMPLE_FMT_FLTP, 44100, 0, NULL);


//                if(swr_init(resample_context) < 0)
//                    break;

//                AVFrame* resampled_frame = av_frame_alloc();
//                resampled_frame->sample_rate = audio_frame->sample_rate;
//                resampled_frame->ch_layout = audio_frame->ch_layout;
//                resampled_frame->ch_layout.nb_channels = audio_frame->ch_layout.nb_channels;
//                resampled_frame->format = AV_SAMPLE_FMT_FLTP;

//                swr_convert_frame(resample_context, resampled_frame, audio_frame);
//                av_frame_unref(audio_frame);

                m_ioAudioDevice->write(reinterpret_cast<char*>(audio_frame->data[0]), audio_frame->linesize[0]);
                av_frame_unref(audio_frame);
            }
        }

        // for preview
        if (packet->stream_index == video_idx)
        {
            int ret = avcodec_send_packet(videoCodecContext, packet);
            if (ret < 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                std::cout << "video avcodec_send_packet: " << ret << std::endl;
                break;
            }
            while (ret  >= 0) {
                ret = avcodec_receive_frame(videoCodecContext, video_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    //std::cout << "video avcodec_receive_frame: " << ret << std::endl;
                    break;
                }

                SwsContext* swsContext = sws_getContext(video_frame->width, video_frame->height, videoCodecContext->pix_fmt,
                                                        video_frame->width, video_frame->height, AV_PIX_FMT_RGB32,
                                                        SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!swsContext) {
                    std::cout << "Failed to create SwsContext" << std::endl;
                    break;
                }

                // Initialize the SwsContext
                sws_init_context(swsContext, nullptr, nullptr);

                uint8_t* destData[1] = { nullptr };
                int destLinesize[1] = { 0 };

                QImage image(video_frame->width, video_frame->height, QImage::Format_RGB32);

                destData[0] = image.bits();
                destLinesize[0] = image.bytesPerLine();

                sws_scale(swsContext, video_frame->data, video_frame->linesize, 0, video_frame->height, destData, destLinesize);

                // Cleanup
                sws_freeContext(swsContext);
                emit sendFrame(image);
                av_frame_unref(video_frame);
            }
        }

        AVStream* inputStream = inputContext->streams[packet->stream_index];
        AVStream* outputStream = outputContext->streams[packet->stream_index];

        if (packet->stream_index >= 0 && packet->stream_index < inputContext->nb_streams)
        {
            // Rescale packet timestamps
            packet->pts = av_rescale_q(packet->pts, inputStream->time_base, outputStream->time_base);
            packet->dts = av_rescale_q(packet->dts, inputStream->time_base, outputStream->time_base);
            packet->duration = av_rescale_q(packet->duration, inputStream->time_base, outputStream->time_base);
            packet->pos = -1;
            av_interleaved_write_frame(outputContext, packet);
        }
        av_packet_unref(packet);
    }

    emit sendConnectionStatus(false);
    m_audioOutput->stop();

    //    m_audioOutput->disconnect(this);

    // Write the output file trailer
    av_write_trailer(outputContext);

    // Close input and output contexts
    avformat_close_input(&inputContext);
    if (outputContext && !(outputContext->oformat->flags & AVFMT_NOFILE))
        avio_close(outputContext->pb);
    avformat_free_context(outputContext);
}
