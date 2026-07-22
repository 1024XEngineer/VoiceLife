#include <AudioToolbox/AudioToolbox.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUFFER_COUNT 8
#define READ_SIZE 8192

typedef struct {
  dispatch_semaphore_t available_buffers;
} PlayerState;

static void output_callback(
  void *user_data,
  AudioQueueRef queue,
  AudioQueueBufferRef buffer
) {
  (void)queue;
  (void)buffer;
  PlayerState *state = (PlayerState *)user_data;
  dispatch_semaphore_signal(state->available_buffers);
}

static void fail_status(const char *operation, OSStatus status) {
  fprintf(stderr, "%s失败，状态码：%d\n", operation, (int)status);
  exit(1);
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "用法：voicelife-pcm-stream-player <sample-rate> <channels>\n");
    return 1;
  }

  const double sample_rate = strtod(argv[1], NULL);
  const uint32_t channels = (uint32_t)strtoul(argv[2], NULL, 10);
  if (sample_rate <= 0 || channels == 0 || channels > 2) {
    fprintf(stderr, "无效的 PCM 音频参数\n");
    return 1;
  }

  const uint32_t bytes_per_frame = channels * sizeof(int16_t);
  AudioStreamBasicDescription format = {0};
  format.mSampleRate = sample_rate;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
  format.mBytesPerPacket = bytes_per_frame;
  format.mFramesPerPacket = 1;
  format.mBytesPerFrame = bytes_per_frame;
  format.mChannelsPerFrame = channels;
  format.mBitsPerChannel = 16;

  PlayerState state = {
    .available_buffers = dispatch_semaphore_create(BUFFER_COUNT),
  };
  AudioQueueRef queue = NULL;
  OSStatus status = AudioQueueNewOutput(
    &format,
    output_callback,
    &state,
    NULL,
    NULL,
    0,
    &queue
  );
  if (status != noErr) fail_status("创建音频队列", status);

  AudioQueueBufferRef buffers[BUFFER_COUNT] = {0};
  const uint32_t buffer_capacity = READ_SIZE + bytes_per_frame;
  for (size_t index = 0; index < BUFFER_COUNT; index += 1) {
    status = AudioQueueAllocateBuffer(queue, buffer_capacity, &buffers[index]);
    if (status != noErr) fail_status("分配音频缓冲区", status);
  }

  uint8_t input[READ_SIZE + 8] = {0};
  size_t remainder_size = 0;
  size_t next_buffer = 0;
  int started = 0;

  while (1) {
    const ssize_t bytes_read = read(
      STDIN_FILENO,
      input + remainder_size,
      READ_SIZE
    );
    if (bytes_read == 0) break;
    if (bytes_read < 0) {
      if (errno == EINTR) continue;
      fprintf(stderr, "读取 PCM 数据失败：%s\n", strerror(errno));
      AudioQueueDispose(queue, true);
      return 1;
    }

    const size_t total_size = remainder_size + (size_t)bytes_read;
    const size_t playable_size = total_size - total_size % bytes_per_frame;
    if (playable_size == 0) {
      remainder_size = total_size;
      continue;
    }

    dispatch_semaphore_wait(state.available_buffers, DISPATCH_TIME_FOREVER);
    AudioQueueBufferRef buffer = buffers[next_buffer];
    memcpy(buffer->mAudioData, input, playable_size);
    buffer->mAudioDataByteSize = (UInt32)playable_size;
    status = AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
    if (status != noErr) fail_status("提交音频缓冲区", status);

    next_buffer = (next_buffer + 1) % BUFFER_COUNT;
    remainder_size = total_size - playable_size;
    if (remainder_size > 0) {
      memmove(input, input + playable_size, remainder_size);
    }

    if (!started) {
      status = AudioQueueStart(queue, NULL);
      if (status != noErr) fail_status("启动音频播放", status);
      started = 1;
    }
  }

  if (remainder_size != 0) {
    fprintf(stderr, "收到未对齐的 PCM 音频数据\n");
    AudioQueueDispose(queue, true);
    return 1;
  }

  for (size_t index = 0; index < BUFFER_COUNT; index += 1) {
    dispatch_semaphore_wait(state.available_buffers, DISPATCH_TIME_FOREVER);
  }
  AudioQueueStop(queue, false);
  AudioQueueDispose(queue, true);
  return 0;
}
