/* OpenSL ES compatibility layer for Drastic.
 *
 * Drastic uses Android's push-style simple buffer queue.  This implementation
 * preserves that ABI and feeds libnx audren from a small page-aligned ring.
 * Recorder queues are fed either with deterministic silence for Drastic's
 * white-noise mode or with live Horizon AudioIn samples from an attached
 * headset/USB microphone.
 */

#include <switch.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "opensles.h"
#include "pthr.h"
#include "switch_mic.h"

#define OSL_QUEUE_CAPACITY 16
#define OSL_WAVEBUFS 4
#define OSL_BLOCK_CAPACITY 0x10000
/* Length of the fades where playback runs dry and resumes (2.9 ms at 44.1 kHz). */
#define OSL_FADE_FRAMES 128
/* Fade the end of the queued audio once this little is left and nothing follows:
 * one 5 ms renderer frame of polling plus the fade itself. */
#define OSL_TAIL_FADE_MS 10
/* After running dry, resume once this many buffers are waiting. */
#define OSL_RESUME_PACKETS 2

#define OSL_MIC_CAPTURE_BUFFERS 4
#define OSL_MIC_CAPTURE_FRAMES 480
#define OSL_MIC_CAPTURE_PAGE_SIZE 0x1000
#define OSL_MIC_FIFO_FRAMES 4096
#define OSL_MIC_LIVE_FIFO_FRAMES (OSL_MIC_CAPTURE_FRAMES + 1)
#define OSL_MIC_POLL_NS 5000000ULL
#define OSL_MIC_PACKET_WAIT_NS 20000000ULL
#define OSL_MIC_RETRY_NS 2000000000ULL
#define OSL_MIC_STALE_TIMEOUTS 40

typedef enum {
  OSL_OBJECT_ENGINE,
  OSL_OBJECT_OUTPUT_MIX,
  OSL_OBJECT_PLAYER,
  OSL_OBJECT_RECORDER,
} OslObjectKind;

typedef struct OslObject OslObject;
typedef struct OslAudioObject OslAudioObject;

typedef struct {
  const void *vtable;
  OslObject *owner;
} OslInterface;

struct OslObject {
  const struct SLObjectItf_ *object_vtable;
  OslObjectKind kind;
  SLuint32 state;
  OslInterface engine;
};

typedef struct {
  const void *data;
  SLuint32 size;
} OslPacket;

struct OslAudioObject {
  OslObject base;
  OslInterface play;
  OslInterface record;
  OslInterface queue;
  OslInterface volume;

  Mutex lock;
  CondVar wake;
  pthread_t thread;
  int thread_started;
  volatile int running;
  int suspended;

  SLuint32 io_state;
  SLuint32 sample_rate;
  SLuint32 channels;
  SLuint32 bits;
  SLmillibel volume_mb;
  SLboolean muted;

  OslPacket packets[OSL_QUEUE_CAPACITY];
  unsigned queue_capacity;
  unsigned packet_head;
  unsigned packet_tail;
  unsigned packet_count;
  unsigned completed_count;
  void (*queue_callback)(void *caller, void *context);
  void *queue_context;

  int audren_inited;
  AudioDriver driver;
  int mempool_id;
  int voice_id;
  void *ring;
  size_t ring_size;
  AudioDriverWaveBuf wavebufs[OSL_WAVEBUFS];
  unsigned wave_pending[OSL_WAVEBUFS];
  unsigned next_wave;
  unsigned last_wave;
  u32 queued_frames; /* frames handed to the voice, on its played-sample scale */
  int tail_faded;
  int fade_in_next;
  int rebuffering;
  Handle callback_thread;
  volatile int callback_running;

  SwitchMic microphone;
  void *mic_capture_ring;
  AudioInBuffer mic_capture_buffers[OSL_MIC_CAPTURE_BUFFERS];
  int16_t *mic_fifo;
  size_t mic_fifo_head;
  size_t mic_fifo_count;
  uint64_t mic_resample_phase;
  uint64_t mic_retry_tick;
  uint64_t mic_device_scan_tick;
  unsigned mic_timeout_count;
  unsigned mic_config_generation;
  int mic_backend_active;
};

static OslAudioObject *g_player;
static OslAudioObject *g_recorder;
static unsigned g_master_volume = 100;
static volatile unsigned g_microphone_source = OPENSLES_MIC_SOURCE_SIMULATED;
static volatile int g_microphone_enabled = 1;
static volatile unsigned g_microphone_status = OPENSLES_MIC_STATUS_SIMULATED;
static volatile unsigned g_microphone_generation = 1;

static const struct SLInterfaceID_ iid_engine = { 1, 1, 1, 1, {1,1,1,1,1,1} };
static const struct SLInterfaceID_ iid_play = { 2, 2, 2, 2, {2,2,2,2,2,2} };
static const struct SLInterfaceID_ iid_bufferqueue = { 3, 3, 3, 3, {3,3,3,3,3,3} };
static const struct SLInterfaceID_ iid_androidqueue = { 4, 4, 4, 4, {4,4,4,4,4,4} };
static const struct SLInterfaceID_ iid_volume = { 5, 5, 5, 5, {5,5,5,5,5,5} };
static const struct SLInterfaceID_ iid_record = { 6, 6, 6, 6, {6,6,6,6,6,6} };

const SLInterfaceID SL_IID_ENGINE = &iid_engine;
const SLInterfaceID SL_IID_PLAY = &iid_play;
const SLInterfaceID SL_IID_BUFFERQUEUE = &iid_bufferqueue;
const SLInterfaceID SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &iid_androidqueue;
const SLInterfaceID SL_IID_VOLUME = &iid_volume;
const SLInterfaceID SL_IID_RECORD = &iid_record;

static const AudioRendererConfig k_audio_config = {
  .output_rate = AudioRendererOutputRate_48kHz,
  /* libnx allocates the voice-channel pool from num_voices as well. A stereo
   * Drastic voice needs at least two entries; four leaves safe driver headroom. */
  .num_voices = 4,
  .num_effects = 0,
  .num_sinks = 1,
  .num_mix_objs = 1,
  .num_mix_buffers = 2,
};

static OslInterface *interface_from_self(const void *self) {
  return (OslInterface *)self;
}

static OslAudioObject *audio_from_self(const void *self) {
  OslInterface *interface = interface_from_self(self);
  return (OslAudioObject *)interface->owner;
}

static float player_gain(const OslAudioObject *audio) {
  if (audio->muted || g_master_volume == 0) return 0.0f;
  float gain = powf(10.0f, (float)audio->volume_mb / 2000.0f);
  gain *= (float)g_master_volume / 100.0f;
  if (gain < 0.0f) gain = 0.0f;
  if (gain > 1.0f) gain = 1.0f;
  return gain;
}

static void player_update_gain(OslAudioObject *audio) {
  if (!audio || !audio->audren_inited) return;
  mutexLock(&audio->lock);
  audrvVoiceSetVolume(&audio->driver, audio->voice_id, player_gain(audio));
  audrvUpdate(&audio->driver);
  mutexUnlock(&audio->lock);
}

void opensles_set_master_volume(unsigned percent) {
  if (percent > 100) percent = 100;
  g_master_volume = percent;
  player_update_gain(g_player);
}

static void wake_audio_thread(OslAudioObject *audio) {
  if (!audio) return;
  mutexLock(&audio->lock);
  condvarWakeAll(&audio->wake);
  mutexUnlock(&audio->lock);
}

static void microphone_set_status(OpenSLESMicrophoneStatus status) {
  __atomic_store_n(&g_microphone_status, (unsigned)status, __ATOMIC_RELEASE);
}

void opensles_set_microphone_enabled(bool enabled) {
  __atomic_store_n(&g_microphone_enabled, enabled ? 1 : 0, __ATOMIC_RELEASE);
  __atomic_add_fetch(&g_microphone_generation, 1, __ATOMIC_ACQ_REL);
  if (!enabled)
    microphone_set_status(OPENSLES_MIC_STATUS_DISABLED);
  else if (__atomic_load_n(&g_microphone_source, __ATOMIC_ACQUIRE) ==
           OPENSLES_MIC_SOURCE_EXTERNAL)
    microphone_set_status(OPENSLES_MIC_STATUS_CONNECTING);
  else
    microphone_set_status(OPENSLES_MIC_STATUS_SIMULATED);
  wake_audio_thread(g_recorder);
}

void opensles_set_microphone_source(OpenSLESMicrophoneSource source) {
  if (source != OPENSLES_MIC_SOURCE_EXTERNAL)
    source = OPENSLES_MIC_SOURCE_SIMULATED;
  __atomic_store_n(&g_microphone_source, (unsigned)source, __ATOMIC_RELEASE);
  __atomic_add_fetch(&g_microphone_generation, 1, __ATOMIC_ACQ_REL);
  if (!__atomic_load_n(&g_microphone_enabled, __ATOMIC_ACQUIRE))
    microphone_set_status(OPENSLES_MIC_STATUS_DISABLED);
  else if (source == OPENSLES_MIC_SOURCE_EXTERNAL)
    microphone_set_status(OPENSLES_MIC_STATUS_CONNECTING);
  else
    microphone_set_status(OPENSLES_MIC_STATUS_SIMULATED);
  wake_audio_thread(g_recorder);
}

OpenSLESMicrophoneStatus opensles_get_microphone_status(void) {
  return (OpenSLESMicrophoneStatus)__atomic_load_n(
      &g_microphone_status, __ATOMIC_ACQUIRE);
}

static void audio_set_suspended(OslAudioObject *audio, bool suspended) {
  if (!audio) return;
  mutexLock(&audio->lock);
  audio->suspended = suspended ? 1 : 0;
  if (audio->audren_inited) {
    audrvVoiceSetPaused(&audio->driver, audio->voice_id,
                        audio->suspended ||
                        audio->io_state != SL_PLAYSTATE_PLAYING);
    audrvUpdate(&audio->driver);
  }
  condvarWakeAll(&audio->wake);
  mutexUnlock(&audio->lock);
}

void opensles_set_suspended(bool suspended) {
  audio_set_suspended(g_player, suspended);
  audio_set_suspended(g_recorder, suspended);
}

static int player_voice_init(OslAudioObject *audio) {
  const int voice_ok = audrvVoiceInit(
      &audio->driver, audio->voice_id, (int)audio->channels,
      PcmFormat_Int16, (int)audio->sample_rate);
  if (!voice_ok)
    return 0;
  audrvVoiceSetDestinationMix(&audio->driver, audio->voice_id,
                              AUDREN_FINAL_MIX_ID);
  if (audio->channels == 1) {
    audrvVoiceSetMixFactor(&audio->driver, audio->voice_id, 1.0f, 0, 0);
    audrvVoiceSetMixFactor(&audio->driver, audio->voice_id, 1.0f, 0, 1);
  } else {
    audrvVoiceSetMixFactor(&audio->driver, audio->voice_id, 1.0f, 0, 0);
    audrvVoiceSetMixFactor(&audio->driver, audio->voice_id, 1.0f, 1, 1);
    audrvVoiceSetMixFactor(&audio->driver, audio->voice_id, 0.0f, 1, 0);
    audrvVoiceSetMixFactor(&audio->driver, audio->voice_id, 0.0f, 0, 1);
  }
  audrvVoiceSetVolume(&audio->driver, audio->voice_id, player_gain(audio));
  audrvVoiceStart(&audio->driver, audio->voice_id);
  audrvVoiceSetPaused(&audio->driver, audio->voice_id,
                      audio->suspended ||
                      audio->io_state != SL_PLAYSTATE_PLAYING);
  const Result rc = audrvUpdate(&audio->driver);
  return R_SUCCEEDED(rc);
}

static int player_backend_init(OslAudioObject *audio) {
  Result rc = audrenInitialize(&k_audio_config);
  if (R_FAILED(rc)) return 0;
  audio->audren_inited = 1;
  rc = audrvCreate(&audio->driver, &k_audio_config, 2);
  if (R_FAILED(rc)) goto fail_audren;

  audio->ring_size = (OSL_WAVEBUFS * OSL_BLOCK_CAPACITY + 0xfff) & ~0xfff;
  audio->ring = aligned_alloc(0x1000, audio->ring_size);
  if (!audio->ring) goto fail_driver;
  memset(audio->ring, 0, audio->ring_size);
  armDCacheFlush(audio->ring, audio->ring_size);

  audio->mempool_id = audrvMemPoolAdd(&audio->driver, audio->ring,
                                       audio->ring_size);
  if (audio->mempool_id < 0) goto fail_ring;
  if (!audrvMemPoolAttach(&audio->driver, audio->mempool_id))
    goto fail_pool;
  static const u8 sink_channels[] = { 0, 1 };
  const int sink_id = audrvDeviceSinkAdd(
      &audio->driver, AUDREN_DEFAULT_DEVICE_NAME, 2, sink_channels);
  if (sink_id < 0)
    goto fail_pool;

  /* Commit the mempool and sink before allocating the stereo voice. */
  rc = audrvUpdate(&audio->driver);
  if (R_FAILED(rc)) goto fail_pool;

  audio->voice_id = 0;
  if (!player_voice_init(audio)) goto fail_pool;
  rc = audrenStartAudioRenderer();
  if (R_FAILED(rc)) goto fail_pool;
  return 1;

fail_pool:
  if (audio->mempool_id >= 0) {
    audrvMemPoolDetach(&audio->driver, audio->mempool_id);
    audrvMemPoolRemove(&audio->driver, audio->mempool_id);
  }
fail_ring:
  free(audio->ring);
  audio->ring = NULL;
fail_driver:
  audrvClose(&audio->driver);
fail_audren:
  audrenExit();
  audio->audren_inited = 0;
  return 0;
}

static int wave_is_free(const AudioDriverWaveBuf *wave) {
  return wave->state == AudioDriverWaveBufState_Free ||
         wave->state == AudioDriverWaveBufState_Done;
}

static void queue_invoke_callback(OslAudioObject *audio) {
  void (*callback)(void *, void *) = audio->queue_callback;
  void *context = audio->queue_context;
  if (callback)
    callback((void *)&audio->queue.vtable, context);
}

static unsigned player_pending_count_locked(const OslAudioObject *audio) {
  unsigned count = 0;
  for (unsigned index = 0; index < OSL_WAVEBUFS; index++)
    count += audio->wave_pending[index] != 0;
  return count;
}

static unsigned queue_outstanding_locked(const OslAudioObject *audio) {
  return audio->packet_count + player_pending_count_locked(audio);
}

/* Linear fade over at most OSL_FADE_FRAMES frames of 16-bit PCM. */
static void fade_pcm16(int16_t *samples, size_t frames, unsigned channels,
                       int fade_in) {
  if (frames > OSL_FADE_FRAMES) frames = OSL_FADE_FRAMES;
  for (size_t frame = 0; frame < frames; frame++) {
    const int32_t gain = fade_in ? (int32_t)(frame + 1)
                                 : (int32_t)(frames - frame - 1);
    for (unsigned channel = 0; channel < channels; channel++) {
      int16_t *sample = &samples[frame * channels + channel];
      *sample = (int16_t)((int32_t)*sample * gain / (int32_t)frames);
    }
  }
}

/* The playback thread shares its core with Drastic's busy worker threads.
 * Running ahead of them gets completions to Drastic on time; a late one makes
 * Drastic think its queue is full and discard a frame of audio. The thread
 * sleeps between audio renderer frames. */
static void player_raise_priority(void) {
  s32 current;
  u64 allowed;
  if (R_FAILED(svcGetThreadPriority(&current, CUR_THREAD_HANDLE)) ||
      R_FAILED(svcGetInfo(&allowed, InfoType_PriorityMask,
                          CUR_PROCESS_HANDLE, 0)))
    return;
  for (s32 priority = current - 4; priority < current; priority++) {
    if (priority >= 0 && ((allowed >> priority) & 1) &&
        R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, (u32)priority)))
      return;
  }
}

static void *player_thread(void *argument) {
  OslAudioObject *audio = (OslAudioObject *)argument;
  pthr_ensure_fake_tls();
  pthr_pin_bg_core();
  player_raise_priority();
  audio->callback_thread = threadGetCurHandle();

  while (__atomic_load_n(&audio->running, __ATOMIC_ACQUIRE)) {
    int invoke_callback = 0;
    mutexLock(&audio->lock);
    audrvUpdate(&audio->driver);

    for (unsigned index = 0; index < OSL_WAVEBUFS; index++) {
      AudioDriverWaveBuf *wave = &audio->wavebufs[index];
      if (audio->wave_pending[index] &&
          wave->state == AudioDriverWaveBufState_Done) {
        audio->wave_pending[index] = 0;
        audio->completed_count++;
        invoke_callback = 1;
        break;
      }
    }

    /* The voice ran dry. Wait for a cushion before resuming, as Drastic's own
     * silence buffer did, and fade the restart in. */
    if (!audio->rebuffering && audio->queued_frames &&
        !audio->packet_count && !player_pending_count_locked(audio)) {
      audio->rebuffering = 1;
      audio->fade_in_next = 1;
      audio->queued_frames =
          audrvVoiceGetPlayedSampleCount(&audio->driver, audio->voice_id);
    }

    if (!audio->suspended &&
        audio->io_state == SL_PLAYSTATE_PLAYING && audio->packet_count &&
        (!audio->rebuffering || audio->packet_count >= OSL_RESUME_PACKETS)) {
      unsigned index = audio->next_wave;
      unsigned searched = 0;
      while (searched < OSL_WAVEBUFS &&
             (audio->wave_pending[index] ||
              !wave_is_free(&audio->wavebufs[index]))) {
        index = (index + 1) % OSL_WAVEBUFS;
        searched++;
      }

      if (searched < OSL_WAVEBUFS) {
        AudioDriverWaveBuf *wave = &audio->wavebufs[index];
        OslPacket packet = audio->packets[audio->packet_head];
        audio->packet_head = (audio->packet_head + 1) % OSL_QUEUE_CAPACITY;
        audio->packet_count--;

        size_t size = packet.size;
        if (size > OSL_BLOCK_CAPACITY) size = OSL_BLOCK_CAPACITY;
        const size_t frame_bytes = audio->channels * (audio->bits / 8);
        if (frame_bytes) size -= size % frame_bytes;
        void *block = (char *)audio->ring + index * OSL_BLOCK_CAPACITY;
        memcpy(block, packet.data, size);
        if (audio->fade_in_next && audio->bits == 16 && frame_bytes)
          fade_pcm16(block, size / frame_bytes, audio->channels, 1);
        armDCacheFlush(block, size);

        memset(wave, 0, sizeof(*wave));
        wave->data_raw = audio->ring;
        wave->size = audio->ring_size;
        wave->start_sample_offset =
            (s32)((index * OSL_BLOCK_CAPACITY) / frame_bytes);
        wave->end_sample_offset = wave->start_sample_offset +
                                  (s32)(size / frame_bytes);
        if (audrvVoiceAddWaveBuf(&audio->driver, audio->voice_id, wave)) {
          audio->wave_pending[index] = 1;
          audio->next_wave = (index + 1) % OSL_WAVEBUFS;
          audio->last_wave = index;
          audio->queued_frames += (u32)(size / frame_bytes);
          audio->tail_faded = 0;
          audio->fade_in_next = 0;
          audio->rebuffering = 0;
          audrvUpdate(&audio->driver);
        } else {
          audio->packet_head = (audio->packet_head + OSL_QUEUE_CAPACITY - 1) %
                               OSL_QUEUE_CAPACITY;
          audio->packets[audio->packet_head] = packet;
          audio->packet_count++;
        }
      }
    }

    /* Nothing follows the queued audio and it is about to end: fade its last
     * frames out so running dry does not click. If more audio arrives in time,
     * it fades back in. */
    if (!audio->tail_faded && !audio->packet_count && audio->bits == 16 &&
        audio->wave_pending[audio->last_wave]) {
      const u32 played =
          audrvVoiceGetPlayedSampleCount(&audio->driver, audio->voice_id);
      const u32 remaining = audio->queued_frames - played;
      if (remaining <= audio->sample_rate * OSL_TAIL_FADE_MS / 1000) {
        const AudioDriverWaveBuf *wave = &audio->wavebufs[audio->last_wave];
        const size_t frame_bytes = audio->channels * sizeof(int16_t);
        size_t frames = (size_t)(wave->end_sample_offset -
                                 wave->start_sample_offset);
        if (frames > OSL_FADE_FRAMES) frames = OSL_FADE_FRAMES;
        int16_t *tail = (int16_t *)((char *)audio->ring +
            (size_t)(wave->end_sample_offset - (s32)frames) * frame_bytes);
        fade_pcm16(tail, frames, audio->channels, 0);
        armDCacheFlush(tail, frames * frame_bytes);
        audio->tail_faded = 1;
        audio->fade_in_next = 1;
      }
    }
    mutexUnlock(&audio->lock);

    if (invoke_callback) {
      audio->callback_running = 1;
      queue_invoke_callback(audio);
      audio->callback_running = 0;
    }

    audrenWaitFrame();
  }
  return NULL;
}

static int microphone_result_is_timeout(Result result) {
  return R_VALUE(result) == R_VALUE(KERNELRESULT(TimedOut)) ||
         R_VALUE(result) ==
             R_VALUE(MAKERESULT(Module_Libnx, LibnxError_Timeout));
}

static void recorder_fifo_reset(OslAudioObject *audio) {
  audio->mic_fifo_head = 0;
  audio->mic_fifo_count = 0;
  audio->mic_resample_phase = 0;
}

static void recorder_fifo_drop(OslAudioObject *audio, size_t frames) {
  if (frames >= audio->mic_fifo_count) {
    audio->mic_fifo_head = 0;
    audio->mic_fifo_count = 0;
    return;
  }
  audio->mic_fifo_head =
      (audio->mic_fifo_head + frames) % OSL_MIC_FIFO_FRAMES;
  audio->mic_fifo_count -= frames;
}

static int recorder_resources_init(OslAudioObject *audio) {
  const size_t capture_size =
      OSL_MIC_CAPTURE_BUFFERS * OSL_MIC_CAPTURE_PAGE_SIZE;
  if (!audio->mic_capture_ring) {
    audio->mic_capture_ring = aligned_alloc(OSL_MIC_CAPTURE_PAGE_SIZE,
                                             capture_size);
    if (!audio->mic_capture_ring) return 0;
    memset(audio->mic_capture_ring, 0, capture_size);
  }
  if (!audio->mic_fifo) {
    audio->mic_fifo = calloc(OSL_MIC_FIFO_FRAMES * 2,
                             sizeof(*audio->mic_fifo));
    if (!audio->mic_fifo) return 0;
  }
  return 1;
}

static void recorder_backend_close(OslAudioObject *audio) {
  if (audio->mic_backend_active || audio->microphone.opened)
    switch_mic_close(&audio->microphone);
  audio->mic_backend_active = 0;
  audio->mic_timeout_count = 0;
  audio->mic_device_scan_tick = 0;
  recorder_fifo_reset(audio);
}

static void recorder_schedule_retry(OslAudioObject *audio) {
  recorder_backend_close(audio);
  audio->mic_retry_tick = armGetSystemTick() + armNsToTicks(OSL_MIC_RETRY_NS);
  microphone_set_status(OPENSLES_MIC_STATUS_UNAVAILABLE);
}

static int recorder_backend_init(OslAudioObject *audio) {
  if (!recorder_resources_init(audio)) {
    recorder_schedule_retry(audio);
    return 0;
  }
  recorder_backend_close(audio);
  Result result = switch_mic_open(&audio->microphone);
  if (R_FAILED(result)) {
    recorder_schedule_retry(audio);
    return 0;
  }

  const size_t data_size = OSL_MIC_CAPTURE_FRAMES *
                           audio->microphone.channel_count * sizeof(int16_t);
  for (unsigned index = 0; index < OSL_MIC_CAPTURE_BUFFERS; index++) {
    AudioInBuffer *buffer = &audio->mic_capture_buffers[index];
    memset(buffer, 0, sizeof(*buffer));
    buffer->buffer = (char *)audio->mic_capture_ring +
                     index * OSL_MIC_CAPTURE_PAGE_SIZE;
    buffer->buffer_size = OSL_MIC_CAPTURE_PAGE_SIZE;
    buffer->data_size = data_size;
  }

  result = switch_mic_start(&audio->microphone);
  if (R_FAILED(result)) {
    recorder_schedule_retry(audio);
    return 0;
  }
  for (unsigned index = 0; index < OSL_MIC_CAPTURE_BUFFERS; index++) {
    result = switch_mic_append(&audio->microphone,
                               &audio->mic_capture_buffers[index]);
    if (R_FAILED(result)) {
      recorder_schedule_retry(audio);
      return 0;
    }
  }

  recorder_fifo_reset(audio);
  audio->mic_retry_tick = 0;
  audio->mic_device_scan_tick =
      armGetSystemTick() + armNsToTicks(OSL_MIC_RETRY_NS);
  audio->mic_timeout_count = 0;
  audio->mic_backend_active = 1;
  microphone_set_status(OPENSLES_MIC_STATUS_ACTIVE);
  return 1;
}

static void recorder_fifo_append(OslAudioObject *audio,
                                 const AudioInBuffer *buffer) {
  if (!audio->mic_fifo || !buffer || !buffer->buffer) return;
  const unsigned channels = audio->microphone.channel_count;
  if (!channels || channels > 2) return;
  const size_t frame_bytes = channels * sizeof(int16_t);
  const size_t frames = buffer->data_size / frame_bytes;
  const int16_t *source = (const int16_t *)buffer->buffer;
  for (size_t frame = 0; frame < frames; frame++) {
    if (audio->mic_fifo_count == OSL_MIC_FIFO_FRAMES) {
      recorder_fifo_drop(audio, 1);
      audio->mic_resample_phase = 0;
    }
    const size_t destination =
        (audio->mic_fifo_head + audio->mic_fifo_count) %
        OSL_MIC_FIFO_FRAMES;
    const int16_t left = source[frame * channels];
    const int16_t right = channels == 2 ? source[frame * channels + 1] : left;
    audio->mic_fifo[destination * 2] = left;
    audio->mic_fifo[destination * 2 + 1] = right;
    audio->mic_fifo_count++;
  }
}

static int recorder_capture_once(OslAudioObject *audio, u64 timeout_ns) {
  AudioInBuffer *released = NULL;
  u32 released_count = 0;
  Result result = switch_mic_wait(&audio->microphone, &released,
                                  &released_count, timeout_ns);
  if (R_FAILED(result)) {
    if (microphone_result_is_timeout(result)) {
      if (++audio->mic_timeout_count >= OSL_MIC_STALE_TIMEOUTS)
        recorder_schedule_retry(audio);
      return 0;
    }
    recorder_schedule_retry(audio);
    return -1;
  }
  audio->mic_timeout_count = 0;
  if (!released || !released_count) return 0;

  recorder_fifo_append(audio, released);
  released->data_offset = 0;
  released->data_size = OSL_MIC_CAPTURE_FRAMES *
                        audio->microphone.channel_count * sizeof(int16_t);
  result = switch_mic_append(&audio->microphone, released);
  if (R_FAILED(result)) {
    recorder_schedule_retry(audio);
    return -1;
  }
  return 1;
}

static int16_t recorder_fifo_sample(const OslAudioObject *audio,
                                    size_t frame, unsigned channel) {
  const size_t index = (audio->mic_fifo_head + frame) % OSL_MIC_FIFO_FRAMES;
  return audio->mic_fifo[index * 2 + channel];
}

static int16_t interpolate_sample(int16_t first, int16_t second,
                                  uint32_t fraction) {
  const int64_t delta = (int32_t)second - (int32_t)first;
  return (int16_t)((int32_t)first +
                   (int32_t)((delta * fraction) >> 32));
}

static void recorder_write_sample(OslAudioObject *audio, void *destination,
                                  size_t frame, int16_t sample) {
  for (unsigned channel = 0; channel < audio->channels; channel++) {
    const size_t index = frame * audio->channels + channel;
    if (audio->bits == 8)
      ((int8_t *)destination)[index] = (int8_t)(sample >> 8);
    else
      ((int16_t *)destination)[index] = sample;
  }
}

static void recorder_fill_from_microphone(OslAudioObject *audio,
                                          const OslPacket *packet) {
  memset((void *)packet->data, 0, packet->size);
  const size_t frame_bytes = audio->channels * (audio->bits / 8);
  const size_t output_frames = frame_bytes ? packet->size / frame_bytes : 0;
  if (!output_frames || !audio->sample_rate ||
      !audio->microphone.sample_rate) return;

  const uint64_t step =
      ((uint64_t)audio->microphone.sample_rate << 32) / audio->sample_rate;
  if (!step) return;
  for (size_t frame = 0; frame < output_frames; frame++) {
    const uint64_t next_phase = audio->mic_resample_phase + step;
    const size_t advance = (size_t)(next_phase >> 32);
    const size_t needed = advance + 1 > 2 ? advance + 1 : 2;
    while (audio->mic_fifo_count < needed) {
      if (!audio->mic_backend_active ||
          recorder_capture_once(audio, OSL_MIC_PACKET_WAIT_NS) <= 0)
        return;
    }

    const uint32_t fraction = (uint32_t)audio->mic_resample_phase;
    const int16_t left = interpolate_sample(
        recorder_fifo_sample(audio, 0, 0),
        recorder_fifo_sample(audio, 1, 0), fraction);
    const int16_t right = interpolate_sample(
        recorder_fifo_sample(audio, 0, 1),
        recorder_fifo_sample(audio, 1, 1), fraction);
    const int16_t mono = (int16_t)(((int32_t)left + (int32_t)right) / 2);
    recorder_write_sample(audio, (void *)packet->data, frame, mono);

    recorder_fifo_drop(audio, advance);
    audio->mic_resample_phase = (uint32_t)next_phase;
  }
}

static void recorder_fill_silence(OslAudioObject *audio,
                                  const OslPacket *packet) {
  memset((void *)packet->data, 0, packet->size);
  const size_t frame_bytes = audio->channels * (audio->bits / 8);
  const size_t frames = frame_bytes ? packet->size / frame_bytes : 0;
  if (audio->sample_rate && frames)
    svcSleepThread((u64)frames * 1000000000ULL / audio->sample_rate);
}

static void recorder_wait_idle(OslAudioObject *audio, u64 timeout_ns) {
  mutexLock(&audio->lock);
  if (__atomic_load_n(&audio->running, __ATOMIC_ACQUIRE))
    condvarWaitTimeout(&audio->wake, &audio->lock, timeout_ns);
  mutexUnlock(&audio->lock);
}

static void *recorder_thread(void *argument) {
  OslAudioObject *audio = (OslAudioObject *)argument;
  pthr_ensure_fake_tls();
  pthr_pin_bg_core();

  while (__atomic_load_n(&audio->running, __ATOMIC_ACQUIRE)) {
    OslPacket packet = {0};
    int have_packet = 0;
    int can_record = 0;
    mutexLock(&audio->lock);
    can_record = !audio->suspended &&
                 audio->io_state == SL_RECORDSTATE_RECORDING;
    if (can_record && audio->packet_count) {
      packet = audio->packets[audio->packet_head];
      audio->packet_head = (audio->packet_head + 1) % OSL_QUEUE_CAPACITY;
      audio->packet_count--;
      have_packet = 1;
    }
    mutexUnlock(&audio->lock);

    const int enabled =
        __atomic_load_n(&g_microphone_enabled, __ATOMIC_ACQUIRE);
    const OpenSLESMicrophoneSource source =
        (OpenSLESMicrophoneSource)__atomic_load_n(
            &g_microphone_source, __ATOMIC_ACQUIRE);
    const unsigned generation = __atomic_load_n(
        &g_microphone_generation, __ATOMIC_ACQUIRE);
    if (generation != audio->mic_config_generation) {
      audio->mic_config_generation = generation;
      recorder_backend_close(audio);
      audio->mic_retry_tick = 0;
    }
    const int wants_external = enabled && can_record &&
                               source == OPENSLES_MIC_SOURCE_EXTERNAL;

    if (!wants_external) {
      if (audio->mic_backend_active || audio->microphone.opened)
        recorder_backend_close(audio);
      if (!enabled)
        microphone_set_status(OPENSLES_MIC_STATUS_DISABLED);
      else if (source == OPENSLES_MIC_SOURCE_SIMULATED)
        microphone_set_status(OPENSLES_MIC_STATUS_SIMULATED);
      else
        microphone_set_status(OPENSLES_MIC_STATUS_CONNECTING);
    } else if (!audio->mic_backend_active) {
      const uint64_t now = armGetSystemTick();
      if (!audio->mic_retry_tick || now >= audio->mic_retry_tick) {
        microphone_set_status(OPENSLES_MIC_STATUS_CONNECTING);
        recorder_backend_init(audio);
      }
    }

    if (have_packet) {
      if (wants_external && audio->mic_backend_active)
        recorder_fill_from_microphone(audio, &packet);
      else
        recorder_fill_silence(audio, &packet);
      mutexLock(&audio->lock);
      audio->completed_count++;
      mutexUnlock(&audio->lock);
      queue_invoke_callback(audio);
      continue;
    }

    if (wants_external && audio->mic_backend_active) {
      recorder_capture_once(audio, OSL_MIC_POLL_NS);
      if (audio->mic_fifo_count > OSL_MIC_LIVE_FIFO_FRAMES) {
        recorder_fifo_drop(audio,
                           audio->mic_fifo_count - OSL_MIC_LIVE_FIFO_FRAMES);
        audio->mic_resample_phase = 0;
      }
      const uint64_t now = armGetSystemTick();
      if (audio->mic_backend_active &&
          (!audio->mic_device_scan_tick || now >= audio->mic_device_scan_tick)) {
        bool external_available = false;
        const Result scan_result = switch_mic_external_available(
            &audio->microphone, &external_available);
        audio->mic_device_scan_tick = now + armNsToTicks(OSL_MIC_RETRY_NS);
        if (R_FAILED(scan_result) ||
            external_available != audio->microphone.external_device) {
          recorder_backend_close(audio);
          audio->mic_retry_tick = 0;
          microphone_set_status(OPENSLES_MIC_STATUS_CONNECTING);
        }
      }
    } else {
      recorder_wait_idle(audio, OSL_MIC_POLL_NS);
    }
  }
  recorder_backend_close(audio);
  return NULL;
}

static void audio_backend_shutdown(OslAudioObject *audio) {
  if (!audio) return;
  __atomic_store_n(&audio->running, 0, __ATOMIC_RELEASE);
  condvarWakeAll(&audio->wake);
  if (audio->thread_started) {
    pthread_join(audio->thread, NULL);
    audio->thread_started = 0;
  }
  if (audio->audren_inited) {
    audrenStopAudioRenderer();
    audrvVoiceDrop(&audio->driver, audio->voice_id);
    if (audio->mempool_id >= 0) {
      audrvMemPoolDetach(&audio->driver, audio->mempool_id);
      audrvMemPoolRemove(&audio->driver, audio->mempool_id);
    }
    audrvClose(&audio->driver);
    audrenExit();
    audio->audren_inited = 0;
  }
  free(audio->ring);
  audio->ring = NULL;
  recorder_backend_close(audio);
  free(audio->mic_capture_ring);
  audio->mic_capture_ring = NULL;
  free(audio->mic_fifo);
  audio->mic_fifo = NULL;
}

static SLresult object_realize(SLObjectItf self, SLboolean async) {
  (void)async;
  OslObject *object = (OslObject *)self;
  object->state = SL_OBJECT_STATE_REALIZED;
  return SL_RESULT_SUCCESS;
}

static SLresult object_resume(SLObjectItf self, SLboolean async) {
  return object_realize(self, async);
}

static SLresult object_get_state(SLObjectItf self, SLuint32 *state) {
  if (!state) return SL_RESULT_PARAMETER_INVALID;
  *state = ((OslObject *)self)->state;
  return SL_RESULT_SUCCESS;
}

static SLresult object_get_interface(SLObjectItf self, const SLInterfaceID iid,
                                     void *result) {
  if (!result || !iid) return SL_RESULT_PARAMETER_INVALID;
  OslObject *object = (OslObject *)self;
  OslInterface *interface = NULL;
  if (object->kind == OSL_OBJECT_ENGINE && iid == SL_IID_ENGINE)
    interface = &object->engine;
  else if (object->kind == OSL_OBJECT_PLAYER) {
    OslAudioObject *audio = (OslAudioObject *)object;
    if (iid == SL_IID_PLAY) interface = &audio->play;
    else if (iid == SL_IID_BUFFERQUEUE ||
             iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE) interface = &audio->queue;
    else if (iid == SL_IID_VOLUME) interface = &audio->volume;
  } else if (object->kind == OSL_OBJECT_RECORDER) {
    OslAudioObject *audio = (OslAudioObject *)object;
    if (iid == SL_IID_RECORD) interface = &audio->record;
    else if (iid == SL_IID_BUFFERQUEUE ||
             iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE) interface = &audio->queue;
  }
  if (!interface) return SL_RESULT_FEATURE_UNSUPPORTED;
  *(void **)result = (void *)&interface->vtable;
  return SL_RESULT_SUCCESS;
}

static SLresult object_register_callback(SLObjectItf self,
                                         slObjectCallback callback,
                                         void *context) {
  (void)self; (void)callback; (void)context;
  return SL_RESULT_SUCCESS;
}

static void object_abort(SLObjectItf self) { (void)self; }

static void object_destroy(SLObjectItf self) {
  OslObject *object = (OslObject *)self;
  if (object->kind == OSL_OBJECT_PLAYER || object->kind == OSL_OBJECT_RECORDER) {
    OslAudioObject *audio = (OslAudioObject *)object;
    if (g_player == audio) g_player = NULL;
    if (g_recorder == audio) g_recorder = NULL;
    audio_backend_shutdown(audio);
  }
  free(object);
}

static SLresult object_set_priority(SLObjectItf self, SLint32 priority,
                                    SLboolean preemptable) {
  (void)self; (void)priority; (void)preemptable;
  return SL_RESULT_SUCCESS;
}

static SLresult object_get_priority(SLObjectItf self, SLint32 *priority,
                                    SLboolean *preemptable) {
  (void)self;
  if (priority) *priority = 0;
  if (preemptable) *preemptable = SL_BOOLEAN_FALSE;
  return SL_RESULT_SUCCESS;
}

static SLresult object_set_loss(SLObjectItf self, SLint16 count,
                                SLInterfaceID *ids, SLboolean enabled) {
  (void)self; (void)count; (void)ids; (void)enabled;
  return SL_RESULT_SUCCESS;
}

static const struct SLObjectItf_ object_vtable = {
  object_realize, object_resume, object_get_state, object_get_interface,
  object_register_callback, object_abort, object_destroy, object_set_priority,
  object_get_priority, object_set_loss,
};

static void audio_format_from_source(OslAudioObject *audio,
                                     const SLDataSource *source,
                                     const SLDataSink *sink) {
  const SLDataFormat_PCM *format = NULL;
  if (source && source->pFormat) format = (const SLDataFormat_PCM *)source->pFormat;
  if (!format && sink && sink->pFormat) format = (const SLDataFormat_PCM *)sink->pFormat;
  audio->sample_rate = 44100;
  audio->channels = 2;
  audio->bits = 16;
  audio->queue_capacity = OSL_QUEUE_CAPACITY;

  /* Android's numBuffers limits the complete queue, including buffers already
   * handed to the device. Drastic relies on Enqueue returning
   * BUFFER_INSUFFICIENT when that total is full to arbitrate its generated
   * audio and internally generated-silence paths. */
  const void *locators[] = {
      source ? source->pLocator : NULL,
      sink ? sink->pLocator : NULL,
  };
  for (unsigned index = 0; index < 2; index++) {
    const SLuint32 *locator = (const SLuint32 *)locators[index];
    if (!locator)
      continue;
    if ((locator[0] == SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE ||
         locator[0] == SL_DATALOCATOR_BUFFERQUEUE) &&
        locator[1] > 0) {
      audio->queue_capacity = locator[1] < OSL_QUEUE_CAPACITY
                                  ? locator[1]
                                  : OSL_QUEUE_CAPACITY;
      break;
    }
  }
  if (format && format->formatType == SL_DATAFORMAT_PCM) {
    if (format->samplesPerSec) audio->sample_rate = format->samplesPerSec / 1000;
    if (format->numChannels >= 1 && format->numChannels <= 2)
      audio->channels = format->numChannels;
    if (format->bitsPerSample == 8 || format->bitsPerSample == 16)
      audio->bits = format->bitsPerSample;
  }
}

static OslAudioObject *audio_object_create(OslObjectKind kind) {
  OslAudioObject *audio = calloc(1, sizeof(*audio));
  if (!audio) return NULL;
  audio->base.object_vtable = &object_vtable;
  audio->base.kind = kind;
  audio->base.state = SL_OBJECT_STATE_UNREALIZED;
  audio->play.owner = &audio->base;
  audio->record.owner = &audio->base;
  audio->queue.owner = &audio->base;
  audio->volume.owner = &audio->base;
  audio->volume_mb = 0;
  audio->mempool_id = -1;
  mutexInit(&audio->lock);
  condvarInit(&audio->wake);
  return audio;
}

static SLresult play_set_state(SLPlayItf self, SLuint32 state) {
  OslAudioObject *audio = audio_from_self(self);
  mutexLock(&audio->lock);
  audio->io_state = state;
  if (audio->audren_inited) {
    audrvVoiceSetPaused(&audio->driver, audio->voice_id,
                        audio->suspended || state != SL_PLAYSTATE_PLAYING);
    audrvUpdate(&audio->driver);
  }
  condvarWakeAll(&audio->wake);
  mutexUnlock(&audio->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult play_get_state(SLPlayItf self, SLuint32 *state) {
  if (!state) return SL_RESULT_PARAMETER_INVALID;
  *state = audio_from_self(self)->io_state;
  return SL_RESULT_SUCCESS;
}

static SLresult play_get_duration(SLPlayItf self, SLmillisecond *value) {
  (void)self;
  if (value) *value = SL_TIME_UNKNOWN;
  return SL_RESULT_SUCCESS;
}

static SLresult play_get_position(SLPlayItf self, SLmillisecond *value) {
  (void)self;
  if (value) *value = 0;
  return SL_RESULT_SUCCESS;
}

static SLresult play_register(SLPlayItf self, slPlayCallback callback,
                              void *context) {
  (void)self; (void)callback; (void)context;
  return SL_RESULT_SUCCESS;
}

static SLresult play_set_mask(SLPlayItf self, SLuint32 value) {
  (void)self; (void)value; return SL_RESULT_SUCCESS;
}
static SLresult play_get_mask(SLPlayItf self, SLuint32 *value) {
  (void)self; if (value) *value = 0; return SL_RESULT_SUCCESS;
}
static SLresult play_set_marker(SLPlayItf self, SLmillisecond value) {
  (void)self; (void)value; return SL_RESULT_SUCCESS;
}
static SLresult play_clear_marker(SLPlayItf self) {
  (void)self; return SL_RESULT_SUCCESS;
}
static SLresult play_get_marker(SLPlayItf self, SLmillisecond *value) {
  (void)self; if (value) *value = SL_TIME_UNKNOWN; return SL_RESULT_SUCCESS;
}
static SLresult play_set_period(SLPlayItf self, SLmillisecond value) {
  (void)self; (void)value; return SL_RESULT_SUCCESS;
}
static SLresult play_get_period(SLPlayItf self, SLmillisecond *value) {
  (void)self; if (value) *value = 0; return SL_RESULT_SUCCESS;
}

static const struct SLPlayItf_ play_vtable = {
  play_set_state, play_get_state, play_get_duration, play_get_position,
  play_register, play_set_mask, play_get_mask, play_set_marker,
  play_clear_marker, play_get_marker, play_set_period, play_get_period,
};

static SLresult record_set_state(SLRecordItf self, SLuint32 state) {
  OslAudioObject *audio = audio_from_self(self);
  mutexLock(&audio->lock);
  audio->io_state = state;
  condvarWakeAll(&audio->wake);
  mutexUnlock(&audio->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult record_get_state(SLRecordItf self, SLuint32 *state) {
  if (!state) return SL_RESULT_PARAMETER_INVALID;
  OslAudioObject *audio = audio_from_self(self);
  mutexLock(&audio->lock);
  *state = audio->io_state;
  mutexUnlock(&audio->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult record_duration(SLRecordItf self, SLmillisecond value) {
  (void)self; (void)value; return SL_RESULT_SUCCESS;
}
static SLresult record_position(SLRecordItf self, SLmillisecond *value) {
  (void)self; if (value) *value = 0; return SL_RESULT_SUCCESS;
}
static SLresult record_register(SLRecordItf self, slRecordCallback callback,
                                void *context) {
  (void)self; (void)callback; (void)context; return SL_RESULT_SUCCESS;
}
static SLresult record_set_mask(SLRecordItf self, SLuint32 value) {
  (void)self; (void)value; return SL_RESULT_SUCCESS;
}
static SLresult record_get_mask(SLRecordItf self, SLuint32 *value) {
  (void)self; if (value) *value = 0; return SL_RESULT_SUCCESS;
}
static SLresult record_set_marker(SLRecordItf self, SLmillisecond value) {
  (void)self; (void)value; return SL_RESULT_SUCCESS;
}
static SLresult record_clear_marker(SLRecordItf self) {
  (void)self; return SL_RESULT_SUCCESS;
}
static SLresult record_get_marker(SLRecordItf self, SLmillisecond *value) {
  (void)self; if (value) *value = SL_TIME_UNKNOWN; return SL_RESULT_SUCCESS;
}
static SLresult record_set_period(SLRecordItf self, SLmillisecond value) {
  (void)self; (void)value; return SL_RESULT_SUCCESS;
}
static SLresult record_get_period(SLRecordItf self, SLmillisecond *value) {
  (void)self; if (value) *value = 0; return SL_RESULT_SUCCESS;
}

static const struct SLRecordItf_ record_vtable = {
  record_set_state, record_get_state, record_duration, record_position,
  record_register, record_set_mask, record_get_mask, record_set_marker,
  record_clear_marker, record_get_marker, record_set_period, record_get_period,
};

static SLresult queue_enqueue_common(OslAudioObject *audio, const void *buffer,
                                     SLuint32 size) {
  if (!buffer || !size) return SL_RESULT_PARAMETER_INVALID;
  /* Drastic's completion callback queues a whole buffer of silence when the
   * emulator has not delivered audio in time, and later counts that buffer's
   * completion as real audio, which makes its next real buffer overflow the
   * queue and get lost. The player rebuffers and fades by itself instead. */
  if (audio->callback_running && threadGetCurHandle() == audio->callback_thread)
    return SL_RESULT_SUCCESS;
  mutexLock(&audio->lock);
  const unsigned outstanding = queue_outstanding_locked(audio);
  if (outstanding >= audio->queue_capacity) {
    mutexUnlock(&audio->lock);
    return SL_RESULT_BUFFER_INSUFFICIENT;
  }
  audio->packets[audio->packet_tail].data = buffer;
  audio->packets[audio->packet_tail].size = size;
  audio->packet_tail = (audio->packet_tail + 1) % OSL_QUEUE_CAPACITY;
  audio->packet_count++;
  condvarWakeAll(&audio->wake);
  mutexUnlock(&audio->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult queue_enqueue(SLBufferQueueItf self, const void *buffer,
                              SLuint32 size) {
  return queue_enqueue_common(audio_from_self(self), buffer, size);
}

static SLresult queue_clear(SLBufferQueueItf self) {
  OslAudioObject *audio = audio_from_self(self);
  mutexLock(&audio->lock);
  audio->packet_head = audio->packet_tail = audio->packet_count = 0;
  if (audio->audren_inited) {
    audrvVoiceDrop(&audio->driver, audio->voice_id);
    memset(audio->wavebufs, 0, sizeof(audio->wavebufs));
    memset(audio->wave_pending, 0, sizeof(audio->wave_pending));
    player_voice_init(audio);
  }
  audio->queued_frames = 0;
  audio->tail_faded = 0;
  audio->fade_in_next = 1;
  audio->rebuffering = 0;
  mutexUnlock(&audio->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult queue_get_state(SLBufferQueueItf self,
                                SLBufferQueueState *state) {
  if (!state) return SL_RESULT_PARAMETER_INVALID;
  OslAudioObject *audio = audio_from_self(self);
  mutexLock(&audio->lock);
  unsigned in_flight = 0;
  for (unsigned index = 0; index < OSL_WAVEBUFS; index++)
    in_flight += audio->wave_pending[index] != 0;
  state->count = audio->packet_count + in_flight;
  state->playIndex = audio->completed_count;
  mutexUnlock(&audio->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult queue_register(SLBufferQueueItf self,
                               slBufferQueueCallback callback,
                               void *context) {
  OslAudioObject *audio = audio_from_self(self);
  audio->queue_callback = (void (*)(void *, void *))callback;
  audio->queue_context = context;
  return SL_RESULT_SUCCESS;
}

static const struct SLBufferQueueItf_ queue_vtable = {
  queue_enqueue, queue_clear, queue_get_state, queue_register,
};

static SLresult volume_set(SLVolumeItf self, SLmillibel level) {
  OslAudioObject *audio = audio_from_self(self);
  audio->volume_mb = level;
  player_update_gain(audio);
  return SL_RESULT_SUCCESS;
}
static SLresult volume_get(SLVolumeItf self, SLmillibel *level) {
  if (!level) return SL_RESULT_PARAMETER_INVALID;
  *level = audio_from_self(self)->volume_mb;
  return SL_RESULT_SUCCESS;
}
static SLresult volume_max(SLVolumeItf self, SLmillibel *level) {
  (void)self; if (level) *level = 0; return SL_RESULT_SUCCESS;
}
static SLresult volume_set_mute(SLVolumeItf self, SLboolean mute) {
  OslAudioObject *audio = audio_from_self(self);
  audio->muted = mute;
  player_update_gain(audio);
  return SL_RESULT_SUCCESS;
}
static SLresult volume_get_mute(SLVolumeItf self, SLboolean *mute) {
  if (!mute) return SL_RESULT_PARAMETER_INVALID;
  *mute = audio_from_self(self)->muted;
  return SL_RESULT_SUCCESS;
}
static SLresult volume_stereo_enable(SLVolumeItf self, SLboolean enabled) {
  (void)self; (void)enabled; return SL_RESULT_SUCCESS;
}
static SLresult volume_stereo_is_enabled(SLVolumeItf self, SLboolean *enabled) {
  (void)self; if (enabled) *enabled = SL_BOOLEAN_FALSE; return SL_RESULT_SUCCESS;
}
static SLresult volume_stereo_set(SLVolumeItf self, SLpermille value) {
  (void)self; (void)value; return SL_RESULT_SUCCESS;
}
static SLresult volume_stereo_get(SLVolumeItf self, SLpermille *value) {
  (void)self; if (value) *value = 0; return SL_RESULT_SUCCESS;
}

static const struct SLVolumeItf_ volume_vtable = {
  volume_set, volume_get, volume_max, volume_set_mute, volume_get_mute,
  volume_stereo_enable, volume_stereo_is_enabled, volume_stereo_set,
  volume_stereo_get,
};

static SLresult engine_create_player(SLEngineItf self, SLObjectItf *result,
                                     SLDataSource *source, SLDataSink *sink,
                                     SLuint32 count, const SLInterfaceID *ids,
                                     const SLboolean *required) {
  (void)self; (void)count; (void)ids; (void)required;
  if (!result) return SL_RESULT_PARAMETER_INVALID;
  *result = NULL;
  if (g_player) return SL_RESULT_RESOURCE_ERROR;
  OslAudioObject *audio = audio_object_create(OSL_OBJECT_PLAYER);
  if (!audio) return SL_RESULT_MEMORY_FAILURE;
  audio_format_from_source(audio, source, sink);
  if (audio->bits != 16) audio->bits = 16;
  audio->play.vtable = &play_vtable;
  audio->queue.vtable = &queue_vtable;
  audio->volume.vtable = &volume_vtable;
  audio->io_state = SL_PLAYSTATE_STOPPED;
  if (!player_backend_init(audio)) {
    free(audio);
    return SL_RESULT_RESOURCE_ERROR;
  }
  audio->running = 1;
  if (pthread_create(&audio->thread, NULL, player_thread, audio) != 0) {
    audio_backend_shutdown(audio);
    free(audio);
    return SL_RESULT_RESOURCE_ERROR;
  }
  audio->thread_started = 1;
  g_player = audio;
  *result = (SLObjectItf)&audio->base.object_vtable;
  return SL_RESULT_SUCCESS;
}

static SLresult engine_create_recorder(SLEngineItf self, SLObjectItf *result,
                                       SLDataSource *source, SLDataSink *sink,
                                       SLuint32 count, const SLInterfaceID *ids,
                                       const SLboolean *required) {
  (void)self; (void)count; (void)ids; (void)required;
  if (!result) return SL_RESULT_PARAMETER_INVALID;
  if (g_recorder) return SL_RESULT_RESOURCE_ERROR;
  OslAudioObject *audio = audio_object_create(OSL_OBJECT_RECORDER);
  if (!audio) return SL_RESULT_MEMORY_FAILURE;
  audio_format_from_source(audio, source, sink);
  audio->record.vtable = &record_vtable;
  audio->queue.vtable = &queue_vtable;
  audio->io_state = SL_RECORDSTATE_STOPPED;
  audio->running = 1;
  if (pthread_create(&audio->thread, NULL, recorder_thread, audio) != 0) {
    free(audio);
    return SL_RESULT_RESOURCE_ERROR;
  }
  audio->thread_started = 1;
  g_recorder = audio;
  *result = (SLObjectItf)&audio->base.object_vtable;
  return SL_RESULT_SUCCESS;
}

static SLresult engine_create_output_mix(SLEngineItf self, SLObjectItf *result,
                                         SLuint32 count,
                                         const SLInterfaceID *ids,
                                         const SLboolean *required) {
  (void)self; (void)count; (void)ids; (void)required;
  if (!result) return SL_RESULT_PARAMETER_INVALID;
  OslObject *object = calloc(1, sizeof(*object));
  if (!object) return SL_RESULT_MEMORY_FAILURE;
  object->object_vtable = &object_vtable;
  object->kind = OSL_OBJECT_OUTPUT_MIX;
  object->state = SL_OBJECT_STATE_UNREALIZED;
  *result = (SLObjectItf)&object->object_vtable;
  return SL_RESULT_SUCCESS;
}

static SLresult engine_unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }
static SLresult engine_num_interfaces(SLEngineItf self, SLuint32 object_id,
                                      SLuint32 *count) {
  (void)self; (void)object_id; if (count) *count = 0; return SL_RESULT_SUCCESS;
}
static SLresult engine_supported_interface(SLEngineItf self, SLuint32 object_id,
                                           SLuint32 index,
                                           SLInterfaceID *iid) {
  (void)self; (void)object_id; (void)index; (void)iid;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
static SLresult engine_num_extensions(SLEngineItf self, SLuint32 *count) {
  (void)self; if (count) *count = 0; return SL_RESULT_SUCCESS;
}
static SLresult engine_get_extension(SLEngineItf self, SLuint32 index,
                                     SLchar *name, SLint16 *length) {
  (void)self; (void)index; (void)name; (void)length;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
static SLresult engine_extension_supported(SLEngineItf self,
                                           const SLchar *name,
                                           SLboolean *supported) {
  (void)self; (void)name; if (supported) *supported = SL_BOOLEAN_FALSE;
  return SL_RESULT_SUCCESS;
}

static const struct SLEngineItf_ engine_vtable = {
  (void *)engine_unsupported,
  (void *)engine_unsupported,
  engine_create_player,
  engine_create_recorder,
  (void *)engine_unsupported,
  (void *)engine_unsupported,
  (void *)engine_unsupported,
  engine_create_output_mix,
  (void *)engine_unsupported,
  (void *)engine_unsupported,
  engine_num_interfaces,
  engine_supported_interface,
  engine_num_extensions,
  engine_get_extension,
  engine_extension_supported,
};

SLresult SLAPIENTRY slCreateEngine(SLObjectItf *result, SLuint32 option_count,
                                   const SLEngineOption *options,
                                   SLuint32 interface_count,
                                   const SLInterfaceID *interface_ids,
                                   const SLboolean *required) {
  (void)option_count; (void)options; (void)interface_count;
  (void)interface_ids; (void)required;
  if (!result) return SL_RESULT_PARAMETER_INVALID;
  OslObject *engine = calloc(1, sizeof(*engine));
  if (!engine) return SL_RESULT_MEMORY_FAILURE;
  engine->object_vtable = &object_vtable;
  engine->kind = OSL_OBJECT_ENGINE;
  engine->state = SL_OBJECT_STATE_UNREALIZED;
  engine->engine.vtable = &engine_vtable;
  engine->engine.owner = engine;
  *result = (SLObjectItf)&engine->object_vtable;
  return SL_RESULT_SUCCESS;
}

void opensles_shutdown(void) {
  if (g_recorder) {
    OslAudioObject *audio = g_recorder;
    g_recorder = NULL;
    audio_backend_shutdown(audio);
  }
  if (g_player) {
    OslAudioObject *audio = g_player;
    g_player = NULL;
    audio_backend_shutdown(audio);
  }
}
