import {
  ContextState,
  PeriodicWaveConstraints,
  AudioContextOptions,
} from '../types';
import { InvalidAccessError, NotSupportedError } from '../errors';
import BaseAudioContext from './BaseAudioContext';
import AnalyserNode from './AnalyserNode';
import AudioDestinationNode from './AudioDestinationNode';
import AudioBuffer from './AudioBuffer';
import AudioBufferSourceNode from './AudioBufferSourceNode';
import BiquadFilterNode from './BiquadFilterNode';
import GainNode from './GainNode';
import OscillatorNode from './OscillatorNode';
import PeriodicWave from './PeriodicWave';
import StereoPannerNode from './StereoPannerNode';
import StretcherNode from './StretcherNode';

import { globalWasmPromise, globalTag } from './custom/LoadCustomWasm';

export default class AudioContext implements BaseAudioContext {
  readonly context: globalThis.AudioContext;

  readonly destination: AudioDestinationNode;
  readonly sampleRate: number;

  constructor(options?: AudioContextOptions) {
    if (options && (options.sampleRate < 8000 || options.sampleRate > 96000)) {
      throw new NotSupportedError(
        `The provided sampleRate is not supported: ${options.sampleRate}`
      );
    }

    this.context = new window.AudioContext(options);

    this.sampleRate = this.context.sampleRate;
    this.destination = new AudioDestinationNode(this, this.context.destination);
  }

  public get currentTime(): number {
    return this.context.currentTime;
  }

  public get state(): ContextState {
    return this.context.state as ContextState;
  }

  createOscillator(): OscillatorNode {
    return new OscillatorNode(this, this.context.createOscillator());
  }

  createGain(): GainNode {
    return new GainNode(this, this.context.createGain());
  }

  createStereoPanner(): StereoPannerNode {
    return new StereoPannerNode(this, this.context.createStereoPanner());
  }

  createBiquadFilter(): BiquadFilterNode {
    return new BiquadFilterNode(this, this.context.createBiquadFilter());
  }

  createBufferSource(): AudioBufferSourceNode {
    return new AudioBufferSourceNode(this, this.context.createBufferSource());
  }

  createBuffer(
    numOfChannels: number,
    length: number,
    sampleRate: number
  ): AudioBuffer {
    if (numOfChannels < 1 || numOfChannels >= 32) {
      throw new NotSupportedError(
        `The number of channels provided (${numOfChannels}) is outside the range [1, 32]`
      );
    }

    if (length <= 0) {
      throw new NotSupportedError(
        `The number of frames provided (${length}) is less than or equal to the minimum bound (0)`
      );
    }

    if (sampleRate < 8000 || sampleRate > 96000) {
      throw new NotSupportedError(
        `The sample rate provided (${sampleRate}) is outside the range [8000, 96000]`
      );
    }

    return new AudioBuffer(
      this.context.createBuffer(numOfChannels, length, sampleRate)
    );
  }

  createPeriodicWave(
    real: number[],
    imag: number[],
    constraints?: PeriodicWaveConstraints
  ): PeriodicWave {
    if (real.length !== imag.length) {
      throw new InvalidAccessError(
        `The lengths of the real (${real.length}) and imaginary (${imag.length}) arrays must match.`
      );
    }

    return new PeriodicWave(
      this.context.createPeriodicWave(real, imag, constraints)
    );
  }

  createAnalyser(): AnalyserNode {
    return new AnalyserNode(this, this.context.createAnalyser());
  }

  async createStretcher(): Promise<StretcherNode> {
    await globalWasmPromise;

    const wasmStretch = await window[globalTag](this.context);

    return new StretcherNode(this, wasmStretch);
  }

  async decodeAudioDataSource(source: string): Promise<AudioBuffer> {
    const arrayBuffer = await fetch(source).then((response) =>
      response.arrayBuffer()
    );

    return new AudioBuffer(await this.context.decodeAudioData(arrayBuffer));
  }

  async close(): Promise<void> {
    await this.context.close();
  }

  async resume(): Promise<void> {
    await this.context.resume();
  }

  async suspend(): Promise<void> {
    await this.context.suspend();
  }
}
