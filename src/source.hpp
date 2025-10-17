/*
    Copyright (C) 2021 Devin Davila

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once
#include <mutex>
#include <obs-module.h>
#include <util/bmem.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <fftw3.h>
#include <string.h>
#include <assert.h>
#include "module.hpp"
#include "aligned_buffer.hpp"
#include "filter.hpp"

using AVXBufR = AlignedBuffer<float>;
using AVXBufC = AlignedBuffer<fftwf_complex>;

extern "C" {
struct circlebuf {
	void *data;
	size_t size;

	size_t start_pos;
	size_t end_pos;
	size_t capacity;
};

static inline void circlebuf_init(struct circlebuf *cb)
{
	memset(cb, 0, sizeof(struct circlebuf));
}

static inline void circlebuf_free(struct circlebuf *cb)
{
	bfree(cb->data);
	memset(cb, 0, sizeof(struct circlebuf));
}

static inline void circlebuf_reorder_data(struct circlebuf *cb, size_t new_capacity)
{
	size_t difference;
	uint8_t *data;

	if (!cb->size || !cb->start_pos || cb->end_pos > cb->start_pos)
		return;

	difference = new_capacity - cb->capacity;
	data = (uint8_t *)cb->data + cb->start_pos;
	memmove(data + difference, data, cb->capacity - cb->start_pos);
	cb->start_pos += difference;
}

static inline void circlebuf_ensure_capacity(struct circlebuf *cb)
{
	size_t new_capacity;
	if (cb->size <= cb->capacity)
		return;

	new_capacity = cb->capacity * 2;
	if (cb->size > new_capacity)
		new_capacity = cb->size;

	cb->data = brealloc(cb->data, new_capacity);
	circlebuf_reorder_data(cb, new_capacity);
	cb->capacity = new_capacity;
}

static inline void circlebuf_reserve(struct circlebuf *cb, size_t capacity)
{
	if (capacity <= cb->capacity)
		return;

	cb->data = brealloc(cb->data, capacity);
	circlebuf_reorder_data(cb, capacity);
	cb->capacity = capacity;
}

static inline void circlebuf_upsize(struct circlebuf *cb, size_t size)
{
	size_t add_size = size - cb->size;
	size_t new_end_pos = cb->end_pos + add_size;

	if (size <= cb->size)
		return;

	cb->size = size;
	circlebuf_ensure_capacity(cb);

	if (new_end_pos > cb->capacity) {
		size_t back_size = cb->capacity - cb->end_pos;
		size_t loop_size = add_size - back_size;

		if (back_size)
			memset((uint8_t *)cb->data + cb->end_pos, 0, back_size);

		memset(cb->data, 0, loop_size);
		new_end_pos -= cb->capacity;
	} else {
		memset((uint8_t *)cb->data + cb->end_pos, 0, add_size);
	}

	cb->end_pos = new_end_pos;
}

/** Overwrites data at a specific point in the buffer (relative).  */
static inline void circlebuf_place(struct circlebuf *cb, size_t position, const void *data, size_t size)
{
	size_t end_point = position + size;
	size_t data_end_pos;

	if (end_point > cb->size)
		circlebuf_upsize(cb, end_point);

	position += cb->start_pos;
	if (position >= cb->capacity)
		position -= cb->capacity;

	data_end_pos = position + size;
	if (data_end_pos > cb->capacity) {
		size_t back_size = data_end_pos - cb->capacity;
		size_t loop_size = size - back_size;

		memcpy((uint8_t *)cb->data + position, data, loop_size);
		memcpy(cb->data, (uint8_t *)data + loop_size, back_size);
	} else {
		memcpy((uint8_t *)cb->data + position, data, size);
	}
}

static inline void circlebuf_push_back(struct circlebuf *cb, const void *data, size_t size)
{
	size_t new_end_pos = cb->end_pos + size;

	cb->size += size;
	circlebuf_ensure_capacity(cb);

	if (new_end_pos > cb->capacity) {
		size_t back_size = cb->capacity - cb->end_pos;
		size_t loop_size = size - back_size;

		if (back_size)
			memcpy((uint8_t *)cb->data + cb->end_pos, data, back_size);
		memcpy(cb->data, (uint8_t *)data + back_size, loop_size);

		new_end_pos -= cb->capacity;
	} else {
		memcpy((uint8_t *)cb->data + cb->end_pos, data, size);
	}

	cb->end_pos = new_end_pos;
}

static inline void circlebuf_push_front(struct circlebuf *cb, const void *data, size_t size)
{
	cb->size += size;
	circlebuf_ensure_capacity(cb);

	if (cb->size == size) {
		cb->start_pos = 0;
		cb->end_pos = size;
		memcpy((uint8_t *)cb->data, data, size);

	} else if (cb->start_pos < size) {
		size_t back_size = size - cb->start_pos;

		if (cb->start_pos)
			memcpy(cb->data, (uint8_t *)data + back_size, cb->start_pos);

		cb->start_pos = cb->capacity - back_size;
		memcpy((uint8_t *)cb->data + cb->start_pos, data, back_size);
	} else {
		cb->start_pos -= size;
		memcpy((uint8_t *)cb->data + cb->start_pos, data, size);
	}
}

static inline void circlebuf_push_back_zero(struct circlebuf *cb, size_t size)
{
	size_t new_end_pos = cb->end_pos + size;

	cb->size += size;
	circlebuf_ensure_capacity(cb);

	if (new_end_pos > cb->capacity) {
		size_t back_size = cb->capacity - cb->end_pos;
		size_t loop_size = size - back_size;

		if (back_size)
			memset((uint8_t *)cb->data + cb->end_pos, 0, back_size);
		memset(cb->data, 0, loop_size);

		new_end_pos -= cb->capacity;
	} else {
		memset((uint8_t *)cb->data + cb->end_pos, 0, size);
	}

	cb->end_pos = new_end_pos;
}

static inline void circlebuf_push_front_zero(struct circlebuf *cb, size_t size)
{
	cb->size += size;
	circlebuf_ensure_capacity(cb);

	if (cb->size == size) {
		cb->start_pos = 0;
		cb->end_pos = size;
		memset((uint8_t *)cb->data, 0, size);

	} else if (cb->start_pos < size) {
		size_t back_size = size - cb->start_pos;

		if (cb->start_pos)
			memset(cb->data, 0, cb->start_pos);

		cb->start_pos = cb->capacity - back_size;
		memset((uint8_t *)cb->data + cb->start_pos, 0, back_size);
	} else {
		cb->start_pos -= size;
		memset((uint8_t *)cb->data + cb->start_pos, 0, size);
	}
}

static inline void circlebuf_peek_front(struct circlebuf *cb, void *data, size_t size)
{
	assert(size <= cb->size);

	if (data) {
		size_t start_size = cb->capacity - cb->start_pos;

		if (start_size < size) {
			memcpy(data, (uint8_t *)cb->data + cb->start_pos, start_size);
			memcpy((uint8_t *)data + start_size, cb->data, size - start_size);
		} else {
			memcpy(data, (uint8_t *)cb->data + cb->start_pos, size);
		}
	}
}

static inline void circlebuf_peek_back(struct circlebuf *cb, void *data, size_t size)
{
	assert(size <= cb->size);

	if (data) {
		size_t back_size = (cb->end_pos ? cb->end_pos : cb->capacity);

		if (back_size < size) {
			size_t front_size = size - back_size;
			size_t new_end_pos = cb->capacity - front_size;

			memcpy((uint8_t *)data + (size - back_size), cb->data, back_size);
			memcpy(data, (uint8_t *)cb->data + new_end_pos, front_size);
		} else {
			memcpy(data, (uint8_t *)cb->data + cb->end_pos - size, size);
		}
	}
}

static inline void circlebuf_pop_front(struct circlebuf *cb, void *data, size_t size)
{
	circlebuf_peek_front(cb, data, size);

	cb->size -= size;
	if (!cb->size) {
		cb->start_pos = cb->end_pos = 0;
		return;
	}

	cb->start_pos += size;
	if (cb->start_pos >= cb->capacity)
		cb->start_pos -= cb->capacity;
}

static inline void circlebuf_pop_back(struct circlebuf *cb, void *data, size_t size)
{
	circlebuf_peek_back(cb, data, size);

	cb->size -= size;
	if (!cb->size) {
		cb->start_pos = cb->end_pos = 0;
		return;
	}

	if (cb->end_pos <= size)
		cb->end_pos = cb->capacity - (size - cb->end_pos);
	else
		cb->end_pos -= size;
}

static inline void *circlebuf_data(struct circlebuf *cb, size_t idx)
{
	uint8_t *ptr = (uint8_t *)cb->data;
	size_t offset = cb->start_pos + idx;

	if (idx >= cb->size)
		return NULL;

	if (offset >= cb->capacity)
		offset -= cb->capacity;

	return ptr + offset;
}
}

enum class FFTWindow
{
    NONE,
    HANN,
    HAMMING,
    BLACKMAN,
    BLACKMAN_HARRIS,
    POWER_OF_SINE
};

enum class InterpMode
{
    POINT,
    LANCZOS,
    CATROM
};

enum class FilterMode
{
    NONE,
    GAUSS
};

// temporal smoothing
enum class TSmoothingMode
{
    NONE,
    EXPONENTIAL,
    TVEXPONENTIAL
};

enum class RenderMode
{
    LINE,
    SOLID,
    GRADIENT,
    PULSE,
    RANGE
};

enum class PulseMode
{
    MAGNITUDE,
    FREQUENCY
};

enum class DisplayMode
{
    CURVE,
    BAR,
    STEPPED_BAR,
    METER,
    STEPPED_METER,
    WAVEFORM
};

enum class ChannelMode
{
    MONO,
    STEREO,
    SINGLE
};

class WAVSource
{
protected:
    // audio callback (and posssibly others) run in separate thread
    // obs_source_remove_audio_capture_callback evidently flushes the callback
    // so mutex must be recursive to avoid deadlock
    std::recursive_timed_mutex m_mtx;

    // obs sources
    obs_source_t *m_source = nullptr;               // our source
    obs_weak_source_t *m_audio_source = nullptr;    // captured audio source
    std::string m_audio_source_name;

    // audio capture
    obs_audio_info m_audio_info{};
    circlebuf m_capturebufs[2]{};
    uint32_t m_capture_channels = 0;        // audio input channels
    uint32_t m_output_channels = 0;         // fft output channels (*not* display channels)
    bool m_output_bus_captured = false;     // do we have an active audio output callback? (via audio_output_connect())

    // 32-byte aligned buffers for FFT/AVX processing
    AVXBufR m_fft_input;
    AVXBufC m_fft_output;
    fftwf_plan m_fft_plan{};
    AVXBufR m_window_coefficients;
    AVXBufR m_tsmooth_buf[2];               // last frames magnitudes
    AVXBufR m_decibels[2];                  // dBFS, or audio sample buffer in meter mode
    size_t m_fft_size = 0;                  // number of fft elements, or audio samples in meter/waveform mode (not bytes, multiple of 16)
                                            // in meter/waveform mode m_fft_size is the size of the circular buffer in samples

    // meter mode
    size_t m_meter_pos[2] = { 0, 0 };       // circular buffer position (per channel)
    float m_meter_val[2] = { 0.0f, 0.0f };  // dBFS
    float m_meter_buf[2] = { 0.0f, 0.0f };  // EMA
    bool m_meter_rms = false;               // RMS mode
    bool m_meter_mode = false;              // either meter or stepped meter display mode is selected
    int m_meter_ms = 100;                   // milliseconds of audio data to buffer

    // waveform
    size_t m_waveform_samples = 0;          // maximum number of input samples to buffer in waveform mode
    size_t m_waveform_ts = 0;               // timestamp of next sample in nanoseconds

    // video fps
    double m_fps = 0.0;

    // video size
    unsigned int m_width = 800;
    unsigned int m_height = 225;

    // show video source
    bool m_show = true;

    // graph was silent last frame
    bool m_last_silent = false;

    // audio capture retries
    int m_retries = 0;
    float m_next_retry = 0.0f;

    uint64_t m_capture_ts = 0;  // timestamp of last audio callback in nanoseconds
    uint64_t m_audio_ts = 0;    // timestamp of the end of available audio in nanoseconds
    uint64_t m_tick_ts = 0;     // timestamp of last 'tick' in nanoseconds
    int64_t m_ts_offset = 0;    // audio sync offset in nanoseconds

    // settings
    RenderMode m_render_mode = RenderMode::SOLID;
    PulseMode m_pulse_mode = PulseMode::MAGNITUDE;
    FFTWindow m_window_func = FFTWindow::HANN;
    InterpMode m_interp_mode = InterpMode::LANCZOS;
    FilterMode m_filter_mode = FilterMode::GAUSS;
    TSmoothingMode m_tsmoothing = TSmoothingMode::EXPONENTIAL;
    DisplayMode m_display_mode = DisplayMode::CURVE;
    ChannelMode m_channel_mode = ChannelMode::MONO;
    bool m_stereo = false;
    bool m_auto_fft_size = true;
    int m_cutoff_low = 0;
    int m_cutoff_high = 24000;
    int m_floor = -120;
    int m_ceiling = 0;
    float m_gravity = 0.0f;
    float m_grad_ratio = 1.0f;
    int m_range_middle = -20;
    int m_range_crest = -9;
    bool m_fast_peaks = false;
    vec4 m_color_base{ {{1.0, 1.0, 1.0, 1.0}} };
    vec4 m_color_middle{ {{1.0, 1.0, 1.0, 1.0}} };
    vec4 m_color_crest{ {{1.0, 1.0, 1.0, 1.0}} };
    float m_slope = 0.0f;
    bool m_log_scale = true;
    bool m_mirror_freq_axis = false;
    int m_bar_width = 0;
    int m_bar_gap = 0;
    int m_step_width = 0;
    int m_step_gap = 0;
    int m_num_bars = 0;
    bool m_radial = false;
    bool m_invert = false;
    float m_deadzone = 0.0f; // radial display deadzone
    float m_radial_arc = 1.0f;
    float m_radial_rotation = 0.0f;
    bool m_rounded_caps = false;
    bool m_hide_on_silent = false;
    int m_channel_spacing = 0;
    float m_rolloff_q = 0.0f;
    float m_rolloff_rate = 0.0f;
    bool m_normalize_volume = false;
    float m_volume_target = -3.0f;  // volume normalization target
    float m_max_gain = 30.0f;       // maximum volume normalization gain
    int m_min_bar_height = 0;
    int m_channel_base = 0; // channel to use in single channel mode
    bool m_ignore_mute = false;
    int m_sine_exponent = 2;

    // interpolation
    std::vector<float> m_interp_indices;
    std::vector<float> m_interp_bufs[3];    // third buffer used as intermediate for gauss filter
    std::vector<int> m_band_widths;         // size of the band each bar represents

    // roll-off
    AVXBufR m_rolloff_modifiers;

    // gaussian filter
    Kernel<float> m_kernel;
    float m_filter_radius = 0.0f;

    // lanczos filter
    Kernel<float> m_interp_kernel;

    // slope
    AVXBufR m_slope_modifiers;

    // rounded caps
    float m_cap_radius = 0.0f;
    int m_cap_tris = 4;             // number of triangles each cap is composed of (4 min)
    std::vector<vec3> m_cap_verts;  // pre-rotated cap vertices (to be translated to final pos)

    // stepped bars
    vec3 m_step_verts[6]{};         // vertices for one step of a bar (to be translated to final pos)

    // render vars
    gs_effect_t *m_shader = nullptr;
    gs_vertbuffer_t *m_vbuf = nullptr;

    // volume normalization
    float m_input_rms = 0.0f;
    AVXBufR m_input_rms_buf;
    AVXBufR m_rms_temp_buf;     // temp buffer, bit too large for stack
    circlebuf m_rms_sync_buf{}; // A/V syncronization buffer
    size_t m_input_rms_size = 0;
    size_t m_input_rms_pos = 0;

    // FFT window
    float m_window_sum = 1.0f;

    void create_vbuf();
    void free_vbuf();
    void create_shader();
    void free_shader();

    void get_settings(obs_data_t *settings);

    void recapture_audio();
    void release_audio_capture();
    bool check_audio_capture(float seconds); // check if capture is valid and retry if not
    void free_bufs();

    bool sync_rms_buffer();

    void init_interp(unsigned int sz);
    void init_rolloff();
    void init_steps();

    void render_curve(gs_effect_t *effect);
    void render_bars(gs_effect_t *effect);

    gs_technique_t *get_shader_tech();
    void set_shader_vars(float cpos, float miny, float minpos, float channel_offset, float border_top, float border_bottom);

    virtual void update_input_rms() = 0;    // update RMS window

    virtual void tick_spectrum(float) = 0;  // process audio data in frequency spectrum mode
    virtual void tick_meter(float) = 0;     // process audio data in meter mode
    virtual void tick_waveform(float) = 0;  // process audio data in waveform mode

    int64_t get_audio_sync(uint64_t ts)     // get delta between end of available audio and given time in nanoseconds
    {
        auto audio_ts = m_audio_ts + m_ts_offset;
        auto delta = std::max(audio_ts, ts) - std::min(audio_ts, ts);
        delta = std::min(delta, MAX_TS_DELTA);
        return (audio_ts < ts) ? -(int64_t)delta : (int64_t)delta;
    }

    // constants
    static const float DB_MIN;
    static constexpr auto RETRY_DELAY = 2.0f;
    static constexpr uint64_t CAPTURE_TIMEOUT = 1000000ull * 500u;  // time in nanoseconds before audio capture is considered "lost" (500 ms)
    static constexpr uint64_t MAX_TS_DELTA = 1000000000ull * 16u;   // 16 seconds in ns

    inline float dbfs(float mag)
    {
        if(mag > 0.0f)
            return 20.0f * std::log10(mag);
        else
            return DB_MIN;
    }

    inline float get_gravity(float seconds)
    {
        // FIXME: Scaling on this slider could probably use adjustment.
        // I don't remember what this constant is supposed to be but originally the idea was to tune the slider so the default value behaved
        // about the same for both EMA types at 60 FPS, but it made for weird scaling so now we have this.
        constexpr float denom = 0.03868924705242879469662125316986f;
        constexpr float hi = denom * 5.0f;
        constexpr float lo = 0.0f;
        if((m_tsmoothing == TSmoothingMode::NONE) || (m_gravity <= 0.0f))
            return 0.0f;
        return (m_tsmoothing == TSmoothingMode::TVEXPONENTIAL) ? std::exp(-seconds / lerp(lo, hi, m_gravity)) : m_gravity;
    }

public:
    WAVSource(obs_source_t *source);
    virtual ~WAVSource();

    // no copying
    WAVSource(const WAVSource&) = delete;
    WAVSource& operator=(const WAVSource&) = delete;

    unsigned int width();
    unsigned int height();

    // main callbacks
    virtual void update(obs_data_t *settings);
    virtual void tick(float seconds);
    virtual void render(gs_effect_t *effect);

    void show();
    void hide();

    static void register_source();

    // audio capture callback
    void capture_audio(obs_source_t *source, const audio_data *audio, bool muted);

    // for capturing the final OBS audio output stream
    void capture_output_bus(size_t mix_idx, const audio_data *audio);

#ifdef ENABLE_X86_SIMD
    // constants
    static const bool HAVE_AVX2;
    static const bool HAVE_AVX;
    static const bool HAVE_FMA3;
#endif // ENABLE_X86_SIMD
};

class WAVSourceGeneric : public WAVSource
{
protected:
    void tick_spectrum(float seconds) override;
    void tick_meter(float seconds) override;
    void tick_waveform(float seconds) override;

    void update_input_rms() override;

public:
    using WAVSource::WAVSource;
    ~WAVSourceGeneric() override = default;
};

#ifdef ENABLE_X86_SIMD

class WAVSourceAVX : public WAVSourceGeneric
{
protected:
    void tick_spectrum(float seconds) override;
    void tick_meter(float seconds) override;

    void update_input_rms() override;

public:
    using WAVSourceGeneric::WAVSourceGeneric;
    ~WAVSourceAVX() override = default;
};

class WAVSourceAVX2 : public WAVSourceAVX
{
protected:
    void tick_spectrum(float seconds) override;

public:
    using WAVSourceAVX::WAVSourceAVX;
    ~WAVSourceAVX2() override = default;
};

#endif // ENABLE_X86_SIMD
