class PcmCaptureProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.targetSampleRate = options.processorOptions?.targetSampleRate ?? 16000;
    this.frameSamples = options.processorOptions?.frameSamples ?? 320;
    this.frame = new Int16Array(this.frameSamples);
    this.frameOffset = 0;
    this.phase = 0;
    this.accumulator = 0;
    this.accumulatorSamples = 0;
  }

  pushSample(sample) {
    const clamped = Math.max(-1, Math.min(1, sample));
    this.frame[this.frameOffset] = clamped < 0
      ? Math.round(clamped * 0x8000)
      : Math.round(clamped * 0x7fff);
    this.frameOffset += 1;
    if (this.frameOffset !== this.frameSamples) return;

    const packet = this.frame.buffer;
    this.port.postMessage(packet, [packet]);
    this.frame = new Int16Array(this.frameSamples);
    this.frameOffset = 0;
  }

  process(inputs) {
    const input = inputs[0]?.[0];
    if (!input) return true;

    if (sampleRate === this.targetSampleRate) {
      for (const sample of input) this.pushSample(sample);
      return true;
    }

    for (const sample of input) {
      this.accumulator += sample;
      this.accumulatorSamples += 1;
      this.phase += this.targetSampleRate;
      if (this.phase >= sampleRate) {
        this.pushSample(this.accumulator / this.accumulatorSamples);
        this.phase -= sampleRate;
        this.accumulator = 0;
        this.accumulatorSamples = 0;
      }
    }
    return true;
  }
}

registerProcessor("pcm-capture", PcmCaptureProcessor);
