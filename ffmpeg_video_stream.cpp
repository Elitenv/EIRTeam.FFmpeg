/**************************************************************************/
/*  ffmpeg_video_stream.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             EIRTeam.FFmpeg                             */
/*                         https://ph.eirteam.moe                         */
/**************************************************************************/
/* Copyright (c) 2023-present Álex Román (EIRTeam) & contributors.        */
/*                                                                        */
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

#include "ffmpeg_video_stream.h"
#include <iterator>

#ifdef GDEXTENSION
#include "gdextension_build/gdex_print.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/rd_shader_file.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
typedef RenderingDevice RD;
typedef RenderingServer RS;
typedef RDTextureView RDTextureViewC;
struct RDTextureFormatC {
	RenderingDevice::DataFormat format;
	int width;
	int height;
	int usage_bits;
	int depth;
	int array_layers;
	int mipmaps;

	Ref<RDTextureFormat> get_texture_format() {
		Ref<RDTextureFormat> tf;
		tf.instantiate();
		tf->set_height(height);
		tf->set_width(width);
		tf->set_usage_bits(usage_bits);
		tf->set_format(format);
		tf->set_depth(depth);
		tf->set_array_layers(array_layers);
		tf->set_mipmaps(mipmaps);
		return tf;
	}
};
RDTextureFormatC tfc_from_rdtf(Ref<RDTextureFormat> p_texture_format) {
	RDTextureFormatC tfc;
	tfc.width = p_texture_format->get_width();
	tfc.height = p_texture_format->get_height();
	tfc.usage_bits = p_texture_format->get_usage_bits();
	tfc.format = p_texture_format->get_format();
	tfc.depth = p_texture_format->get_depth();
	tfc.array_layers = p_texture_format->get_array_layers();
	tfc.mipmaps = p_texture_format->get_mipmaps();
	return tfc;
}

typedef int64_t ComputeListID;
#define TEXTURE_FORMAT_COMPAT(tf) tfc_from_rdtf(tf);
#else
#include "servers/rendering/rendering_device_binds.h"
typedef RD::TextureFormat RDTextureFormatC;
typedef RD::TextureView RDTextureViewC;
#define TEXTURE_FORMAT_COMPAT(tf) tf;
typedef RD::ComputeListID ComputeListID;
#endif

#include "tracy_import.h"
#include "yuv_to_rgb.glsl.gen.h"

#ifdef GDEXTENSION
#define FREE_RD_RID(rid) RS::get_singleton()->get_rendering_device()->free_rid(rid);
#else
#define FREE_RD_RID(rid) RS::get_singleton()->get_rendering_device()->free(rid);
#endif
void FFmpegVideoStreamPlayback::seek_into_sync() {
	decoder->seek(playback_position);
	Vector<Ref<DecodedFrame>> decoded_frames;
	for (Ref<DecodedFrame> df : available_frames) {
		decoded_frames.push_back(df);
	}
	decoder->return_frames(decoded_frames);
	available_frames.clear();
	available_audio_frames.clear();
}

double FFmpegVideoStreamPlayback::get_current_frame_time() {
	if (last_frame.is_valid()) {
		return last_frame->get_time();
	}
	return 0.0f;
}

bool FFmpegVideoStreamPlayback::check_next_frame_valid(Ref<DecodedFrame> p_decoded_frame) {
	// in the case of looping, we may start a seek back to the beginning but still receive some lingering frames from the end of the last loop. these should be allowed to continue playing.
	if (looping && Math::abs((p_decoded_frame->get_time() - decoder->get_duration()) - playback_position) < LENIENCE_BEFORE_SEEK)
		return true;

	return p_decoded_frame->get_time() <= playback_position && Math::abs(p_decoded_frame->get_time() - playback_position) < LENIENCE_BEFORE_SEEK;
}

bool FFmpegVideoStreamPlayback::check_next_audio_frame_valid(Ref<DecodedAudioFrame> p_decoded_frame) {
	// in the case of looping, we may start a seek back to the beginning but still receive some lingering frames from the end of the last loop. these should be allowed to continue playing.
	if (looping && Math::abs((p_decoded_frame->get_time() - decoder->get_duration()) - playback_position) < LENIENCE_BEFORE_SEEK)
		return true;

	return p_decoded_frame->get_time() <= playback_position && Math::abs(p_decoded_frame->get_time() - playback_position) < LENIENCE_BEFORE_SEEK;
}

const char *const upd_str = "update_internal";

void FFmpegVideoStreamPlayback::update_internal(double p_delta) {
	ZoneScopedN("update_internal");

	if (paused || !playing) {
		return;
	}
	uint64_t pt0 = prof_enabled ? Time::get_singleton()->get_ticks_usec() : 0;
	if (prof_enabled) {
		if (prof_last_end != 0) {
			prof_gap_acc += pt0 - prof_last_end;
		}
		prof_delta_last = p_delta;
	}

	playback_position += p_delta * 1000.0f;

	if (decoder->get_decoder_state() == VideoDecoder::DecoderState::END_OF_STREAM && available_frames.size() == 0) {
		if (playback_position < decoder->get_last_decoded_frame_time()) {
			// The decode thread finished ahead of real-time playback (short videos decode
			// in milliseconds). The remaining frames are still in the decoder's queue and
			// drain through get_decoded_frames() as playback_position advances — just wait.
			// Do NOT seek here (the old seek-into-sync caused a seek/re-decode storm: the
			// thread hits EOF again within ms and the main thread stalls on the synchronous
			// seek — 100% CPU spin, frames discarded, BGA video crawl) and do NOT stop
			// (the node-level loop would restart immediately — the same storm).
		} else if (decoder->get_last_decoded_frame_time() <= 0.0) {
			// ⚠️ 一帧都还没解出来（首帧 / seek 之后立刻报 EOF）：**绝不能**报 playing=false。
			// 引擎（VideoStreamPlayer）看到 !is_playing() 会每帧调用 play() → 我们这边
			// play_internal() 里那次**同步** seek(0,true) 会阻塞主线程 ~47ms/帧，而解码线程每帧
			// 都被 seek 打断 → 永远解不出第一帧 → 自持的 seek 风暴（2026-10-04 实机插桩实证：
			// play=120/47.33ms per 120 updates、accepted=0、pos=0、整帧 50ms = 20fps）。
			// 保持 playing=true，让解码线程跑出第一帧。
		} else {
			// Genuine end of stream: freeze on the last frame (non-looping) or let the
			// node-level loop restart the video from 0.
			playing = false;
		}
	}

	Ref<DecodedFrame> peek_frame = available_frames.size() > 0 ? available_frames.front()->get() : nullptr;
	bool out_of_sync = false;

	if (peek_frame.is_valid()) {
		out_of_sync = Math::abs(playback_position - peek_frame->get_time()) > LENIENCE_BEFORE_SEEK;

		if (looping) {
			out_of_sync &= Math::abs(playback_position - decoder->get_duration() - peek_frame->get_time()) > LENIENCE_BEFORE_SEEK &&
					Math::abs(playback_position + decoder->get_duration() - peek_frame->get_time()) > LENIENCE_BEFORE_SEEK;
		}
	}

	if (out_of_sync) {
		print_line(vformat("Video too far out of sync (%.2f), seeking to %.2f", peek_frame->get_time(), playback_position));
		seek_into_sync();
	}
	uint64_t pt1 = prof_enabled ? Time::get_singleton()->get_ticks_usec() : 0;

	double frame_time = get_current_frame_time();

	bool got_new_frame = false;

	List<Ref<DecodedFrame>>::Element *next_frame = available_frames.front();
	while (next_frame && (check_next_frame_valid(next_frame->get()) || just_seeked)) {
		ZoneNamedN(__frame_receive, "frame_receive", true);

		just_seeked = false;

		if (last_frame.is_valid()) {
			decoder->return_frame(last_frame);
		}
		last_frame = next_frame->get();
		last_frame_image = last_frame->get_image();
#ifdef FFMPEG_MT_GPU_UPLOAD
		last_frame_texture = last_frame->get_texture();
#endif
		got_new_frame = true;
		next_frame = next_frame->next();
		available_frames.pop_front();
	}
	uint64_t pt2 = prof_enabled ? Time::get_singleton()->get_ticks_usec() : 0;
#ifndef FFMPEG_MT_GPU_UPLOAD
	if (got_new_frame) {
		// YUV conversion
		if (last_frame->get_format() == FFmpegFrameFormat::YUV420P || last_frame->get_format() == FFmpegFrameFormat::YUVA420P) {
			Ref<Image> y_plane = last_frame->get_yuv_image_plane(0);
			Ref<Image> u_plane = last_frame->get_yuv_image_plane(1);
			Ref<Image> v_plane = last_frame->get_yuv_image_plane(2);
			Ref<Image> a_plane = last_frame->get_yuv_image_plane(3);

			ERR_FAIL_COND(!y_plane.is_valid());
			ERR_FAIL_COND(!u_plane.is_valid());
			ERR_FAIL_COND(!v_plane.is_valid());

			yuv_converter->set_plane_image(0, y_plane);
			yuv_converter->set_plane_image(1, u_plane);
			yuv_converter->set_plane_image(2, v_plane);
			yuv_converter->set_plane_image(3, a_plane);
			yuv_converter->convert();
			// RGBA texture handling
		} else if (texture.is_valid()) {
			if (texture->get_size() != last_frame_image->get_size() || texture->get_format() != last_frame_image->get_format()) {
				ZoneNamedN(__img_upate_slow, "Image update slow", true);
				texture->set_image(last_frame_image); // should never happen, but life has many doors ed-boy...
			} else {
				ZoneNamedN(__img_upate_fast, "Image update fast", true);
				texture->update(last_frame_image);
			}
		}
	}
#endif
	uint64_t pt3 = prof_enabled ? Time::get_singleton()->get_ticks_usec() : 0;

	int refilled = 0;
	if (available_frames.size() == 0) {
		for (Ref<DecodedFrame> frame : decoder->get_decoded_frames()) {
			available_frames.push_back(frame);
			refilled++;
		}
	}
	uint64_t pt4 = prof_enabled ? Time::get_singleton()->get_ticks_usec() : 0;

	Ref<DecodedAudioFrame> peek_audio_frame;
	if (available_audio_frames.size() > 0) {
		peek_audio_frame = available_audio_frames.front()->get();
	}

	bool audio_out_of_sync = false;

	if (peek_audio_frame.is_valid()) {
		audio_out_of_sync = Math::abs(playback_position - peek_audio_frame->get_time()) > LENIENCE_BEFORE_SEEK;

		if (looping) {
			out_of_sync &= Math::abs(playback_position - decoder->get_duration() - peek_audio_frame->get_time()) > LENIENCE_BEFORE_SEEK &&
					Math::abs(playback_position + decoder->get_duration() - peek_audio_frame->get_time()) > LENIENCE_BEFORE_SEEK;
		}
	}

	if (audio_out_of_sync) {
		// TODO: seek audio stream individually if it desyncs
	}

	List<Ref<DecodedAudioFrame>>::Element *next_audio_frame = available_audio_frames.front();
	while (next_audio_frame && check_next_audio_frame_valid(next_audio_frame->get())) {
		ZoneNamedN(__audio_mix, "Audio mix", true);
		Ref<DecodedAudioFrame> audio_frame = next_audio_frame->get();
		int sample_count = audio_frame->get_sample_data().size() / decoder->get_audio_channel_count();
#ifdef GDEXTENSION
		mix_audio(sample_count, audio_frame->get_sample_data(), 0);
#else
		mix_callback(mix_udata, audio_frame->get_sample_data().ptr(), sample_count);
#endif
		next_audio_frame = next_audio_frame->next();
		available_audio_frames.pop_front();
	}
	if (available_audio_frames.size() == 0) {
		for (Ref<DecodedAudioFrame> frame : decoder->get_decoded_audio_frames()) {
			available_audio_frames.push_back(frame);
		}
	}
	uint64_t pt5 = prof_enabled ? Time::get_singleton()->get_ticks_usec() : 0;

	buffering = decoder->is_running() && available_frames.size() == 0;

	if (frame_time != get_current_frame_time()) {
		frames_processed++;
	}

	if (prof_enabled) {
		uint64_t pt6 = Time::get_singleton()->get_ticks_usec();
		prof_last_end = pt6;
		prof_calls++;
		if (got_new_frame) {
			prof_accepted++;
		}
		prof_refilled += refilled;
		prof_total_acc += pt6 - pt0;
		prof_acc[0] += pt1 - pt0;
		prof_acc[1] += pt2 - pt1;
		prof_acc[2] += pt3 - pt2;
		prof_acc[3] += pt4 - pt3;
		prof_acc[4] += pt5 - pt4;
		prof_acc[5] += pt6 - pt5;
		if (prof_calls >= 120) {
			double inv = 1.0 / double(prof_calls);
			double total_ms = double(prof_total_acc) * inv / 1000.0;
			print_line(vformat("[ffmpeg-prof] calls=%d total=%.2fms gap=%.2fms delta=%.4f head=%.2f drain=%.2f yuv=%.2f refill=%.2f audio=%.2f tail=%.2f texq=%.2fms/%d play=%d/%.2fms seek=%d/%.2fms stop=%d | accepted=%d refilled=%d avail=%d pos=%.0f lastf=%.0f",
					prof_calls, total_ms, double(prof_gap_acc) * inv / 1000.0, prof_delta_last, double(prof_acc[0]) * inv / 1000.0, double(prof_acc[1]) * inv / 1000.0,
					double(prof_acc[2]) * inv / 1000.0, double(prof_acc[3]) * inv / 1000.0,
					double(prof_acc[4]) * inv / 1000.0, double(prof_acc[5]) * inv / 1000.0,
						double(prof_texq_acc) * inv / 1000.0, prof_texq_calls,
					prof_play_calls, double(prof_play_acc) * inv / 1000.0, prof_seek_calls, double(prof_seek_acc) * inv / 1000.0, prof_stop_calls,
					prof_accepted, prof_refilled, available_frames.size(), playback_position, decoder->get_last_decoded_frame_time()));
			prof_calls = 0;
			prof_accepted = 0;
			prof_refilled = 0;
			prof_total_acc = 0;
			prof_texq_calls = 0;
			prof_texq_acc = 0;
			prof_gap_acc = 0;
			prof_play_calls = 0;
			prof_play_acc = 0;
			prof_seek_calls = 0;
			prof_seek_acc = 0;
			prof_stop_calls = 0;
			for (int i = 0; i < 6; i++) {
				prof_acc[i] = 0;
			}
		}
	}
}

Error FFmpegVideoStreamPlayback::load(Ref<FileAccess> p_file_access) {
	prof_enabled = bool(ProjectSettings::get_singleton()->get_setting("debug/ffmpeg_bga_prof", false));
	prof_calls = 0;
	prof_accepted = 0;
	prof_refilled = 0;
	prof_total_acc = 0;
	for (int i = 0; i < 6; i++) {
		prof_acc[i] = 0;
	}
	decoder = Ref<VideoDecoder>(memnew(VideoDecoder(p_file_access)));

	decoder->start_decoding();
	Vector2i size = decoder->get_size();
	if (decoder->get_decoder_state() == VideoDecoder::FAULTED) {
		return FAILED;
	}

	if (decoder->get_frame_format() == FFmpegFrameFormat::YUV420P || decoder->get_frame_format() == FFmpegFrameFormat::YUVA420P) {
		yuv_converter.instantiate();
		yuv_converter->set_frame_size(size);
		yuv_texture = yuv_converter->get_output_texture();
	} else {
#ifdef GDEXTENSION
		texture = ImageTexture::create_from_image(Image::create(size.x, size.y, false, Image::FORMAT_RGBA8));
#else
		texture = ImageTexture::create_from_image(Image::create_empty(size.x, size.y, false, Image::FORMAT_RGBA8));
#endif
	}
	return OK;
}

bool FFmpegVideoStreamPlayback::is_paused_internal() const {
	return paused;
}

bool FFmpegVideoStreamPlayback::is_playing_internal() const {
	return playing;
}

void FFmpegVideoStreamPlayback::set_paused_internal(bool p_paused) {
	paused = p_paused;
}

void FFmpegVideoStreamPlayback::play_internal() {
	uint64_t pl0 = prof_enabled ? Time::get_singleton()->get_ticks_usec() : 0;
	prof_play_calls++;
	if (decoder->get_decoder_state() == VideoDecoder::FAULTED) {
		playing = false;
		return;
	}
	clear();
	playback_position = 0;
	// 只在解码线程**尚未产出任何帧**时才做这次同步 seek（它是阻塞主线程的）：
	// 若已经解出过帧（例如引擎重复调用 play()），再 seek(0,true) 会白等一次完整 seek。
	if (!decoder->is_running() || decoder->get_last_decoded_frame_time() <= 0.0) {
		decoder->seek(0, true);
	}
	just_seeked = true;
	playing = true;
	if (prof_enabled) {
		prof_play_acc += Time::get_singleton()->get_ticks_usec() - pl0;
	}
}

void FFmpegVideoStreamPlayback::stop_internal() {
	prof_stop_calls++;
	if (playing) {
		clear();
		playback_position = 0.0f;
		decoder->seek(playback_position, true);
		just_seeked = true;
		texture.unref();
	}
	if (yuv_converter.is_valid()) {
		yuv_converter->clear_output_texture();
	}
	playing = false;
}

void FFmpegVideoStreamPlayback::seek_internal(double p_time) {
	uint64_t sk0 = prof_enabled ? Time::get_singleton()->get_ticks_usec() : 0;
	prof_seek_calls++;
	decoder->seek(p_time * 1000.0f);
	just_seeked = true;
	available_frames.clear();
	available_audio_frames.clear();
	playback_position = p_time * 1000.0f;
	if (prof_enabled) {
		prof_seek_acc += Time::get_singleton()->get_ticks_usec() - sk0;
	}
}

double FFmpegVideoStreamPlayback::get_length_internal() const {
	return decoder->get_duration() / 1000.0f;
}

Ref<Texture2D> FFmpegVideoStreamPlayback::get_texture_internal() const {
	// ⚠️ 引擎**每帧**都调用这里取纹理；YUV 路径里 get_output_texture() 会走 _ensure_output_texture()
	// （RD 调用）——它不在 update_internal 里，必须单独计时，否则会把开销算漏（2026-10-04）。
	uint64_t q0 = prof_enabled ? Time::get_singleton()->get_ticks_usec() : 0;
	Ref<Texture2D> ret;
#ifdef FFMPEG_MT_GPU_UPLOAD
	ret = last_frame_texture;
#else
	if (yuv_converter.is_valid()) {
		ret = yuv_converter->get_output_texture();
	} else {
		ret = texture;
	}
#endif
	if (prof_enabled) {
		prof_texq_calls++;
		prof_texq_acc += Time::get_singleton()->get_ticks_usec() - q0;
	}
	return ret;
}

double FFmpegVideoStreamPlayback::get_playback_position_internal() const {
	return playback_position / 1000.0;
}

int FFmpegVideoStreamPlayback::get_mix_rate_internal() const {
	return decoder->get_audio_mix_rate();
}

int FFmpegVideoStreamPlayback::get_channels_internal() const {
	return decoder->get_audio_channel_count();
}

FFmpegVideoStreamPlayback::FFmpegVideoStreamPlayback() {
}

void FFmpegVideoStreamPlayback::clear() {
	last_frame.unref();
	last_frame_texture.unref();
	available_frames.clear();
	available_audio_frames.clear();
	frames_processed = 0;
	playing = false;
}

YUVGPUConverter::~YUVGPUConverter() {
	for (size_t i = 0; i < std::size(yuv_planes_uniform_sets); i++) {
		if (yuv_planes_uniform_sets[i].is_valid()) {
			FREE_RD_RID(yuv_planes_uniform_sets[i]);
		}
		if (yuv_plane_textures[i].is_valid()) {
			FREE_RD_RID(yuv_plane_textures[i]);
		}
	}

	if (out_texture.is_valid() && out_texture->get_texture_rd_rid().is_valid()) {
		RID out_rid = out_texture->get_texture_rd_rid();
		out_texture->set_texture_rd_rid(RID());
		FREE_RD_RID(out_rid);
	}

	if (pipeline.is_valid()) {
		FREE_RD_RID(pipeline);
	}

	if (shader.is_valid()) {
		FREE_RD_RID(shader);
	}
}

void YUVGPUConverter::_clear_texture_internal() {
	if (out_texture.is_valid() && out_texture->get_texture_rd_rid().is_valid()) {
		RD *rd = RS::get_singleton()->get_rendering_device();
		rd->texture_clear(out_texture->get_texture_rd_rid(), Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
}

void YUVGPUConverter::_ensure_pipeline() {
	if (pipeline.is_valid()) {
		return;
	}

	RD *rd = RS::get_singleton()->get_rendering_device();

#ifdef GDEXTENSION

	Ref<RDShaderSource> shader_source;
	shader_source.instantiate();
	// Ugly hack to skip the #[compute] in the header, because parse_versions_from_text is not available through GDNative
	shader_source->set_stage_source(RenderingDevice::ShaderStage::SHADER_STAGE_COMPUTE, yuv_to_rgb_shader_glsl + 10);
	Ref<RDShaderSPIRV> shader_spirv = rd->shader_compile_spirv_from_source(shader_source);

#else

	Ref<RDShaderFile> shader_file;
	shader_file.instantiate();
	Error err = shader_file->parse_versions_from_text(yuv_to_rgb_shader_glsl);
	if (err != OK) {
		print_line("Something catastrophic happened, call eirexe");
	}
	Vector<RD::ShaderStageSPIRVData> shader_spirv = shader_file->get_spirv_stages();

#endif
	shader = rd->shader_create_from_spirv(shader_spirv);
	pipeline = rd->compute_pipeline_create(shader);
}

Error YUVGPUConverter::_ensure_plane_textures() {
	RD *rd = RS::get_singleton()->get_rendering_device();
	for (size_t i = 0; i < std::size(yuv_plane_textures); i++) {
		if (yuv_plane_textures[i].is_valid()) {
			RDTextureFormatC format = TEXTURE_FORMAT_COMPAT(rd->texture_get_format(yuv_plane_textures[i]));

			int desired_frame_width = i == 0 || i == 3 ? frame_size.width : Math::ceil(frame_size.width / 2.0f);
			int desired_frame_height = i == 0 || i == 3 ? frame_size.height : Math::ceil(frame_size.height / 2.0f);

			if (static_cast<int>(format.width) == desired_frame_width && static_cast<int>(format.height) == desired_frame_height) {
				continue;
			}
			continue;
		}

		// Texture didn't exist or was invalid, re-create it

		// free existing texture if needed
		if (yuv_plane_textures[i].is_valid()) {
			FREE_RD_RID(yuv_plane_textures[i]);
		}

		RDTextureFormatC new_format;
		new_format.format = RenderingDevice::DATA_FORMAT_R8_UNORM;
		// chroma planes are half the size of the luma plane
		new_format.width = i == 0 || i == 3 ? frame_size.width : Math::ceil(frame_size.width / 2.0f);
		new_format.height = i == 0 || i == 3 ? frame_size.height : Math::ceil(frame_size.height / 2.0f);
		new_format.depth = 1;
		new_format.array_layers = 1;
		new_format.mipmaps = 1;
		new_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;

#ifdef GDEXTENSION
		Ref<RDTextureFormat> new_format_c = new_format.get_texture_format();
		Ref<RDTextureViewC> texture_view;
		texture_view.instantiate();
#else
		RD::TextureFormat new_format_c = new_format;
		RDTextureViewC texture_view;
#endif
		yuv_plane_textures[i] = rd->texture_create(new_format_c, texture_view);

		if (yuv_planes_uniform_sets[i].is_valid()) {
			FREE_RD_RID(yuv_planes_uniform_sets[i]);
		}

		yuv_planes_uniform_sets[i] = _create_uniform_set(yuv_plane_textures[i], i);
	}

	return OK;
}

Error YUVGPUConverter::_ensure_output_texture() {
	_ensure_pipeline();
	RD *rd = RS::get_singleton()->get_rendering_device();
	if (!out_texture.is_valid()) {
		out_texture.instantiate();
	}

	if (out_texture->get_texture_rd_rid().is_valid()) {
		RDTextureFormatC format = TEXTURE_FORMAT_COMPAT(rd->texture_get_format(out_texture->get_texture_rd_rid()));
		if (static_cast<int>(format.width) == frame_size.width && static_cast<int>(format.height) == frame_size.height) {
			return OK;
		}
	}

	if (out_texture->get_texture_rd_rid().is_valid()) {
		FREE_RD_RID(out_texture->get_texture_rd_rid());
	}

	RDTextureFormatC out_texture_format;
	out_texture_format.format = RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM;
	out_texture_format.width = frame_size.width;
	out_texture_format.height = frame_size.height;
	out_texture_format.depth = 1;
	out_texture_format.array_layers = 1;
	out_texture_format.mipmaps = 1;
	// RD::TEXTURE_USAGE_CAN_UPDATE_BIT not needed since we won't update it from the CPU
	out_texture_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

#ifdef GDEXTENSION
	Ref<RDTextureView> texture_view;
	texture_view.instantiate();
	Ref<RDTextureFormat> out_texture_format_c = out_texture_format.get_texture_format();
#else
	RD::TextureFormat out_texture_format_c = out_texture_format;
	RD::TextureView texture_view;
#endif
	out_texture->set_texture_rd_rid(rd->texture_create(out_texture_format_c, texture_view));
	rd->texture_clear(out_texture->get_texture_rd_rid(), Color(0, 0, 0, 0), 0, 1, 0, 1);

	if (out_uniform_set.is_valid()) {
		FREE_RD_RID(out_uniform_set);
	}
	out_uniform_set = _create_uniform_set(out_texture->get_texture_rd_rid(), 4);
	return OK;
}

RID YUVGPUConverter::_create_uniform_set(const RID &p_texture_rd_rid, int p_shader_set) {
#ifdef GDEXTENSION
	Ref<RDUniform> uniform;
	uniform.instantiate();
	uniform->set_binding(0);
	uniform->set_uniform_type(RD::UNIFORM_TYPE_IMAGE);
	uniform->add_id(p_texture_rd_rid);
	TypedArray<RDUniform> uniforms;
	uniforms.push_back(uniform);
#else
	RD::Uniform uniform;
	uniform.uniform_type = RD::UNIFORM_TYPE_IMAGE;
	uniform.binding = 0;
	uniform.append_id(p_texture_rd_rid);
	Vector<RD::Uniform> uniforms;
	uniforms.push_back(uniform);
#endif
	return RS::get_singleton()->get_rendering_device()->uniform_set_create(uniforms, shader, p_shader_set);
}

void YUVGPUConverter::_upload_plane_images() {
	for (size_t i = 0; i < std::size(yuv_plane_images); i++) {
		ERR_CONTINUE_MSG(!yuv_plane_images[i].is_valid() && i != 3, vformat("YUV plane %d was missing, cannot upload texture data.", (int)i));
		if (!yuv_plane_images[i].is_valid()) {
			continue;
		}
		RS::get_singleton()->get_rendering_device()->texture_update(yuv_plane_textures[i], 0, yuv_plane_images[i]->get_data());
	}
}

void YUVGPUConverter::set_plane_image(int p_plane_idx, Ref<Image> p_image) {
	if (!p_image.is_valid()) {
		yuv_plane_images[p_plane_idx] = p_image;
		return;
	}
	ERR_FAIL_COND(!p_image.is_valid());
	ERR_FAIL_INDEX((size_t)p_plane_idx, std::size(yuv_plane_images));
	// Sanity checks
	int desired_frame_width = p_plane_idx == 0 || p_plane_idx == 3 ? frame_size.width : Math::ceil(frame_size.width / 2.0f);
	int desired_frame_height = p_plane_idx == 0 || p_plane_idx == 3 ? frame_size.height : Math::ceil(frame_size.height / 2.0f);
	ERR_FAIL_COND_MSG(p_image->get_width() != desired_frame_width, vformat("Wrong YUV plane width for plane %d, expected %d got %d", p_plane_idx, desired_frame_width, p_image->get_width()));
	ERR_FAIL_COND_MSG(p_image->get_height() != desired_frame_height, vformat("Wrong YUV plane height for plane %, expected %d got %d", p_plane_idx, desired_frame_height, p_image->get_height()));
	ERR_FAIL_COND_MSG(p_image->get_format() != Image::FORMAT_R8, "Wrong image format, expected R8");
	yuv_plane_images[p_plane_idx] = p_image;
}

Vector2i YUVGPUConverter::get_frame_size() const { return frame_size; }

void YUVGPUConverter::set_frame_size(const Vector2i &p_frame_size) {
	ERR_FAIL_COND_MSG(p_frame_size.x == 0, "Frame size cannot be zero!");
	ERR_FAIL_COND_MSG(p_frame_size.y == 0, "Frame size cannot be zero!");
	frame_size = p_frame_size;

	yuv_plane_images[0].unref();
	yuv_plane_images[1].unref();
	yuv_plane_images[2].unref();
}

void YUVGPUConverter::convert() {
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &YUVGPUConverter::_convert_internal));
}

void YUVGPUConverter::_convert_internal() {
	// First we must ensure everything we need exists
	_ensure_pipeline();
	_ensure_plane_textures();
	_ensure_output_texture();
	_upload_plane_images();

	RD *rd = RS::get_singleton()->get_rendering_device();

	push_constant.use_alpha = yuv_plane_images[3].is_valid() ? 1 : 0;

	PackedByteArray push_constant_data;
	push_constant_data.resize(sizeof(push_constant));
	memcpy(push_constant_data.ptrw(), &push_constant, push_constant_data.size());

	ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, pipeline);

#ifdef GDEXTENSION
	rd->compute_list_set_push_constant(compute_list, push_constant_data, push_constant_data.size());
#else
	rd->compute_list_set_push_constant(compute_list, push_constant_data.ptr(), push_constant_data.size());
#endif
	rd->compute_list_bind_uniform_set(compute_list, yuv_planes_uniform_sets[0], 0);
	rd->compute_list_bind_uniform_set(compute_list, yuv_planes_uniform_sets[1], 1);
	rd->compute_list_bind_uniform_set(compute_list, yuv_planes_uniform_sets[2], 2);
	rd->compute_list_bind_uniform_set(compute_list, yuv_planes_uniform_sets[3], 3);
	rd->compute_list_bind_uniform_set(compute_list, out_uniform_set, 4);

	rd->compute_list_dispatch(compute_list, Math::ceil(frame_size.x / 8.0f), Math::ceil(frame_size.y / 8.0f), 1);
	rd->compute_list_end();
}

Ref<Texture2D> YUVGPUConverter::get_output_texture() const {
	const_cast<YUVGPUConverter *>(this)->_ensure_output_texture();
	return out_texture;
}

void YUVGPUConverter::clear_output_texture() {
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &YUVGPUConverter::_clear_texture_internal));
}

YUVGPUConverter::YUVGPUConverter() {
	out_texture.instantiate();
}
