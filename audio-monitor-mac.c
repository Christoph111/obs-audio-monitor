
#include "audio-monitor-mac.h"
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioQueue.h>
#include <CoreFoundation/CFString.h>
#include <CoreAudio/CoreAudio.h>
#include <stdint.h>
#include <string.h>
#include <util/deque.h>
#include <obs-module.h>

#include "media-io/audio-resampler.h"
#include "util/platform.h"
#include "util/threading.h"

#define START_RETRY_INTERVAL_NS 1000000000ULL
#define DRIFT_LOG_INTERVAL_NS 5000000000ULL
#define DRIFT_WINDOW_10S_NS 10000000000ULL
#define DRIFT_WINDOW_60S_NS 60000000000ULL
#define DRIFT_WINDOW_10M_NS 600000000000ULL
#define AUDIO_QUEUE_BUFFER_COUNT 8
#define AUDIO_QUEUE_MIN_BUFFER_COUNT 2

struct drift_window {
	uint64_t duration_ns;
	uint64_t start_ns;
	uint64_t start_frames;
	double ppm;
};

static bool success_(OSStatus stat, const char *func, const char *call)
{
	if (stat != noErr) {
		blog(LOG_WARNING, "%s: %s failed: %d", func, call, (int)stat);
		return false;
	}

	return true;
}

#define success(stat, call) success_(stat, __FUNCTION__, call)

struct audio_monitor {
	AudioQueueRef queue;
	AudioQueueBufferRef buffers[AUDIO_QUEUE_BUFFER_COUNT];
	size_t buffer_size;
	size_t wait_size;
	size_t bytes_per_frame;
	struct deque empty_buffers;
	struct deque new_data;
	volatile bool active;
	bool stopping;
	bool paused;
	uint64_t last_start_attempt_ns;
	uint32_t channels;
	uint32_t sample_rate;
	uint32_t device_sample_rate;
	audio_resampler_t *resampler;
	float volume;
	bool mono;
	float balance;
	pthread_mutex_t mutex;
	char *device_id;
	char *device_name;
	char *source_name;

	uint64_t input_frames;
	uint64_t enqueued_frames;
	uint64_t queue_callbacks;
	uint64_t underruns;
	uint64_t fill_waits;
	uint64_t padded_buffers;
	uint64_t padded_frames;
	uint64_t dropped_frames;
	uint64_t enqueue_failures;
	uint64_t pauses;
	uint64_t restarts;
	uint64_t resample_failures;
	volatile long skipped_trylocks;
	size_t peak_queue_size;
	size_t low_queue_size;
	size_t peak_total_buffer_size;
	size_t low_total_buffer_size;
	uint64_t last_audio_timestamp_ns;
	uint64_t audio_timestamp_delta_min_ns;
	uint64_t audio_timestamp_delta_max_ns;
	uint64_t audio_timestamp_delta_total_ns;
	uint64_t audio_timestamp_delta_count;
	uint64_t last_callback_ns;
	uint64_t callback_delta_min_ns;
	uint64_t callback_delta_max_ns;
	uint64_t callback_delta_total_ns;
	uint64_t callback_delta_count;
	uint64_t last_log_ns;
	struct drift_window drift_windows[3];
};

static bool monitor_success_(struct audio_monitor *monitor, OSStatus stat,
			     const char *func, const char *call)
{
	if (stat != noErr) {
		blog(LOG_WARNING,
		     "%s: %s failed for device=\"%s\" id=\"%s\" source=\"%s\" "
		     "status=%d sample_rate=%u device_sample_rate=%u channels=%u",
		     func, call, monitor->device_name ? monitor->device_name : "",
		     monitor->device_id ? monitor->device_id : "",
		     monitor->source_name ? monitor->source_name : "", (int)stat,
		     monitor->sample_rate, monitor->device_sample_rate,
		     monitor->channels);
		return false;
	}

	return true;
}

#define monitor_success(monitor, stat, call) \
	monitor_success_(monitor, stat, __FUNCTION__, call)

struct device_name_lookup {
	const char *id;
	char *name;
};

static bool find_monitoring_device_name(void *data, const char *name,
					const char *id)
{
	struct device_name_lookup *lookup = data;
	if (lookup->id && id && strcmp(id, lookup->id) == 0) {
		lookup->name = bstrdup(name ? name : id);
		return false;
	}

	return true;
}

static char *get_monitoring_device_name(const char *device_id)
{
	if (!device_id || !device_id[0])
		return bstrdup("");
	if (strcmp(device_id, "default") == 0)
		return bstrdup("default");

	struct device_name_lookup lookup = {.id = device_id, .name = NULL};
	obs_enum_audio_monitoring_devices(find_monitoring_device_name, &lookup);

	return lookup.name ? lookup.name : bstrdup(device_id);
}

static AudioDeviceID get_default_output_device_id(void)
{
	AudioObjectPropertyAddress addr = {
		kAudioHardwarePropertyDefaultOutputDevice,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain};
	AudioDeviceID id = kAudioObjectUnknown;
	UInt32 size = sizeof(id);
	OSStatus stat = AudioObjectGetPropertyData(kAudioObjectSystemObject,
						   &addr, 0, NULL, &size,
						   &id);

	return stat == noErr ? id : kAudioObjectUnknown;
}

static AudioDeviceID get_device_id_from_uid(const char *uid)
{
	if (!uid || !uid[0])
		return kAudioObjectUnknown;
	if (strcmp(uid, "default") == 0)
		return get_default_output_device_id();

	AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDevices,
					   kAudioObjectPropertyScopeGlobal,
					   kAudioObjectPropertyElementMain};
	UInt32 size = 0;
	OSStatus stat = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
						       &addr, 0, NULL,
						       &size);
	if (stat != noErr || !size)
		return kAudioObjectUnknown;

	AudioDeviceID *ids = bmalloc(size);
	if (!ids)
		return kAudioObjectUnknown;

	stat = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0,
					  NULL, &size, ids);
	if (stat != noErr) {
		bfree(ids);
		return kAudioObjectUnknown;
	}

	CFStringRef target_uid = CFStringCreateWithBytes(
		NULL, (const UInt8 *)uid, strlen(uid), kCFStringEncodingUTF8,
		false);
	if (!target_uid) {
		bfree(ids);
		return kAudioObjectUnknown;
	}

	AudioDeviceID id = kAudioObjectUnknown;
	const UInt32 count = size / sizeof(AudioDeviceID);
	for (UInt32 i = 0; i < count; i++) {
		CFStringRef device_uid = NULL;
		UInt32 uid_size = sizeof(device_uid);

		addr.mSelector = kAudioDevicePropertyDeviceUID;
		stat = AudioObjectGetPropertyData(ids[i], &addr, 0, NULL,
						  &uid_size, &device_uid);
		if (stat != noErr || !device_uid)
			continue;

		const bool match = CFStringCompare(device_uid, target_uid, 0) ==
				   kCFCompareEqualTo;
		CFRelease(device_uid);

		if (match) {
			id = ids[i];
			break;
		}
	}

	CFRelease(target_uid);
	bfree(ids);
	return id;
}

static uint32_t get_device_sample_rate(const char *uid,
				       uint32_t fallback_sample_rate)
{
	const AudioDeviceID id = get_device_id_from_uid(uid);
	if (id == kAudioObjectUnknown)
		return fallback_sample_rate;

	AudioObjectPropertyAddress addr = {
		kAudioDevicePropertyNominalSampleRate,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain};
	Float64 sample_rate = 0.0;
	UInt32 size = sizeof(sample_rate);
	OSStatus stat = AudioObjectGetPropertyData(id, &addr, 0, NULL, &size,
						   &sample_rate);
	if (stat != noErr || sample_rate < 1.0)
		return fallback_sample_rate;

	return (uint32_t)(sample_rate + 0.5);
}

static uint64_t queued_frames(const struct audio_monitor *monitor)
{
	if (!monitor->bytes_per_frame)
		return 0;

	return (uint64_t)(monitor->new_data.size / monitor->bytes_per_frame);
}

static size_t empty_buffer_count(const struct audio_monitor *monitor)
{
	return monitor->empty_buffers.size / sizeof(AudioQueueBufferRef);
}

static size_t audioqueue_buffer_count(const struct audio_monitor *monitor)
{
	const size_t empty = empty_buffer_count(monitor);
	return empty > AUDIO_QUEUE_BUFFER_COUNT ? 0 : AUDIO_QUEUE_BUFFER_COUNT - empty;
}

static size_t audioqueue_buffered_size(const struct audio_monitor *monitor)
{
	return audioqueue_buffer_count(monitor) * monitor->buffer_size;
}

static size_t total_buffered_size(const struct audio_monitor *monitor)
{
	return monitor->new_data.size + audioqueue_buffered_size(monitor);
}

static double frames_to_ms(const struct audio_monitor *monitor,
			   uint64_t frames)
{
	if (!monitor->sample_rate)
		return 0.0;

	return (double)frames * 1000.0 / (double)monitor->sample_rate;
}

static double bytes_to_ms(const struct audio_monitor *monitor, size_t bytes)
{
	if (!monitor->bytes_per_frame)
		return 0.0;

	return frames_to_ms(monitor,
			    (uint64_t)(bytes / monitor->bytes_per_frame));
}

static void reset_drift_window(struct drift_window *window, uint64_t now,
			       uint64_t frames)
{
	window->start_ns = now;
	window->start_frames = frames;
}

static void update_drift_windows(struct audio_monitor *monitor, uint64_t now)
{
	if (!monitor->sample_rate)
		return;

	const uint64_t frames = queued_frames(monitor);
	for (size_t i = 0; i < 3; i++) {
		struct drift_window *window = &monitor->drift_windows[i];
		if (!window->start_ns) {
			reset_drift_window(window, now, frames);
			continue;
		}

		const uint64_t elapsed_ns = now - window->start_ns;
		if (elapsed_ns < window->duration_ns)
			continue;

		const int64_t delta_frames =
			(int64_t)frames - (int64_t)window->start_frames;
		const double elapsed_sec = (double)elapsed_ns / 1000000000.0;
		window->ppm = ((double)delta_frames / elapsed_sec) /
			      (double)monitor->sample_rate * 1000000.0;
		reset_drift_window(window, now, frames);
	}
}

static void update_queue_watermarks(struct audio_monitor *monitor)
{
	const size_t size = monitor->new_data.size;
	const size_t total_size = total_buffered_size(monitor);
	if (size > monitor->peak_queue_size)
		monitor->peak_queue_size = size;
	if (monitor->low_queue_size == SIZE_MAX || size < monitor->low_queue_size)
		monitor->low_queue_size = size;
	if (total_size > monitor->peak_total_buffer_size)
		monitor->peak_total_buffer_size = total_size;
	if (monitor->low_total_buffer_size == SIZE_MAX ||
	    total_size < monitor->low_total_buffer_size)
		monitor->low_total_buffer_size = total_size;
}

static void reset_telemetry(struct audio_monitor *monitor, uint64_t now)
{
	monitor->input_frames = 0;
	monitor->enqueued_frames = 0;
	monitor->queue_callbacks = 0;
	monitor->underruns = 0;
	monitor->fill_waits = 0;
	monitor->padded_buffers = 0;
	monitor->padded_frames = 0;
	monitor->dropped_frames = 0;
	monitor->enqueue_failures = 0;
	monitor->pauses = 0;
	monitor->restarts = 0;
	monitor->resample_failures = 0;
	os_atomic_set_long(&monitor->skipped_trylocks, 0);
	monitor->peak_queue_size = 0;
	monitor->low_queue_size = SIZE_MAX;
	monitor->peak_total_buffer_size = 0;
	monitor->low_total_buffer_size = SIZE_MAX;
	monitor->last_audio_timestamp_ns = 0;
	monitor->audio_timestamp_delta_min_ns = 0;
	monitor->audio_timestamp_delta_max_ns = 0;
	monitor->audio_timestamp_delta_total_ns = 0;
	monitor->audio_timestamp_delta_count = 0;
	monitor->last_callback_ns = 0;
	monitor->callback_delta_min_ns = 0;
	monitor->callback_delta_max_ns = 0;
	monitor->callback_delta_total_ns = 0;
	monitor->callback_delta_count = 0;
	monitor->last_log_ns = now;

	monitor->drift_windows[0] =
		(struct drift_window){.duration_ns = DRIFT_WINDOW_10S_NS};
	monitor->drift_windows[1] =
		(struct drift_window){.duration_ns = DRIFT_WINDOW_60S_NS};
	monitor->drift_windows[2] =
		(struct drift_window){.duration_ns = DRIFT_WINDOW_10M_NS};
	for (size_t i = 0; i < 3; i++)
		reset_drift_window(&monitor->drift_windows[i], now, 0);
}

static void record_delta(uint64_t delta, uint64_t *min, uint64_t *max,
			 uint64_t *total, uint64_t *count)
{
	if (!*min || delta < *min)
		*min = delta;
	if (delta > *max)
		*max = delta;
	*total += delta;
	(*count)++;
}

static double delta_avg_ms(uint64_t total, uint64_t count)
{
	return count ? (double)total / (double)count / 1000000.0 : 0.0;
}

static void log_telemetry(struct audio_monitor *monitor, uint64_t now,
			  bool force)
{
	if (!force && now <= monitor->last_log_ns)
		return;
	if (!force && now - monitor->last_log_ns < DRIFT_LOG_INTERVAL_NS)
		return;

	update_drift_windows(monitor, now);

	const uint64_t frames = queued_frames(monitor);
	const size_t low_queue_size =
		monitor->low_queue_size == SIZE_MAX ? monitor->new_data.size
						    : monitor->low_queue_size;
	const size_t audioqueue_size = audioqueue_buffered_size(monitor);
	const size_t total_size = total_buffered_size(monitor);
	const size_t low_total_size =
		monitor->low_total_buffer_size == SIZE_MAX
			? total_size
			: monitor->low_total_buffer_size;
	const double callback_avg_ms =
		delta_avg_ms(monitor->callback_delta_total_ns,
			     monitor->callback_delta_count);
	const double audio_ts_avg_ms =
		delta_avg_ms(monitor->audio_timestamp_delta_total_ns,
			     monitor->audio_timestamp_delta_count);

	blog(LOG_INFO,
	     "AudioMonitorDrift device=\"%s\" id=\"%s\" source=\"%s\" "
	     "sample_rate=%u device_sample_rate=%u channels=%u queued_ms=%.2f "
	     "queued_frames=%llu audioqueue_ms=%.2f total_ms=%.2f peak_ms=%.2f "
	     "low_ms=%.2f total_peak_ms=%.2f total_low_ms=%.2f ppm_10s=%.2f "
	     "ppm_60s=%.2f ppm_10m=%.2f input_frames=%llu enqueued_frames=%llu "
	     "underruns=%llu fill_waits=%llu padded_buffers=%llu "
	     "padded_frames=%llu drops=%llu enqueue_failures=%llu "
	     "pauses=%llu restarts=%llu resample_failures=%llu skipped_trylocks=%ld "
	     "empty_buffers=%zu queue_buffers=%zu "
	     "callbacks=%llu callback_ms_avg=%.3f callback_ms_min=%.3f "
	     "callback_ms_max=%.3f audio_ts_ms_avg=%.3f audio_ts_ms_min=%.3f "
	     "audio_ts_ms_max=%.3f",
	     monitor->device_name ? monitor->device_name : "",
	     monitor->device_id ? monitor->device_id : "",
	     monitor->source_name ? monitor->source_name : "",
	     monitor->sample_rate, monitor->device_sample_rate, monitor->channels,
	     frames_to_ms(monitor, frames), (unsigned long long)frames,
	     bytes_to_ms(monitor, audioqueue_size),
	     bytes_to_ms(monitor, total_size),
	     bytes_to_ms(monitor, monitor->peak_queue_size),
	     bytes_to_ms(monitor, low_queue_size),
	     bytes_to_ms(monitor, monitor->peak_total_buffer_size),
	     bytes_to_ms(monitor, low_total_size),
	     monitor->drift_windows[0].ppm, monitor->drift_windows[1].ppm,
	     monitor->drift_windows[2].ppm,
	     (unsigned long long)monitor->input_frames,
	     (unsigned long long)monitor->enqueued_frames,
	     (unsigned long long)monitor->underruns,
	     (unsigned long long)monitor->fill_waits,
	     (unsigned long long)monitor->padded_buffers,
	     (unsigned long long)monitor->padded_frames,
	     (unsigned long long)monitor->dropped_frames,
	     (unsigned long long)monitor->enqueue_failures,
	     (unsigned long long)monitor->pauses,
	     (unsigned long long)monitor->restarts,
	     (unsigned long long)monitor->resample_failures,
	     os_atomic_load_long(&monitor->skipped_trylocks),
	     empty_buffer_count(monitor), audioqueue_buffer_count(monitor),
	     (unsigned long long)monitor->queue_callbacks, callback_avg_ms,
	     (double)monitor->callback_delta_min_ns / 1000000.0,
	     (double)monitor->callback_delta_max_ns / 1000000.0,
	     audio_ts_avg_ms,
	     (double)monitor->audio_timestamp_delta_min_ns / 1000000.0,
	     (double)monitor->audio_timestamp_delta_max_ns / 1000000.0);

	monitor->last_log_ns = now;
}

static void audio_monitor_reset(struct audio_monitor *audio_monitor)
{
	if (audio_monitor->queue) {
		for (size_t i = 0; i < AUDIO_QUEUE_BUFFER_COUNT; i++) {
			if (audio_monitor->buffers[i]) {
				AudioQueueFreeBuffer(audio_monitor->queue,
						     audio_monitor->buffers[i]);
				audio_monitor->buffers[i] = NULL;
			}
		}

		AudioQueueDispose(audio_monitor->queue, true);
		audio_monitor->queue = NULL;
	}

	deque_free(&audio_monitor->empty_buffers);
	deque_free(&audio_monitor->new_data);
	audio_resampler_destroy(audio_monitor->resampler);
	audio_monitor->resampler = NULL;
	audio_monitor->active = false;
	audio_monitor->paused = false;
	audio_monitor->buffer_size = 0;
	audio_monitor->wait_size = 0;
	audio_monitor->bytes_per_frame = 0;
	audio_monitor->sample_rate = 0;
	audio_monitor->device_sample_rate = 0;
}

static inline bool enqueue_buffer(struct audio_monitor *monitor,
				  AudioQueueBufferRef buf)
{
	OSStatus stat;

	stat = AudioQueueEnqueueBuffer(monitor->queue, buf, 0, NULL);
	if (!success(stat, "AudioQueueEnqueueBuffer")) {
		blog(LOG_WARNING, "%s: %s", __FUNCTION__,
		     "Failed to enqueue buffer");
		monitor->enqueue_failures++;
		monitor->active = false;
		AudioQueueStop(monitor->queue, false);
		return false;
	}
	monitor->enqueued_frames +=
		monitor->bytes_per_frame
			? monitor->buffer_size / monitor->bytes_per_frame
			: 0;
	return true;
}

static inline bool fill_buffer(struct audio_monitor *monitor)
{
	AudioQueueBufferRef buf;

	if (monitor->new_data.size < monitor->buffer_size) {
		monitor->fill_waits++;
		return false;
	}

	deque_pop_front(&monitor->empty_buffers, &buf, sizeof(buf));
	deque_pop_front(&monitor->new_data, buf->mAudioData,
			    monitor->buffer_size);
	update_queue_watermarks(monitor);

	buf->mAudioDataByteSize = monitor->buffer_size;

	return enqueue_buffer(monitor, buf);
}

static inline bool fill_padding_buffer(struct audio_monitor *monitor)
{
	AudioQueueBufferRef buf;
	const size_t copy_size = monitor->new_data.size < monitor->buffer_size
					 ? monitor->new_data.size
					 : monitor->buffer_size;

	if (monitor->empty_buffers.size == 0)
		return false;

	deque_pop_front(&monitor->empty_buffers, &buf, sizeof(buf));
	if (copy_size > 0)
		deque_pop_front(&monitor->new_data, buf->mAudioData, copy_size);
	memset((uint8_t *)buf->mAudioData + copy_size, 0,
	       monitor->buffer_size - copy_size);
	update_queue_watermarks(monitor);

	buf->mAudioDataByteSize = monitor->buffer_size;
	monitor->padded_buffers++;
	monitor->padded_frames += monitor->bytes_per_frame
					  ? (monitor->buffer_size - copy_size) /
						    monitor->bytes_per_frame
					  : 0;

	return enqueue_buffer(monitor, buf);
}

static inline bool maybe_fill_padding_buffer(struct audio_monitor *monitor,
					     const char *trigger)
{
	if (audioqueue_buffer_count(monitor) > AUDIO_QUEUE_MIN_BUFFER_COUNT)
		return false;
	if (!fill_padding_buffer(monitor))
		return false;

	blog(LOG_WARNING,
	     "AudioMonitorDriftEvent event=\"pad\" trigger=\"%s\" device=\"%s\" "
	     "id=\"%s\" source=\"%s\" queued_ms=%.2f audioqueue_ms=%.2f "
	     "total_ms=%.2f fill_waits=%llu padded_buffers=%llu "
	     "padded_frames=%llu empty_buffers=%zu queue_buffers=%zu",
	     trigger, monitor->device_name ? monitor->device_name : "",
	     monitor->device_id ? monitor->device_id : "",
	     monitor->source_name ? monitor->source_name : "",
	     bytes_to_ms(monitor, monitor->new_data.size),
	     bytes_to_ms(monitor, audioqueue_buffered_size(monitor)),
	     bytes_to_ms(monitor, total_buffered_size(monitor)),
	     (unsigned long long)monitor->fill_waits,
	     (unsigned long long)monitor->padded_buffers,
	     (unsigned long long)monitor->padded_frames,
	     empty_buffer_count(monitor), audioqueue_buffer_count(monitor));
	return true;
}

static void buffer_audio(void *data, AudioQueueRef aq, AudioQueueBufferRef buf)
{
	struct audio_monitor *monitor = data;
	const uint64_t now = os_gettime_ns();

	pthread_mutex_lock(&monitor->mutex);
	if (!monitor->active || monitor->stopping) {
		pthread_mutex_unlock(&monitor->mutex);
		return;
	}
	monitor->queue_callbacks++;
	if (monitor->last_callback_ns) {
		record_delta(now - monitor->last_callback_ns,
			     &monitor->callback_delta_min_ns,
			     &monitor->callback_delta_max_ns,
			     &monitor->callback_delta_total_ns,
			     &monitor->callback_delta_count);
	}
	monitor->last_callback_ns = now;

	deque_push_back(&monitor->empty_buffers, &buf, sizeof(buf));
	while (monitor->empty_buffers.size > 0) {
		if (!fill_buffer(monitor)) {
			if (maybe_fill_padding_buffer(monitor, "callback"))
				continue;
			break;
		}
	}
	if (empty_buffer_count(monitor) == AUDIO_QUEUE_BUFFER_COUNT) {
		monitor->paused = true;
		monitor->wait_size =
			monitor->buffer_size * AUDIO_QUEUE_BUFFER_COUNT;
		monitor->underruns++;
		monitor->pauses++;
		blog(LOG_WARNING,
		     "AudioMonitorDriftEvent event=\"pause\" device=\"%s\" "
		     "id=\"%s\" source=\"%s\" queued_ms=%.2f "
		     "audioqueue_ms=%.2f total_ms=%.2f fill_waits=%llu "
		     "padded_buffers=%llu padded_frames=%llu underruns=%llu "
		     "pauses=%llu callbacks=%llu "
		     "empty_buffers=%zu queue_buffers=%zu",
		     monitor->device_name ? monitor->device_name : "",
		     monitor->device_id ? monitor->device_id : "",
		     monitor->source_name ? monitor->source_name : "",
		     bytes_to_ms(monitor, monitor->new_data.size),
		     bytes_to_ms(monitor, audioqueue_buffered_size(monitor)),
		     bytes_to_ms(monitor, total_buffered_size(monitor)),
		     (unsigned long long)monitor->fill_waits,
		     (unsigned long long)monitor->padded_buffers,
		     (unsigned long long)monitor->padded_frames,
		     (unsigned long long)monitor->underruns,
		     (unsigned long long)monitor->pauses,
		     (unsigned long long)monitor->queue_callbacks,
		     empty_buffer_count(monitor), audioqueue_buffer_count(monitor));
		AudioQueuePause(monitor->queue);
	}
	log_telemetry(monitor, os_gettime_ns(), false);
	pthread_mutex_unlock(&monitor->mutex);

	UNUSED_PARAMETER(aq);
}

void audio_monitor_stop(struct audio_monitor *audio_monitor){
	if (!audio_monitor)
		return;

	pthread_mutex_lock(&audio_monitor->mutex);
	if (audio_monitor->stopping) {
		pthread_mutex_unlock(&audio_monitor->mutex);
		return;
	}
	audio_monitor->stopping = true;
	AudioQueueRef queue = audio_monitor->queue;
	bool active = audio_monitor->active;
	audio_monitor->active = false;
	audio_monitor->paused = false;
	pthread_mutex_unlock(&audio_monitor->mutex);

	if (queue && active) {
		AudioQueueStop(queue, true);
	}

	pthread_mutex_lock(&audio_monitor->mutex);
	if (audio_monitor->sample_rate)
		log_telemetry(audio_monitor, os_gettime_ns(), true);
	audio_monitor_reset(audio_monitor);
	audio_monitor->stopping = false;
	pthread_mutex_unlock(&audio_monitor->mutex);
}

void audio_monitor_start(struct audio_monitor *audio_monitor){
	if (!audio_monitor)
		return;

	uint64_t now = os_gettime_ns();

	pthread_mutex_lock(&audio_monitor->mutex);
	if (audio_monitor->active || audio_monitor->stopping) {
		pthread_mutex_unlock(&audio_monitor->mutex);
		return;
	}
	if (audio_monitor->last_start_attempt_ns &&
	    now - audio_monitor->last_start_attempt_ns <
		    START_RETRY_INTERVAL_NS) {
		pthread_mutex_unlock(&audio_monitor->mutex);
		return;
	}
	audio_monitor->last_start_attempt_ns = now;

	audio_monitor_reset(audio_monitor);

    const struct audio_output_info *info =
		audio_output_get_info(obs_get_audio());
    audio_monitor->channels = get_audio_channels(info->speakers);
	audio_monitor->sample_rate = info->samples_per_sec;
	audio_monitor->device_sample_rate =
		get_device_sample_rate(audio_monitor->device_id,
				       info->samples_per_sec);
	audio_monitor->bytes_per_frame =
		audio_monitor->channels * sizeof(float);
	audio_monitor->buffer_size = audio_monitor->bytes_per_frame *
				     info->samples_per_sec / 100 * 3;
	audio_monitor->wait_size =
		audio_monitor->buffer_size * AUDIO_QUEUE_BUFFER_COUNT;
	reset_telemetry(audio_monitor, now);
	AudioStreamBasicDescription desc = {
		.mSampleRate = (Float64)info->samples_per_sec,
		.mFormatID = kAudioFormatLinearPCM,
		.mFormatFlags = kAudioFormatFlagIsFloat |
				kAudioFormatFlagIsPacked,
		.mBytesPerPacket = sizeof(float) * audio_monitor->channels,
		.mFramesPerPacket = 1,
		.mBytesPerFrame = sizeof(float) * audio_monitor->channels,
		.mChannelsPerFrame = audio_monitor->channels,
		.mBitsPerChannel = sizeof(float) * 8};

	OSStatus stat = AudioQueueNewOutput(&desc, buffer_audio, audio_monitor,
					    NULL, NULL, 0,
					    &audio_monitor->queue);
	if (!monitor_success(audio_monitor, stat, "AudioQueueNewOutput"))
		goto fail;

	if (strcmp(audio_monitor->device_id, "default") != 0) {
		CFStringRef cf_uid = CFStringCreateWithBytes(
			NULL, (const UInt8 *)audio_monitor->device_id,
			strlen(audio_monitor->device_id), kCFStringEncodingUTF8,
			false);

		stat = AudioQueueSetProperty(audio_monitor->queue,
					     kAudioQueueProperty_CurrentDevice,
					     &cf_uid, sizeof(cf_uid));
		CFRelease(cf_uid);
		if (!monitor_success(audio_monitor, stat, "set current device"))
			goto fail;
	}
	stat = AudioQueueSetParameter(audio_monitor->queue,
				      kAudioQueueParam_Volume, 1.0);
	if (!monitor_success(audio_monitor, stat, "set volume"))
		goto fail;

	for (size_t i = 0; i < AUDIO_QUEUE_BUFFER_COUNT; i++) {
		stat = AudioQueueAllocateBuffer(audio_monitor->queue,
						audio_monitor->buffer_size,
						&audio_monitor->buffers[i]);
		if (!monitor_success(audio_monitor, stat,
				     "allocation of buffer"))
			goto fail;

		deque_push_back(&audio_monitor->empty_buffers,
				    &audio_monitor->buffers[i],
				    sizeof(audio_monitor->buffers[i]));
	}
	struct resample_info from = {.samples_per_sec = info->samples_per_sec,
				     .speakers = info->speakers,
				     .format = AUDIO_FORMAT_FLOAT_PLANAR};
	struct resample_info to = {.samples_per_sec = info->samples_per_sec,
				   .speakers = info->speakers,
				   .format = AUDIO_FORMAT_FLOAT};
	audio_monitor->resampler = audio_resampler_create(&to, &from);
	if (!audio_monitor->resampler)
		goto fail;

	stat = AudioQueueStart(audio_monitor->queue, NULL);
	if (!monitor_success(audio_monitor, stat, "start"))
		goto fail;

	audio_monitor->active = true;
	audio_monitor->last_start_attempt_ns = 0;
	blog(LOG_INFO,
	     "AudioMonitorDriftStart device=\"%s\" id=\"%s\" source=\"%s\" "
	     "sample_rate=%u device_sample_rate=%u channels=%u buffer_ms=%.2f "
	     "wait_ms=%.2f queue_buffers=%u",
	     audio_monitor->device_name ? audio_monitor->device_name : "",
	     audio_monitor->device_id ? audio_monitor->device_id : "",
	     audio_monitor->source_name ? audio_monitor->source_name : "",
	     audio_monitor->sample_rate, audio_monitor->device_sample_rate,
	     audio_monitor->channels,
	     bytes_to_ms(audio_monitor, audio_monitor->buffer_size),
	     bytes_to_ms(audio_monitor, audio_monitor->wait_size),
	     (unsigned)AUDIO_QUEUE_BUFFER_COUNT);
	pthread_mutex_unlock(&audio_monitor->mutex);
	return;

fail:
	audio_monitor_reset(audio_monitor);
	pthread_mutex_unlock(&audio_monitor->mutex);
}

void audio_monitor_audio(void *data, struct obs_audio_data *audio){
	struct audio_monitor *audio_monitor = data;
	if (!audio_monitor->device_id || !strlen(audio_monitor->device_id))
		return;

	if (!os_atomic_load_bool(&audio_monitor->active)) {
		audio_monitor_start(audio_monitor);
	}

    if (!os_atomic_load_bool(&audio_monitor->active))
		return;
	if (!audio_monitor->resampler)
		return;
	if (pthread_mutex_trylock(&audio_monitor->mutex) != 0) {
		os_atomic_inc_long(&audio_monitor->skipped_trylocks);
		return;
	}

	if (audio_monitor->stopping) {
		pthread_mutex_unlock(&audio_monitor->mutex);
		return;
	}

	uint8_t *resample_data[MAX_AV_PLANES];
	uint32_t resample_frames;
	uint64_t ts_offset;
	bool success = audio_resampler_resample(
		audio_monitor->resampler, resample_data, &resample_frames,
		&ts_offset, (const uint8_t *const *)audio->data,
		(uint32_t)audio->frames);
	if (!success) {
		audio_monitor->resample_failures++;
		pthread_mutex_unlock(&audio_monitor->mutex);
		return;
	}
	if (audio->timestamp) {
		if (audio_monitor->last_audio_timestamp_ns &&
		    audio->timestamp > audio_monitor->last_audio_timestamp_ns) {
			record_delta(audio->timestamp -
					     audio_monitor
						     ->last_audio_timestamp_ns,
				     &audio_monitor
					      ->audio_timestamp_delta_min_ns,
				     &audio_monitor
					      ->audio_timestamp_delta_max_ns,
				     &audio_monitor
					      ->audio_timestamp_delta_total_ns,
				     &audio_monitor
					      ->audio_timestamp_delta_count);
		}
		audio_monitor->last_audio_timestamp_ns = audio->timestamp;
	}
	/* apply volume */
	float vol = audio_monitor->volume;
	if (!close_float(vol, 1.0f, EPSILON)) {
		register float *cur = (float *)resample_data[0];
		register float *end =
			cur + resample_frames * audio_monitor->channels;

		while (cur < end)
			*(cur++) *= vol;
	}
	/* apply mono */
	if (audio_monitor->mono && audio_monitor->channels > 1) {
		for (uint32_t frame = 0; frame < resample_frames; frame++) {
			float avg = 0.0f;
			for (uint32_t channel = 0; channel < audio_monitor->channels; channel++) {
				avg += ((float *)resample_data[0])[frame * audio_monitor->channels + channel];
			}
			avg /= (float)audio_monitor->channels;
			for (uint32_t channel = 0; channel < audio_monitor->channels; channel++) {
				((float *)resample_data[0])[frame * audio_monitor->channels + channel] = avg;
			}
		}
	}
	/* apply balance */
	float bal = (audio_monitor->balance + 1.0f) / 2.0f;
	if (!close_float(bal, 0.5f, EPSILON) && audio_monitor->channels > 1) {
			for (uint32_t frame = 0; frame < resample_frames; frame++) {
				((float *)resample_data[0])[frame * audio_monitor->channels + 0] =
					((float *)resample_data[0])[frame * audio_monitor->channels + 0] *
					sinf((1.0f - bal) * (M_PI / 2.0f));
				((float *)resample_data[0])[frame * audio_monitor->channels + 1] =
					((float *)resample_data[0])[frame * audio_monitor->channels + 1] *
					sinf(bal * (M_PI / 2.0f));
			}
	}
	uint32_t bytes =
		sizeof(float) * audio_monitor->channels * resample_frames;
	deque_push_back(&audio_monitor->new_data, resample_data[0], bytes);
	audio_monitor->input_frames += resample_frames;
	update_queue_watermarks(audio_monitor);
	if (audio_monitor->new_data.size >= audio_monitor->wait_size) {
		audio_monitor->wait_size = 0;

		while (audio_monitor->empty_buffers.size > 0) {
			if (!fill_buffer(audio_monitor)) {
				if (maybe_fill_padding_buffer(audio_monitor, "input"))
					continue;
				break;
			}
		}

		if (audio_monitor->paused) {
			OSStatus stat = AudioQueueStart(audio_monitor->queue, NULL);
			if (success(stat, "restart")) {
				audio_monitor->paused = false;
				audio_monitor->restarts++;
				blog(LOG_WARNING,
				     "AudioMonitorDriftEvent event=\"restart\" "
				     "device=\"%s\" id=\"%s\" source=\"%s\" "
				     "queued_ms=%.2f audioqueue_ms=%.2f "
				     "total_ms=%.2f fill_waits=%llu "
				     "padded_buffers=%llu padded_frames=%llu "
				     "underruns=%llu pauses=%llu restarts=%llu "
				     "empty_buffers=%zu queue_buffers=%zu",
				     audio_monitor->device_name
					     ? audio_monitor->device_name
					     : "",
				     audio_monitor->device_id ? audio_monitor->device_id
							      : "",
				     audio_monitor->source_name
					     ? audio_monitor->source_name
					     : "",
				     bytes_to_ms(audio_monitor,
						 audio_monitor->new_data.size),
				     bytes_to_ms(audio_monitor,
						 audioqueue_buffered_size(
							 audio_monitor)),
				     bytes_to_ms(audio_monitor,
						 total_buffered_size(audio_monitor)),
				     (unsigned long long)audio_monitor->fill_waits,
				     (unsigned long long)audio_monitor->padded_buffers,
				     (unsigned long long)audio_monitor->padded_frames,
				     (unsigned long long)audio_monitor->underruns,
				     (unsigned long long)audio_monitor->pauses,
				     (unsigned long long)audio_monitor->restarts,
				     empty_buffer_count(audio_monitor),
				     audioqueue_buffer_count(audio_monitor));
			} else {
				audio_monitor->active = false;
			}
		}
	}
	log_telemetry(audio_monitor, os_gettime_ns(), false);
    pthread_mutex_unlock(&audio_monitor->mutex);
}

void audio_monitor_set_volume(struct audio_monitor *audio_monitor, float volume){
	if (!audio_monitor)
		return;
    audio_monitor->volume = volume;
}

void audio_monitor_set_mono(struct audio_monitor *audio_monitor, bool mono){
	if (!audio_monitor)
		return;
	audio_monitor->mono = mono;
}

void audio_monitor_set_balance(struct audio_monitor *audio_monitor, float balance){
	if (!audio_monitor)
		return;
	audio_monitor->balance = balance;
}

struct audio_monitor *audio_monitor_create(const char *device_id, const char* source_name, int port){
	UNUSED_PARAMETER(port);
	struct audio_monitor *audio_monitor = bzalloc(sizeof(struct audio_monitor));
	audio_monitor->device_id = bstrdup(device_id);
	audio_monitor->device_name = get_monitoring_device_name(device_id);
	audio_monitor->source_name = bstrdup(source_name ? source_name : "");
	pthread_mutex_init(&audio_monitor->mutex, NULL);
	return audio_monitor;
}

void audio_monitor_destroy(struct audio_monitor *audio_monitor){
	if (!audio_monitor)
		return;
	audio_monitor_stop(audio_monitor);
	pthread_mutex_destroy(&audio_monitor->mutex);
    bfree(audio_monitor->device_id);
	bfree(audio_monitor->device_name);
	bfree(audio_monitor->source_name);
	bfree(audio_monitor);
}

const char *audio_monitor_get_device_id(struct audio_monitor *audio_monitor){
	if (!audio_monitor)
		return NULL;
    return audio_monitor->device_id;
}

void audio_monitor_set_format(struct audio_monitor *audio_monitor,
			      enum audio_format format){
	UNUSED_PARAMETER(audio_monitor);
	UNUSED_PARAMETER(format);
}

void audio_monitor_set_samples_per_sec(struct audio_monitor *audio_monitor,
				       long long samples_per_sec){
	UNUSED_PARAMETER(audio_monitor);
	UNUSED_PARAMETER(samples_per_sec);
}
