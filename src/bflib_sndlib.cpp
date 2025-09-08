#include "pre_inc.h"
#include "config_keeperfx.h"
#include "cdrom.h"
#include "bflib_sndlib.h"
#include "bflib_datetm.h"
#include "bflib_sound.h"
#include "bflib_fileio.h"

// See: https://trac.ffmpeg.org/ticket/3626
extern "C" {
	#include <libswresample/swresample.h>
}

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <SDL2/SDL.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <fstream>
#include <string>
#include <utility>
#include <array>
#include <deque>
#include <mutex>
#include <atomic>
#include <set>
#include <list>
#include <algorithm>

#include "post_inc.h"

namespace {

struct device_deleter {
	void operator()(ALCdevice * device) {
		alcCloseDevice(device);
	}
};

struct context_deleter {
	void operator()(ALCcontext * context) {
		alcMakeContextCurrent(nullptr);
		alcDestroyContext(context);
	}
};

using ALCdevice_ptr = std::unique_ptr<ALCdevice, device_deleter>;
using ALCcontext_ptr = std::unique_ptr<ALCcontext, context_deleter>;

SoundVolume g_master_volume = 0;
SoundVolume g_effects_volume = 0;
SoundVolume g_music_volume = 0;
SoundVolume g_mentor_volume = 0;
ALCdevice_ptr g_openal_device;
ALCcontext_ptr g_openal_context;
std::set<uint32_t> g_tick_samples;
bool g_bb_king_mode = false;
SDL_AudioDeviceID g_sdl_device = 0;
Uint8 g_device_num_channels = 0;
int g_device_sample_rate = 0;
SDL_AudioFormat g_device_format = 0;
std::mutex g_sdl_lock;

enum source_flags {
	bb_king_mode = 1,
};

const char * alErrorStr(ALenum code) {
	switch (code) {
		case AL_NO_ERROR: return "No error";
		case AL_INVALID_NAME: return "Invalid name";
		case AL_INVALID_ENUM: return "Invalid enum value";
		case AL_INVALID_VALUE: return "Invalid value";
		case AL_INVALID_OPERATION: return "Invalid operation";
		case AL_OUT_OF_MEMORY: return "Out of memory";
	}
	return "Unknown";
}

class openal_error : public std::runtime_error {
public:
	inline openal_error(const char * description, ALenum errcode = alGetError())
	: runtime_error(std::string("OpenAL error: ") + description + ": " + alErrorStr(errcode))
	{}
};

class openal_buffer {
public:
	ALuint id = 0;

	openal_buffer() {
		ALuint buffers[1];
		alGenBuffers(1, buffers);
		const auto errcode = alGetError();
		if (errcode != AL_NO_ERROR) {
			throw openal_error("Cannot create buffer", errcode);
		}
		id = buffers[0];
	}

	inline ~openal_buffer() noexcept {
		alDeleteBuffers(1, &id);
	}

	openal_buffer(const openal_buffer &) = delete;
	openal_buffer & operator=(const openal_buffer &) = delete;

	inline openal_buffer(openal_buffer && other)
	: id(std::exchange(other.id, 0)) {}

	inline openal_buffer & operator=(openal_buffer && other) {
		id = std::exchange(other.id, 0);
		return *this;
	}
};

class openal_source {
public:
	ALuint id = 0;
	SoundMilesID mss_id = 0;
	SoundEmitterID emit_id = 0;
	SoundSmplTblID smptbl_id = 0;
	SoundBankID bank_id = 0;
	int flags = 0;

	openal_source() {
		ALuint sources[1];
		alGenSources(1, sources);
		const auto errcode = alGetError();
		if (errcode != AL_NO_ERROR) {
			throw openal_error("Cannot create source", errcode);
		}
		id = sources[0];
	}

	inline ~openal_source() noexcept {
		alDeleteSources(1, &id);
	}

	void play(const openal_buffer & buffer) {
		alSourcei(id, AL_BUFFER, buffer.id);
		auto errcode = alGetError();
		if (errcode != AL_NO_ERROR) {
			throw openal_error("Cannot attach buffer", errcode);
		}
		alSourcePlay(id);
		errcode = alGetError();
		if (errcode != AL_NO_ERROR) {
			throw openal_error("Cannot play source", errcode);
		}
	}

	void stop() {
		alSourceStop(id);
		const auto errcode = alGetError();
		if (errcode != AL_NO_ERROR) {
			throw openal_error("Cannot stop source", errcode);
		}
	}

	void gain(SoundVolume volume) {
		alSourcef(id, AL_GAIN, float(volume) / FULL_LOUDNESS);
		const auto errcode = alGetError();
		if (errcode != AL_NO_ERROR) {
			throw openal_error("Cannot set volume", errcode);
		}
	}

	void pitch(SoundPitch pitch) {
		alSourcef(id, AL_PITCH, float(pitch) / NORMAL_PITCH);
		const auto errcode = alGetError();
		if (errcode != AL_NO_ERROR) {
			throw openal_error("Cannot set pitch", errcode);
		}
	}

	void pan(SoundPan pan) {
		// convert 0..128 (where 64 is center) to -1.0..1.0 and then reduce stereo separation by 50%
		const auto x = (-(float(64 - pan) / 64.0f)) * 0.5f;
		const auto z = -1.0f; // in front of listener
		alSource3f(id, AL_POSITION, x, 0, z);
		const auto errcode = alGetError();
		if (errcode != AL_NO_ERROR) {
			throw openal_error("Cannot set position", errcode);
		}
	}

	void repeat(bool value) {
		alSourcei(id, AL_LOOPING, value ? AL_TRUE : AL_FALSE);
		const auto errcode = alGetError();
		if (errcode != AL_NO_ERROR) {
			throw openal_error("Cannot toggle looping", errcode);
		}
	}

	bool is_playing() const {
		ALint state = 0;
		alGetSourcei(id, AL_SOURCE_STATE, &state);
		const auto errcode = alGetError();
		if (errcode != AL_NO_ERROR) {
			throw openal_error("Cannot get source state", errcode);
		}
		return state == AL_PLAYING;
	}

	openal_source(const openal_source &) = delete;
	openal_source & operator=(const openal_source &) = delete;

	inline openal_source(openal_source && other)
	: id(std::exchange(other.id, 0))
	, mss_id(std::exchange(other.mss_id, 0))
	, emit_id(std::exchange(other.emit_id, 0))
	, smptbl_id(std::exchange(other.smptbl_id, 0))
	, bank_id(std::exchange(other.bank_id, 0)){}

	inline openal_source & operator=(openal_source && other) {
		id = std::exchange(other.id, 0);
		mss_id = std::exchange(other.mss_id, 0);
		emit_id = std::exchange(other.emit_id, 0);
		smptbl_id = std::exchange(other.smptbl_id, 0);
		bank_id = std::exchange(other.bank_id, 0);
		return *this;
	}
};

inline uint32_t make_fourcc(const char (& code)[5]) {
	return
		(uint32_t(code[0]) << 0) |
		(uint32_t(code[1]) << 8) |
		(uint32_t(code[2]) << 16) |
		(uint32_t(code[3]) << 24);
}

#define WAVE_FORMAT_PCM 1
#define WAVE_FORMAT_ADPCM 2

#pragma pack(1)
struct riff_chunk_t {
	uint32_t tag;
	uint32_t size;
	// zero or more bytes of data
	// padding byte if data size not a multiple of two
};
#pragma pack()

#pragma pack(1)
struct WAVEFORMATEX {
	uint16_t wFormatTag;
	uint16_t nChannels;
	uint32_t nSamplesPerSec;
	uint32_t nAvgBytesPerSec;
	uint16_t nBlockAlign;
	uint16_t wBitsPerSample;
	// uint16_t cbSize;
};
#pragma pack()

class wave_file {
public:
	wave_file(std::ifstream & stream) {
		riff_chunk_t riff_header;
		stream.read(reinterpret_cast<char *>(&riff_header), sizeof(riff_header));
		if (riff_header.tag != make_fourcc("RIFF")) {
			throw std::runtime_error("Expected RIFF chunk");
		}
		uint32_t filetype;
		stream.read(reinterpret_cast<char *>(&filetype), sizeof(filetype));
		if (filetype != make_fourcc("WAVE")) {
			throw std::runtime_error("Expected WAVE chunk");
		}
		riff_chunk_t chunk;
		for (bool have_format = false, have_data = false; !(have_format && have_data);) {
			stream.read(reinterpret_cast<char *>(&chunk), sizeof(chunk));
			if (chunk.tag == make_fourcc("fmt ")) {
				if (chunk.size < sizeof(WAVEFORMATEX)) {
					throw std::runtime_error("Expected WAVEFORMATEX struct");
				}
				WAVEFORMATEX formatex;
				stream.read(reinterpret_cast<char *>(&formatex), sizeof(formatex));
				if (!(formatex.wFormatTag == WAVE_FORMAT_PCM || formatex.wFormatTag == WAVE_FORMAT_ADPCM)) {
					throw std::runtime_error("Unsupported format");
				} else if (formatex.nChannels == 1 && formatex.wBitsPerSample == 4) {
					m_format = AL_FORMAT_MONO_MSADPCM_SOFT;
				} else if (formatex.nChannels == 1 && formatex.wBitsPerSample == 8) {
					m_format = AL_FORMAT_MONO8;
				} else if (formatex.nChannels == 1 && formatex.wBitsPerSample == 16) {
					m_format = AL_FORMAT_MONO16;
				} else if (formatex.nChannels == 2 && formatex.wBitsPerSample == 4) {
					m_format = AL_FORMAT_STEREO_MSADPCM_SOFT;
				} else if (formatex.nChannels == 2 && formatex.wBitsPerSample == 8) {
					m_format = AL_FORMAT_STEREO8;
				} else if (formatex.nChannels == 2 && formatex.wBitsPerSample == 16) {
					m_format = AL_FORMAT_STEREO16;
				} else {
					throw std::runtime_error("Unsupported format");
				}
				m_samplerate = formatex.nSamplesPerSec;
				if (chunk.size > sizeof(formatex)) {
					stream.seekg(chunk.size - sizeof(formatex), std::ios::cur);
				}
				have_format = true;
			} else if (chunk.tag == make_fourcc("data")) {
				m_pcm.resize(chunk.size);
				stream.read(reinterpret_cast<char *>(m_pcm.data()), m_pcm.size());
				have_data = true;
			} else {
				stream.seekg(chunk.size, std::ios::cur);
			}
		}
	}

	inline const std::vector<uint8_t> & pcm() const {
		return m_pcm;
	}

	inline int samplerate() const {
		return m_samplerate;
	}

	inline ALenum format() const {
		return m_format;
	}

protected:
	int m_samplerate = 0;
	ALenum m_format = 0;
	std::vector<uint8_t> m_pcm;
};

struct sound_sample {

	std::string name;
	SoundSFXID sfx_id;
	openal_buffer buffer;

	sound_sample(const char * _name, SoundSFXID _sfx_id, const wave_file & wav) {
		name = _name;
		sfx_id = _sfx_id;
		const auto & pcm = wav.pcm();
		const auto format = wav.format();
		if (format == AL_FORMAT_MONO_MSADPCM_SOFT) {
			// Needed for heart6a.wav
			std::vector<uint8_t> converted(pcm.size() * 2);
			for (size_t i = 0; i < pcm.size(); ++i) {
				converted[(i * 2) + 0] = (pcm[i] >> 4) * 2;
				converted[(i * 2) + 1] = (pcm[i] & 0x7) * 2;
			}
			alBufferData(buffer.id, AL_FORMAT_MONO8, converted.data(), converted.size(), wav.samplerate());
		} else if (format == AL_FORMAT_STEREO_MSADPCM_SOFT) {
			throw std::runtime_error("Format not implemented");
		} else {
			alBufferData(buffer.id, format, pcm.data(), pcm.size(), wav.samplerate());
		}
		const auto errcode = alGetError();
		if (errcode != AL_NO_ERROR) {
			throw openal_error("Cannot buffer sample data", errcode);
		}
	}
};

#pragma pack(1)
struct SoundBankHead { // sizeof = 18
	uint8_t signature[14];
	uint32_t version;
};
#pragma pack()

#pragma pack(1)
struct SoundBankSample { // sizeof = 32
	/** Name of the sound file the sample comes from. */
	char filename[18];
	/** Offset of the sample data. */
	uint32_t data_offset;
	uint32_t sample_rate;
	/** Size of the sample file. */
	uint32_t data_size;
	SoundSFXID sfxid;
	uint8_t format_flags;
};
#pragma pack()

#pragma pack(1)
struct SoundBankEntry { // sizeof = 16
	uint32_t first_sample_offset;
	uint32_t first_data_offset;
	uint32_t total_samples_size;
	uint32_t entries_count;
};
#pragma pack()

std::vector<sound_sample> load_sound_bank(const char * filename) {
	const int directory_index = 2; // a5 was always 1622
	std::ifstream stream(filename, std::ios::in | std::ios::binary);
	if (!stream.is_open()) {
		throw std::runtime_error("Cannot open sound bank file");
	}
	stream.seekg(-4, std::ios::end);
	uint32_t head_offset;
	stream.read(reinterpret_cast<char *>(&head_offset), sizeof(head_offset));
	stream.seekg(head_offset, std::ios::beg);
	SoundBankHead bhead;
	stream.read(reinterpret_cast<char *>(&bhead), sizeof(bhead));
	SoundBankEntry bentries[9];
	stream.read(reinterpret_cast<char *>(bentries), sizeof(bentries));
	const auto & directory = bentries[directory_index];
	if (directory.first_sample_offset == 0) {
		throw std::runtime_error("Invalid sample offset");
	} else if (directory.total_samples_size < sizeof(SoundBankSample)) {
		throw std::runtime_error("Invalid samples size");
	}
	const int sample_count = directory.total_samples_size / sizeof(SoundBankSample);
	stream.seekg(directory.first_sample_offset, std::ios::beg);
	std::vector<sound_sample> buffers;
	buffers.reserve(sample_count);
	SoundBankSample sample;
	for (int i = 0; i < sample_count; ++i) {
		stream.seekg(directory.first_sample_offset + (sizeof(sample) * i), std::ios::beg);
		stream.read(reinterpret_cast<char *>(&sample), sizeof(sample));
		stream.seekg(directory.first_data_offset + sample.data_offset, std::ios::beg);
		buffers.emplace_back(sample.filename, sample.sfxid, wave_file(stream));
	}
	JUSTLOG("Loaded %d sound samples from %s", buffers.size(), filename);
	return buffers;
}

std::vector<openal_source> g_sources;
std::array<std::vector<sound_sample>, 2> g_banks;

void load_sound_banks() {
	char snd_fname[2048];
	prepare_file_path_buf(snd_fname, sizeof(snd_fname), FGrp_LrgSound, "sound.dat");
	// language-specific speech file
	char * spc_fname = prepare_file_fmtpath(FGrp_LrgSound, "speech_%s.dat", get_language_lwrstr(install_info.lang_id));
	// default speech file
	if (!LbFileExists(spc_fname)) {
		spc_fname = prepare_file_path(FGrp_LrgSound, "speech.dat");
	}
	// speech file for english
	if (!LbFileExists(spc_fname)) {
		spc_fname = prepare_file_fmtpath(FGrp_LrgSound, "speech_%s.dat", get_language_lwrstr(1));
	}
	g_banks[0] = load_sound_bank(snd_fname);
	g_banks[1] = load_sound_bank(spc_fname);
}

struct queued_sample {
	std::string fname;
	SoundVolume volume;
};

struct SampleBuffer {

	uint8_t * data = nullptr;
	size_t offset = 0;
	size_t size = 0;

	inline SampleBuffer(uint8_t * _data, size_t _size)
	: data(_data), size(_size) {}

	inline ~SampleBuffer() noexcept {
		av_freep(&data);
	}

	SampleBuffer(const SampleBuffer &) = delete;
	SampleBuffer & operator=(const SampleBuffer &) = delete;

	inline SampleBuffer(SampleBuffer && other) noexcept
	: data(std::exchange(other.data, nullptr))
	, offset(std::exchange(other.offset, 0))
	, size(std::exchange(other.size, 0)) {}

	inline SampleBuffer & operator=(SampleBuffer && other) noexcept {
		data = std::exchange(other.data, nullptr);
		offset = std::exchange(other.offset, 0);
		size = std::exchange(other.size, 0);
		return *this;
	}
};

AVSampleFormat SDL_to_FFmpeg_format(SDL_AudioFormat format) {
	switch (format) {
		case AUDIO_S8: return AV_SAMPLE_FMT_U8;
		case AUDIO_S16SYS: return AV_SAMPLE_FMT_S16;
		case AUDIO_S32SYS: return AV_SAMPLE_FMT_S32;
		case AUDIO_F32SYS: return AV_SAMPLE_FMT_FLT;
		default: return AV_SAMPLE_FMT_NONE;
	}
}

SDL_AudioFormat FFmpeg_to_SDL_format(AVSampleFormat format) {
	switch (format) {
		case AV_SAMPLE_FMT_U8: return AUDIO_S8;
		case AV_SAMPLE_FMT_S16: return AUDIO_S16SYS;
		case AV_SAMPLE_FMT_S32: return AUDIO_S32SYS;
		case AV_SAMPLE_FMT_FLT: return AUDIO_F32SYS;
		default: return 0;
	}
}

AVChannelLayout channo_to_FFmpeg_layout(int num_channels) {
	switch (num_channels) {
		case 1: return AV_CHANNEL_LAYOUT_MONO;
		case 2: return AV_CHANNEL_LAYOUT_STEREO;
		case 3: return AV_CHANNEL_LAYOUT_SURROUND;
		case 4: return AV_CHANNEL_LAYOUT_QUAD;
		case 5: return AV_CHANNEL_LAYOUT_4POINT1;
		case 6: return AV_CHANNEL_LAYOUT_5POINT1;
		case 7: return AV_CHANNEL_LAYOUT_6POINT1;
		case 8: return AV_CHANNEL_LAYOUT_7POINT1;
		default: return {};
	}
}

auto SDL_AudioFormat_to_string(SDL_AudioFormat format) {

	switch (format) {
		case AUDIO_U8: return "AUDIO_U8";
		case AUDIO_S8: return "AUDIO_S8";
		case AUDIO_U16LSB: return (AUDIO_U16LSB == AUDIO_U16SYS) ? "AUDIO_U16SYS" : "AUDIO_U16LSB";
		case AUDIO_U16MSB: return (AUDIO_U16MSB == AUDIO_U16SYS) ? "AUDIO_U16SYS" : "AUDIO_U16MSB";
		case AUDIO_S16LSB: return (AUDIO_S16LSB == AUDIO_S16SYS) ? "AUDIO_S16SYS" : "AUDIO_S16LSB";
		case AUDIO_S16MSB: return (AUDIO_S16MSB == AUDIO_S16SYS) ? "AUDIO_S16SYS" : "AUDIO_S16MSB";
		case AUDIO_S32LSB: return (AUDIO_S32LSB == AUDIO_S32SYS) ? "AUDIO_S32SYS" : "AUDIO_S32LSB";
		case AUDIO_S32MSB: return (AUDIO_S32MSB == AUDIO_S32SYS) ? "AUDIO_S32SYS" : "AUDIO_S32MSB";
		case AUDIO_F32LSB: return (AUDIO_F32LSB == AUDIO_F32SYS) ? "AUDIO_F32SYS" : "AUDIO_F32LSB";
		case AUDIO_F32MSB: return (AUDIO_F32MSB == AUDIO_F32SYS) ? "AUDIO_F32SYS" : "AUDIO_F32MSB";
		default: return "unknown";
	}
}

class Resampler {
protected:
	SwrContext * m_resampler = nullptr;

public:

	inline Resampler() = default;

	Resampler(
		const AVChannelLayout & input_layout, AVSampleFormat input_format, int input_sample_rate,
		const AVChannelLayout & output_layout, AVSampleFormat output_format, int output_sample_rate
	) {
		const auto result = swr_alloc_set_opts2(
			&m_resampler,
			&output_layout, output_format, output_sample_rate,
			&input_layout, input_format, input_sample_rate,
			0, nullptr
		);
		if (result != 0) {
			throw std::runtime_error("Cannot allocate resampler");
		}
		if (swr_init(m_resampler) != 0) {
			swr_free(&m_resampler);
			throw std::runtime_error("Cannot initialize resampler");
		}
		JUSTLOG("Created resampler for %d -> %d channels, %d Hz -> %d Hz, %s -> %s",
			input_layout.nb_channels,
			output_layout.nb_channels,
			input_sample_rate,
			output_sample_rate,
			SDL_AudioFormat_to_string(FFmpeg_to_SDL_format(input_format)),
			SDL_AudioFormat_to_string(FFmpeg_to_SDL_format(output_format))
		);
	}

	inline ~Resampler() noexcept {
		swr_free(&m_resampler);
	}

	Resampler(const Resampler &) = delete;
	Resampler & operator=(const Resampler &) = delete;

	inline Resampler(Resampler && other) noexcept
	: m_resampler(std::exchange(other.m_resampler, nullptr)) {}

	inline Resampler & operator=(Resampler && other) noexcept {
		m_resampler = std::exchange(other.m_resampler, nullptr);
		return *this;
	}

	auto expected_samples(int input_samples) const {
		return swr_get_out_samples(m_resampler, input_samples);
	}

	auto expected_samples(const AVFrame * frame) const {
		return expected_samples((frame) ? frame->nb_samples : 0);
	}

	auto convert(void * dst, int dst_samples, const void * src, int src_samples) const {
		const auto out = static_cast<uint8_t *>(dst);
		const auto in = static_cast<const uint8_t *>(src);
		const auto result = swr_convert(
			m_resampler,
			&out, dst_samples,
			&in, src_samples
		);
		if (result < 0) {
			throw std::runtime_error("Cannot convert samples");
		}
		return result;
	}

	auto convert(void * buffer, const AVFrame * frame) const {
		const auto num_samples = expected_samples(frame);
		return convert(
			buffer, num_samples,
			(frame) ? frame->data[0] : nullptr,
			(frame) ? frame->nb_samples : 0
		);
	}
};

auto make_sample_buffer(int num_channels, int num_samples, AVSampleFormat format) {
	uint8_t * buffer = nullptr;
	const auto result = av_samples_alloc(
		&buffer,
		nullptr,
		num_channels,
		num_samples,
		format,
		1
	);
	if (result < 0) {
		throw std::runtime_error("Cannot allocate sample buffer");
	}
	return SampleBuffer(buffer, num_channels * num_samples * av_get_bytes_per_sample(format));
}

} // local

struct FFmpegStream {

	AudioType type = AudioType::AT_INVALID;
	AVChannelLayout input_layout = {};
	AVSampleFormat input_format = AV_SAMPLE_FMT_NONE;
	int input_sample_rate = 0;
	std::list<SampleBuffer> buffers;
	Resampler resampler;
	bool stopped = false;
	bool closed = false;

	FFmpegStream(
		AudioType _type,
		const AVChannelLayout * layout,
		AVSampleFormat format,
		int sample_rate
	) {
		type = _type;
		if (av_channel_layout_copy(&input_layout, layout) != 0) {
			throw std::runtime_error("Cannot copy channel layout");
		}
		input_format = format;
		input_sample_rate = sample_rate;
		const auto output_layout = channo_to_FFmpeg_layout(g_device_num_channels);
		const auto output_format = SDL_to_FFmpeg_format(g_device_format);
		const auto output_sample_rate = g_device_sample_rate;
		resampler = Resampler(
			input_layout, input_format, input_sample_rate,
			output_layout, output_format, output_sample_rate
		);
	}

	inline ~FFmpegStream() = default;

	FFmpegStream(const FFmpegStream &) = delete;
	FFmpegStream & operator=(const FFmpegStream &) = delete;

	FFmpegStream(FFmpegStream && other)
	: type(std::exchange(other.type, AudioType::AT_INVALID))
	, input_layout(std::exchange(other.input_layout, {}))
	, input_format(std::exchange(other.input_format, AV_SAMPLE_FMT_NONE))
	, input_sample_rate(std::exchange(other.input_sample_rate, 0))
	, buffers(std::move(other.buffers))
	, resampler(std::move(other.resampler))
	, stopped(std::exchange(other.stopped, false))
	, closed(std::exchange(other.closed, false)) {}

	FFmpegStream & operator=(FFmpegStream && other) {
		type = std::exchange(other.type, AudioType::AT_INVALID);
		input_layout = std::exchange(other.input_layout, {});
		input_format = std::exchange(other.input_format, AV_SAMPLE_FMT_NONE);
		input_sample_rate = std::exchange(other.input_sample_rate, 0);
		buffers = std::move(other.buffers);
		resampler = std::move(other.resampler);
		stopped = std::exchange(other.stopped, false);
		closed = std::exchange(other.closed, false);
		return *this;
	}

	void append(const AVFrame * frame) {
		// Process frame samples
		JUSTLOG("Resampling %d samples", (frame) ? frame->nb_samples : 0);
		const auto output_format = SDL_to_FFmpeg_format(g_device_format);
		const auto output_channels = g_device_num_channels;
		const auto expected_samples = resampler.expected_samples(frame);
		JUSTLOG("Expecting %d samples", expected_samples);
		const auto output_bps = av_get_bytes_per_sample(output_format);
		if (expected_samples > 0) {
			auto buffer = make_sample_buffer(output_channels, expected_samples, output_format);
			const auto converted_samples = resampler.convert(buffer.data, frame);
			JUSTLOG("Resampled %d samples", converted_samples);
			if (converted_samples > 0) {
				buffer.size = output_channels * converted_samples * output_bps;
				buffers.emplace_back(std::move(buffer));
			}
		}
	}

	void close() {
		// Process remaining samples and mark stream as closed
		JUSTLOG("Resampling remaining samples");
		const auto output_format = SDL_to_FFmpeg_format(g_device_format);
		const auto output_channels = g_device_num_channels;
		const auto expected_samples = resampler.expected_samples(nullptr);
		JUSTLOG("Expecting %d samples", expected_samples);
		const auto output_bps = av_get_bytes_per_sample(output_format);
		if (expected_samples > 0) {
			auto buffer = make_sample_buffer(output_channels, expected_samples, output_format);
			const auto converted_samples = resampler.convert(buffer.data, nullptr);
			JUSTLOG("Resampled %d samples", converted_samples);
			if (converted_samples > 0) {
				buffer.size = output_channels * converted_samples * output_bps;
				buffers.emplace_back(std::move(buffer));
			}
		}
		closed = true;
	}

	// TODO: test
	void reconfigure(SDL_AudioFormat format, Uint8 new_channels, int new_sample_rate) {
		// Convert existing buffers to new format
		JUSTLOG("Reconfiguring...");
		const auto old_channels = g_device_num_channels;
		const auto old_layout = channo_to_FFmpeg_layout(old_channels);
		const auto old_format = SDL_to_FFmpeg_format(g_device_format);
		const auto old_sample_rate = g_device_sample_rate;
		const auto old_bps = av_get_bytes_per_sample(old_format);
		const auto new_layout = channo_to_FFmpeg_layout(new_channels);
		const auto new_format = SDL_to_FFmpeg_format(format);
		const auto new_bps = av_get_bytes_per_sample(new_format);

		Resampler buffer_resampler(
			old_layout, old_format, old_sample_rate,
			new_layout, new_format, new_sample_rate
		);

		std::list<SampleBuffer> new_buffers;

		for (const auto & buffer : buffers) {
			const auto samples_in_buffer = (buffer.size - buffer.offset) / (old_channels * old_bps);
			if (samples_in_buffer > 0) {
				const auto expected_samples = buffer_resampler.expected_samples(samples_in_buffer);
				auto new_buffer = make_sample_buffer(new_channels, expected_samples, new_format);
				const auto converted_samples = buffer_resampler.convert(
					new_buffer.data, expected_samples,
					&buffer.data[buffer.offset], samples_in_buffer
				);
				if (converted_samples > 0) {
					new_buffer.size = new_channels * converted_samples * new_bps;
					new_buffers.emplace_back(std::move(new_buffer));
				}
			}
		}
		const auto remaining_samples = buffer_resampler.expected_samples(0);
		if (remaining_samples > 0) {
			auto new_buffer = make_sample_buffer(new_channels, remaining_samples, new_format);
			const auto converted_samples = buffer_resampler.convert(
				new_buffer.data, remaining_samples,
				nullptr, 0
			);
			new_buffer.size = new_channels * converted_samples * new_bps;
			new_buffers.emplace_back(std::move(new_buffer));
		}

		buffers = std::move(new_buffers);

		// Create new resampler for future samples
		resampler = Resampler(
			input_layout, input_format, input_sample_rate,
			new_layout, new_format, new_sample_rate
		);
	}
};

namespace {

std::list<FFmpegStream> g_ffmpeg_streams;

template<typename sample_type>
inline void mix_samples(void * dst, const void * src, size_t size) {
	const auto samples = size / sizeof(sample_type);
	JUSTLOG("Mixing %d samples", samples);
	for (size_t i = 0; i < samples; ++i) {
		static_cast<sample_type *>(dst)[i] += static_cast<const sample_type *>(src)[i];
	}
}

bool device_format_changed() {
	// SDL3 provides SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED but since we don't have it, check every time
	SDL_AudioSpec spec;
	if (SDL_GetDefaultAudioInfo(nullptr, &spec, 0) != 0) {
		throw std::runtime_error("Cannot query default audio device");
	}
	return spec.channels != g_device_num_channels ||
		spec.format != g_device_format ||
		spec.freq != g_device_sample_rate;
}

void SDLCALL sdl_audio_callback(void *, Uint8 * sum, int len);

void reconfigure_sdl_device() {
	std::lock_guard<std::mutex> guard(g_sdl_lock);
	try {
		JUSTLOG("Reconfiguring SDL device...");
		if (g_sdl_device > 0) {
			SDL_CloseAudioDevice(g_sdl_device);
			g_sdl_device = 0;
		}
		SDL_AudioSpec desired;
		if (SDL_GetDefaultAudioInfo(nullptr, &desired, 0) != 0) {
			throw std::runtime_error("Cannot query default audio device");
		}
		desired.silence = 0;
		desired.samples = 0;
		desired.padding = 0;
		desired.size = 0;
		desired.callback = sdl_audio_callback;
		desired.userdata = nullptr;
		SDL_AudioSpec obtained;
		g_sdl_device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
		if (g_sdl_device <= 0) {
			throw std::runtime_error("Cannot open audio device");
		}
		JUSTLOG("Channels %u -> %u", g_device_num_channels, obtained.channels);
		JUSTLOG("Format %s -> %s", SDL_AudioFormat_to_string(g_device_format), SDL_AudioFormat_to_string(obtained.format));
		JUSTLOG("Sample Rate %d Hz -> %d Hz", g_device_sample_rate, obtained.freq);

		for (auto & stream : g_ffmpeg_streams) {
			try {
				stream.reconfigure(obtained.format, obtained.channels, obtained.freq);
			} catch (const std::exception & e) {
				ERRORLOG("%s", e.what());
				stream.stopped = true;
			}
		}
		g_device_num_channels = obtained.channels;
		g_device_sample_rate = obtained.freq;
		g_device_format = obtained.format;
		SDL_PauseAudioDevice(g_sdl_device, 0);
	} catch (const std::exception & e) {
		ERRORLOG("%s", e.what());
	}
}

void SDLCALL sdl_audio_callback(void *, Uint8 * sum, int len)
{
	std::lock_guard<std::mutex> guard(g_sdl_lock);
	JUSTLOG("sdl_audio_callback, len = %d", len);
	memset(sum, 0, len); // required on Windows apparently
	if (device_format_changed()) {
		JUSTLOG("Device format changed");
		// We can't open or close devices within this callback, stop playback and defer work to a temporary thread.
		SDL_PauseAudioDevice(g_sdl_device, 1);
		std::thread(reconfigure_sdl_device).detach();
		return;
	}
	JUSTLOG("streams %d", g_ffmpeg_streams.size());
	for (auto & stream : g_ffmpeg_streams) {
		size_t offset = 0;
		JUSTLOG("buffers %d, stopped %s, closed %s", stream.buffers.size(), stream.stopped ? "yes" : "no", stream.closed ? "yes" : "no");
		for (size_t remaining = len; remaining > 0;) {
			if (stream.buffers.empty() || stream.stopped) {
				break;
			}
			auto & buffer = stream.buffers.front();
			const size_t available = buffer.size - buffer.offset;
			const size_t consumed = std::min(available, remaining);
			switch (g_device_format) {
				case AUDIO_S8: {
					mix_samples<int8_t>(&sum[offset], &buffer.data[buffer.offset], consumed);
					offset += consumed;
					break;
				}
				case AUDIO_S16SYS: {
					mix_samples<int16_t>(&sum[offset], &buffer.data[buffer.offset], consumed);
					offset += consumed;
					break;
				}
				case AUDIO_S32SYS: {
					mix_samples<int32_t>(&sum[offset], &buffer.data[buffer.offset], consumed);
					offset += consumed;
					break;
				}
				case AUDIO_F32SYS: {
					mix_samples<float>(&sum[offset], &buffer.data[buffer.offset], consumed);
					offset += consumed;
					break;
				}
				default: {
					// don't care how to mix this, skip
					offset += consumed;
					break;
				}
			}
			buffer.offset += consumed;
			remaining -= consumed;
			if (buffer.offset >= buffer.size) {
				stream.buffers.pop_front();
			}
		}
	}
	g_ffmpeg_streams.erase(std::remove_if(
		g_ffmpeg_streams.begin(), g_ffmpeg_streams.end(),
		[](const auto & stream) -> auto {
			return (stream.buffers.empty() || stream.stopped) && stream.closed;
		}),
		g_ffmpeg_streams.end()
	);
}

} // local

extern "C" void FreeAudio() {
	g_sources.clear();
	g_banks[0].clear();
	g_banks[1].clear();
	g_openal_context = nullptr;
	g_openal_device = nullptr;
	SDL_CloseAudioDevice(g_sdl_device);
	g_sdl_device = 0;
	g_device_num_channels = 0;
	g_device_format = 0;
	g_device_sample_rate = 0;
}

extern "C" void set_master_volume(SoundVolume volume) {
	g_master_volume = volume;
}

extern "C" void set_music_volume(SoundVolume value) {
	g_music_volume = value;
	SetRedbookVolume(value);
	// TODO: adjust music volume
}

extern "C" TbBool play_music(const char * fname) {
	game.music_track = -1;
	snprintf(game.music_fname, sizeof(game.music_fname), "%s", fname);
	// TODO: stream music from file
	JUSTLOG("Playing %s", game.music_fname);
	return true;
}

extern "C" TbBool play_music_track(int track) {
	game.music_track = track;
	memset(game.music_fname, 0, sizeof(game.music_fname));
	if (game.music_track == 0) {
		stop_music();
		return true;
	} else if (features_enabled & Ft_NoCdMusic) {
		return play_music(prepare_file_fmtpath(FGrp_Music, "keeper%02d.ogg", track));
	} else {
		if (PlayRedbookTrack(track)) {
			JUSTLOG("Playing track %d", game.music_track);
			return true;
		} else {
			WARNLOG("Cannot play track %d", game.music_track);
			return false;
		}
	}
}

extern "C" void pause_music() {
	JUSTLOG("Pausing music");
	if (features_enabled & Ft_NoCdMusic) {
		// TODO: pause music
	} else {
		PauseRedbookTrack();
	}
}

extern "C" void resume_music() {
	JUSTLOG("Resuming music");
	if (features_enabled & Ft_NoCdMusic) {
		// TODO: resume music
	} else {
		ResumeRedbookTrack();
	}
}

extern "C" void stop_music() {
	JUSTLOG("Stopping music");
	game.music_track = 0;
	memset(game.music_fname, 0, sizeof(game.music_fname));
	if (features_enabled & Ft_NoCdMusic) {
		// TODO: fade out and stop music
	} else {
		StopRedbookTrack();
	}
}

extern "C" TbBool GetSoundInstalled() {
	return g_openal_device && g_openal_context;
}

// This function gets called every tick
extern "C" void MonitorStreamedSoundTrack() {
	for (auto & source : g_sources) {
		try {
			if (source.emit_id > 0 && !source.is_playing()) {
				source.emit_id = 0;
				source.smptbl_id = 0;
				source.bank_id = 0;
			}
		} catch (const std::exception & e) {
			ERRORLOG("%s", e.what());
		}
	}
	g_tick_samples.clear();
}

extern "C" void * GetSoundDriver() {
	// This just needs to return any non-null pointer. FMV library appears to have standalone audio
	static int dummy = 0;
	return &dummy;
}

extern "C" void StopAllSamples() {
	for (auto & source : g_sources) {
		try {
			source.stop();
		} catch (const std::exception & e) {
			ERRORLOG("%s", e.what());
		}
	}
}

extern "C" TbBool InitAudio(const SoundSettings * settings) {
	try {
		if (game.flags_font & FFlg_AlexCheat) {
			TbDate date;
			LbDate(&date);
			g_bb_king_mode |= ((date.Day == 1) && (date.Month == 2));
		}
		if (SoundDisabled) {
			WARNLOG("Sound is disabled, skipping initialization");
			return false;
		}
		// Set up SDL2 first
		if (SDL_Init(SDL_INIT_AUDIO) < 0) {
			throw std::runtime_error("Cannot initialise SDL audio subsystem");
		}
		SDL_AudioSpec desired;
		if (SDL_GetDefaultAudioInfo(nullptr, &desired, 0) != 0) {
			throw std::runtime_error("Cannot query default audio device");
		}
		JUSTLOG("Channels %u", desired.channels);
		JUSTLOG("Format %s", SDL_AudioFormat_to_string(desired.format));
		JUSTLOG("Sample Rate %d Hz", desired.freq);
		desired.silence = 0;
		desired.samples = 0;
		desired.padding = 0;
		desired.size = 0;
		desired.callback = sdl_audio_callback;
		desired.userdata = nullptr;
		SDL_AudioSpec obtained;
		g_sdl_device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
		if (g_sdl_device <= 0) {
			throw std::runtime_error("Cannot open audio device");
		}
		JUSTLOG("Channels %u", obtained.channels);
		JUSTLOG("Format %s", SDL_AudioFormat_to_string(obtained.format));
		JUSTLOG("Sample Rate %d Hz", obtained.freq);
		g_device_num_channels = obtained.channels;
		g_device_sample_rate = obtained.freq;
		g_device_format = obtained.format;
		SDL_PauseAudioDevice(g_sdl_device, 0);
		// Now set up OpenAL
		ALCdevice_ptr device(alcOpenDevice(nullptr));
		if (!device) {
			throw openal_error("Cannot open default audio device");
		}
		ALCcontext_ptr context(alcCreateContext(device.get(), nullptr));
		if (!context) {
			throw openal_error("Cannot create context");
		} else if (!alcMakeContextCurrent(context.get())) {
			throw openal_error("Cannot make context current");
		}
		g_sources.resize(settings->max_number_of_samples);
		for (size_t i = 0; i < g_sources.size(); ++i) {
			g_sources[i].mss_id = i + 1;
		}
		load_sound_banks();
		g_openal_device = std::move(device);
		g_openal_context = std::move(context);
		return true;
	} catch (const std::exception & e) {
		ERRORLOG("%s", e.what());
	}
	SoundDisabled = true;
	return false;
}

extern "C" TbBool IsSamplePlaying(SoundMilesID mss_id) {
	try {
		for (const auto & source : g_sources) {
			if (source.mss_id == mss_id) {
				return source.is_playing();
			}
		}
	} catch (const std::exception & e) {
		ERRORLOG("%s", e.what());
	}
	return false;
}

extern "C" SoundVolume GetCurrentSoundMasterVolume() {
	return g_master_volume;
}

extern "C" void SetSampleVolume(SoundEmitterID emit_id, SoundSmplTblID smptbl_id, SoundVolume volume) {
	for (auto & source : g_sources) {
		if (source.emit_id == emit_id && source.smptbl_id == smptbl_id) {
			try {
				source.gain(volume);
			} catch (const std::exception & e) {
				ERRORLOG("%s", e.what());
			}
		}
	}
}

extern "C" void SetSamplePan(SoundEmitterID emit_id, SoundSmplTblID smptbl_id, SoundPan pan) {
	for (auto & source : g_sources) {
		if (source.emit_id == emit_id && source.smptbl_id == smptbl_id) {
			try {
				source.pan(pan);
			} catch (const std::exception & e) {
				ERRORLOG("%s", e.what());
			}
		}
	}
}

extern "C" void SetSamplePitch(SoundEmitterID emit_id, SoundSmplTblID smptbl_id, SoundPitch pitch) {
	for (auto & source : g_sources) {
		if (source.emit_id == emit_id && source.smptbl_id == smptbl_id) {
			try {
				if (source.flags & bb_king_mode) {
					return; // ben enjoyed dofi's stream so much I made random pitch an easter egg
				} else {
					source.pitch(pitch);
				}
			} catch (const std::exception & e) {
				ERRORLOG("%s", e.what());
			}
		}
	}
}

extern "C" SoundMilesID play_sample(
	SoundEmitterID emit_id,
	SoundSmplTblID smptbl_id,
	SoundVolume volume,
	SoundPan pan,
	SoundPitch pitch,
	char repeats, // possible values: -1, 0
	unsigned char ctype, // possible values: 2, 3
	SoundBankID bank_id
) {
	if (emit_id <= 0) {
		ERRORLOG("Can't play sample %d from bank %u, invalid emitter ID", smptbl_id, bank_id);
		return 0;
	} else if (bank_id > g_banks.size()) {
		ERRORLOG("Can't play sample %d from bank %u, invalid bank ID", smptbl_id, bank_id);
		return 0;
	} else if (smptbl_id == 0) {
		return 0; // silently ignore
	} else if (smptbl_id <= 0 || smptbl_id >= g_banks[bank_id].size()) {
		ERRORLOG("Can't play sample %d from bank %u, invalid sample ID", smptbl_id, bank_id);
		return 0;
	}
	// (ab)use the fact that bank_id and smptbl_id are currently 8- and 16-bits wide respectively.
	const uint32_t tick_sample_key = (uint32_t(bank_id) << 16) | (smptbl_id & 0xffff);
	if (g_tick_samples.count(tick_sample_key) > 0) {
		return 0; // don't play the same sample multiple times on the same tick
	}
	try {
		g_tick_samples.emplace(tick_sample_key);
		for (auto & source : g_sources) {
			if (source.emit_id == 0) {
				source.gain(volume);
				source.pan(pan);
				source.repeat(repeats == -1);
				if (g_bb_king_mode) {
					// ben enjoyed dofi's stream so much I made random pitch an easter egg
					if (UNSYNC_RANDOM(10) > 7) { // ~30% of the time
						source.flags |= bb_king_mode;
						source.pitch((NORMAL_PITCH / 2) + UNSYNC_RANDOM(NORMAL_PITCH));
					} else {
						source.flags &= ~bb_king_mode;
						source.pitch(pitch);
					}
				} else {
					source.pitch(pitch);
				}
				source.play(g_banks[bank_id][smptbl_id].buffer);
				source.emit_id = emit_id;
				source.smptbl_id = smptbl_id;
				source.bank_id = bank_id;
				return source.mss_id;
			}
		}
        if (game.frame_skip < 2)
        {
            ERRORLOG("Can't play sample %d from bank %u, too many samples playing at once", smptbl_id, bank_id);
        }
		return 0;
	} catch (const std::exception & e) {
		ERRORLOG("%s", e.what());
	}
	return 0;
}

extern "C" void stop_sample(SoundEmitterID emit_id, SoundSmplTblID smptbl_id, SoundBankID bank_id) {
	for (auto & source : g_sources) {
		if (emit_id == source.emit_id && smptbl_id == source.smptbl_id && bank_id == source.bank_id) {
			try {
				source.stop();
				source.emit_id = 0;
				source.smptbl_id = 0;
				source.bank_id = 0;
			} catch (const std::exception & e) {
				ERRORLOG("%s", e.what());
			}
		}
	}
}

extern "C" SoundSFXID get_sample_sfxid(SoundSmplTblID smptbl_id, SoundBankID bank_id) {
	if (bank_id > 1) {
		return 0;
	} else if (smptbl_id < 0 || smptbl_id >= g_banks[bank_id].size()) {
		return 0;
	}
	return g_banks[bank_id][smptbl_id].sfx_id;
}

extern "C" TbBool stream_sound_effect(
	const char * fname,
	SoundVolume volume,
	SoundPan pan,
	SoundPitch pitch
) {
	if (SoundDisabled || fname == nullptr || strlen(fname) == 0) {
		return false;
	}
	// TODO: start playing sound sample from file
	return true;
}

extern "C" TbBool stream_mentor_speech(const char * fname)
{
	if (SoundDisabled || fname == nullptr || strlen(fname) == 0) {
		return false;
	}
	// TODO: start playing sound sample from file
	return true;
}

extern "C" void stop_streamed_samples()
{
	// TODO: stop playing sound sample from file
}

extern "C" void set_mentor_volume(SoundVolume volume) {
	g_mentor_volume = volume;
}

extern "C" void set_effects_volume(SoundVolume volume) {
	g_effects_volume = volume;
}

extern "C" void toggle_bbking_mode() {
	g_bb_king_mode = !g_bb_king_mode;
}

extern "C" FFmpegStream * ffmpeg_stream_open(
	AudioType type,
	const AVChannelLayout * layout,
	AVSampleFormat format,
	int sample_rate
) {
	std::lock_guard<std::mutex> guard(g_sdl_lock);
	try {
		g_ffmpeg_streams.emplace_back(type, layout, format, sample_rate);
		const auto ptr = &g_ffmpeg_streams.back();
		return ptr;
	} catch (const std::exception & e) {
		ERRORLOG("%s", e.what());
		return nullptr;
	}
}

extern "C" void ffmpeg_stream_append(FFmpegStream * stream, const AVFrame * frame) {
	if (stream) {
		std::lock_guard<std::mutex> guard(g_sdl_lock);
		try {
			stream->append(frame);
		} catch (const std::exception & e) {
			ERRORLOG("%s", e.what());
		}
	}
}

extern "C" void ffmpeg_stream_stop(FFmpegStream * stream) {
	if (stream) {
		std::lock_guard<std::mutex> guard(g_sdl_lock);
		try {
			stream->stopped = true;
		} catch (const std::exception & e) {
			ERRORLOG("%s", e.what());
		}
	}
}

extern "C" void ffmpeg_stream_close(FFmpegStream * stream) {
	if (stream) {
		std::lock_guard<std::mutex> guard(g_sdl_lock);
		try {
			stream->close();
		} catch (const std::exception & e) {
			ERRORLOG("%s", e.what());
		}
	}
}
