/**************************************************************************/
/*  ffmpeg_audio_decoder.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             EIRTeam.FFmpeg                             */
/*                         https://ph.eirteam.moe                         */
/**************************************************************************/
/* Copyright (c) 2023-present Álex Román (EIRTeam) & contributors.        */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "ffmpeg_audio_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <libavutil/samplefmt.h>
}

#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

FFmpegAudioDecoder::FFmpegAudioDecoder() {
}

FFmpegAudioDecoder::~FFmpegAudioDecoder() {
}

void FFmpegAudioDecoder::_bind_methods() {
	ClassDB::bind_method(D_METHOD("decode_to_pcm", "path"), &FFmpegAudioDecoder::decode_to_pcm);
}

Dictionary FFmpegAudioDecoder::decode_to_pcm(const String &p_path) {
	Dictionary result;

	AVFormatContext *fmt_ctx = nullptr;
	AVCodecContext *codec_ctx = nullptr;
	SwrContext *swr_ctx = nullptr;
	AVFrame *frame = nullptr;
	AVPacket *pkt = nullptr;

	// 收集到的 PCM 数据
	PackedByteArray all_samples;
	int target_sample_rate = 0;
	int target_channels = 0;

	do {
		// 1. 打开文件
		CharString path_utf8 = p_path.utf8();
		if (avformat_open_input(&fmt_ctx, path_utf8.get_data(), nullptr, nullptr) < 0) {
			UtilityFunctions::printerr("FFmpegAudioDecoder: 无法打开文件 ", p_path);
			break;
		}

		// 2. 查找流信息
		if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
			UtilityFunctions::printerr("FFmpegAudioDecoder: 无法获取流信息");
			break;
		}

		// 3. 查找最佳音频流
		AVCodec *decoder = nullptr;
		int stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
		if (stream_idx < 0) {
			UtilityFunctions::printerr("FFmpegAudioDecoder: 未找到音频流");
			break;
		}

		AVStream *audio_stream = fmt_ctx->streams[stream_idx];

		// 4. 打开解码器
		codec_ctx = avcodec_alloc_context3(decoder);
		if (!codec_ctx) {
			UtilityFunctions::printerr("FFmpegAudioDecoder: 无法分配解码器上下文");
			break;
		}
		avcodec_parameters_to_context(codec_ctx, audio_stream->codecpar);
		if (avcodec_open2(codec_ctx, decoder, nullptr) < 0) {
			UtilityFunctions::printerr("FFmpegAudioDecoder: 无法打开解码器");
			break;
		}

		target_sample_rate = codec_ctx->sample_rate;
		target_channels = codec_ctx->ch_layout.nb_channels;

		// 5. 初始化重采样器（统一输出为 s16 立体声/单声道）
		AVChannelLayout out_ch_layout;
		av_channel_layout_default(&out_ch_layout, target_channels);

		swr_ctx = swr_alloc_set_opts(nullptr,
				&out_ch_layout, AV_SAMPLE_FMT_S16, target_sample_rate,
				&codec_ctx->ch_layout, codec_ctx->sample_fmt, codec_ctx->sample_rate,
				0, nullptr);
		if (!swr_ctx || swr_init(swr_ctx) < 0) {
			UtilityFunctions::printerr("FFmpegAudioDecoder: 无法初始化重采样器");
			break;
		}

		// 6. 解码循环
		frame = av_frame_alloc();
		pkt = av_packet_alloc();
		if (!frame || !pkt) {
			UtilityFunctions::printerr("FFmpegAudioDecoder: 无法分配帧/包");
			break;
		}

		while (av_read_frame(fmt_ctx, pkt) >= 0) {
			if (pkt->stream_index != stream_idx) {
				av_packet_unref(pkt);
				continue;
			}

			if (avcodec_send_packet(codec_ctx, pkt) < 0) {
				av_packet_unref(pkt);
				continue;
			}

			while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
				// 重采样为 S16
				uint8_t *s16_buffer = nullptr;
				int out_samples = swr_get_out_samples(swr_ctx, frame->nb_samples);
				int out_buf_size = av_samples_alloc(&s16_buffer, nullptr,
						target_channels, out_samples,
						AV_SAMPLE_FMT_S16, 0);
				if (out_buf_size < 0) {
					av_frame_unref(frame);
					continue;
				}

				int converted = swr_convert(swr_ctx, &s16_buffer, out_samples,
						(const uint8_t **)frame->data, frame->nb_samples);
				if (converted > 0) {
					int bytes = converted * target_channels * 2; // s16 = 2 bytes
					PackedByteArray chunk;
					chunk.resize(bytes);
					memcpy(chunk.ptrw(), s16_buffer, bytes);
					all_samples.append_array(chunk);
				}
				av_freep(&s16_buffer);
				av_frame_unref(frame);
			}
			av_packet_unref(pkt);
		}

		// 7. 刷新解码器（可能还有残帧）
		avcodec_send_packet(codec_ctx, nullptr);
		while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
			uint8_t *s16_buffer = nullptr;
			int out_samples = swr_get_out_samples(swr_ctx, frame->nb_samples);
			int out_buf_size = av_samples_alloc(&s16_buffer, nullptr,
					target_channels, out_samples,
					AV_SAMPLE_FMT_S16, 0);
			if (out_buf_size >= 0) {
				int converted = swr_convert(swr_ctx, &s16_buffer, out_samples,
						(const uint8_t **)frame->data, frame->nb_samples);
				if (converted > 0) {
					int bytes = converted * target_channels * 2;
					PackedByteArray chunk;
					chunk.resize(bytes);
					memcpy(chunk.ptrw(), s16_buffer, bytes);
					all_samples.append_array(chunk);
				}
				av_freep(&s16_buffer);
			}
			av_frame_unref(frame);
		}

		// 8. 组装结果
		result["samples"] = all_samples;
		result["sample_rate"] = target_sample_rate;
		result["channels"] = target_channels;
		result["format"] = AudioStreamWAV::FORMAT_16_BITS;

	} while (false);

	// 清理
	if (pkt) av_packet_free(&pkt);
	if (frame) av_frame_free(&frame);
	if (swr_ctx) swr_free(&swr_ctx);
	if (codec_ctx) avcodec_free_context(&codec_ctx);
	if (fmt_ctx) avformat_close_input(&fmt_ctx);

	return result;
}
