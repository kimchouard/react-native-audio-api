/*
 * Copyright (C) 2012 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <audioapi/core/Constants.h>
#include <audioapi/core/effects/PeriodicWave.h>
#include <audioapi/dsp/FFTFrame.h>
#include <audioapi/dsp/VectorMath.h>

constexpr unsigned NumberOfOctaveBands = 3;
constexpr float CentsPerRange = 1200.0f / NumberOfOctaveBands;
constexpr float interpolate2Point = 0.3;
constexpr float interpolate3Point = 0.16;

namespace audioapi {
PeriodicWave::PeriodicWave(float sampleRate, bool disableNormalization)
    : sampleRate_(sampleRate), disableNormalization_(disableNormalization) {
  numberOfRanges_ = static_cast<int>(round(
      NumberOfOctaveBands * log2f(static_cast<float>(getPeriodicWaveSize()))));
  auto nyquistFrequency = sampleRate_ / 2;
  lowestFundamentalFrequency_ = static_cast<float>(nyquistFrequency) /
      static_cast<float>(getMaxNumberOfPartials());
  scale_ = static_cast<float>(getPeriodicWaveSize()) /
      static_cast<float>(sampleRate_);
  bandLimitedTables_ = new float *[numberOfRanges_];
}

PeriodicWave::PeriodicWave(
    float sampleRate,
    audioapi::OscillatorType type,
    bool disableNormalization)
    : PeriodicWave(sampleRate, disableNormalization) {
  this->generateBasicWaveForm(type);
}

PeriodicWave::PeriodicWave(
    float sampleRate,
    float *real,
    float *imaginary,
    int length,
    bool disableNormalization)
    : PeriodicWave(sampleRate, disableNormalization) {
  createBandLimitedTables(real, imaginary, length);
}

int PeriodicWave::getPeriodicWaveSize() const {
  if (sampleRate_ <= 24000) {
    return 2048;
  }

  if (sampleRate_ <= 88200) {
    return 4096;
  }

  return 16384;
}

float PeriodicWave::getScale() const {
  return scale_;
}

float PeriodicWave::getSample(
    float fundamentalFrequency,
    float phase,
    float phaseIncrement) {
  float *lowerWaveData = nullptr;
  float *higherWaveData = nullptr;

  auto interpolationFactor = getWaveDataForFundamentalFrequency(
      fundamentalFrequency, lowerWaveData, higherWaveData);

  return doInterpolation(
      phase,
      phaseIncrement,
      interpolationFactor,
      lowerWaveData,
      higherWaveData);
}

int PeriodicWave::getMaxNumberOfPartials() const {
  return getPeriodicWaveSize() / 2;
}

int PeriodicWave::getNumberOfPartialsPerRange(int rangeIndex) const {
  // Number of cents below nyquist where we cull partials.
  auto centsToCull = static_cast<float>(rangeIndex) * CentsPerRange;

  // A value from 0 -> 1 representing what fraction of the partials to keep.
  auto cullingScale = std::powf(2, -centsToCull / 1200);

  // The very top range will have all the partials culled.
  int numberOfPartials =
      floor(static_cast<float>(getMaxNumberOfPartials()) * cullingScale);

  return numberOfPartials;
}

void PeriodicWave::generateBasicWaveForm(OscillatorType type) {
  auto fftSize = getPeriodicWaveSize();
  /*
   * For real-valued time-domain signals, the FFT outputs a Hermitian symmetric
   * sequence (where the positive frequencies are the complex conjugate of the
   * negative ones). This symmetry implies that real signals have mirrored
   * frequency components. In such scenarios, all 'real' frequency information
   * is contained in the first half of the transform, and altering parts such as
   * real and imaginary can finely shape which harmonic content is retained or
   * discarded.
   */
  auto halfSize = fftSize / 2;

  auto *real = new float[halfSize];
  auto *imaginary = new float[halfSize];

  // Reset Direct Current (DC) component. First element of frequency domain
  // representation - c0. https://math24.net/complex-form-fourier-series.html
  real[0] = 0.0f;
  imaginary[0] = 0.0f;

  for (int i = 1; i < halfSize; i++) {
    // All waveforms are odd functions with a positive slope at time 0.
    // Hence the coefficients for cos() are always 0.

    // Formulas for Fourier coefficients:
    // https://mathworld.wolfram.com/FourierSeries.html

    // Coefficient for sin()
    float b;

    auto piFactor = 1.0f / (PI * static_cast<float>(i));

    switch (type) {
      case OscillatorType::SINE:
        b = (i == 1) ? 1.0f : 0.0f;
        break;
      case OscillatorType::SQUARE:
        // https://mathworld.wolfram.com/FourierSeriesSquareWave.html
        b = ((i & 1) == 1) ? 4 * piFactor : 0.0f;
        break;
      case OscillatorType::SAWTOOTH:
        // https://mathworld.wolfram.com/FourierSeriesSawtoothWave.html - our
        // Function differs from this one, but coefficients calculation looks
        // similar. our function - f(x) = 2(x / (2 * pi) - floor(x / (2 * pi) +
        // 0.5)));
        // https://www.wolframalpha.com/input?i=2%28x+%2F+%282+*+pi%29+-+floor%28x+%2F+%282+*+pi%29+%2B+0.5%29%29%29%3B
        b = 2 * piFactor * ((i & 1) == 1 ? 1.0f : -1.0f);
        break;
      case OscillatorType::TRIANGLE:
        // https://mathworld.wolfram.com/FourierSeriesTriangleWave.html
        if ((i & 1) == 1) {
          b = 8.0f * piFactor * piFactor * ((i & 3) == 1 ? 1.0f : -1.0f);
        } else {
          b = 0.0f;
        }
        break;
      case OscillatorType::CUSTOM:
        throw std::invalid_argument("Custom waveforms are not supported.");
    }

    real[i] = 0.0f;
    imaginary[i] = b;
  }

  createBandLimitedTables(real, imaginary, halfSize);
}

void PeriodicWave::createBandLimitedTables(
    const float *realData,
    const float *imaginaryData,
    int size) {
  float normalizationFactor = 0.5f;

  auto fftSize = getPeriodicWaveSize();
  auto halfSize = fftSize / 2;

  size = std::min(size, halfSize);

  for (int rangeIndex = 0; rangeIndex < numberOfRanges_; rangeIndex++) {
    FFTFrame fftFrame(fftSize);

    auto *realFFTFrameData = new float[fftSize];
    auto *imaginaryFFTFrameData = new float[fftSize];

    // copy real and imaginary data to the FFT frame and scale it
    VectorMath::multiplyByScalar(
        realData, static_cast<float>(fftSize), realFFTFrameData, size);
    VectorMath::multiplyByScalar(
        imaginaryData,
        -static_cast<float>(fftSize),
        imaginaryFFTFrameData,
        size);

    // Find the starting partial where we should start culling.
    // We need to clear out the highest frequencies to band-limit the waveform.
    auto numberOfPartials = getNumberOfPartialsPerRange(rangeIndex);

    // Clamp the size to the number of partials.
    auto clampedSize = std::min(size, numberOfPartials);
    if (clampedSize < halfSize) {
      // Zero out the higher frequencies for certain range.
      std::fill(
          realFFTFrameData + clampedSize, realFFTFrameData + halfSize, 0.0f);
      std::fill(
          imaginaryFFTFrameData + clampedSize,
          imaginaryFFTFrameData + halfSize,
          0.0f);
    }

    // Zero out the DC and nquist components.
    realFFTFrameData[0] = 0.0f;
    imaginaryFFTFrameData[0] = 0.0f;

    bandLimitedTables_[rangeIndex] = new float[fftSize];

    // Perform the inverse FFT to get the time domain representation of the
    // band-limited waveform.
    fftFrame.doInverseFFT(
        bandLimitedTables_[rangeIndex],
        realFFTFrameData,
        imaginaryFFTFrameData);

    if (!disableNormalization_ && rangeIndex == 0) {
      float maxValue =
          VectorMath::maximumMagnitude(bandLimitedTables_[rangeIndex], fftSize);
      if (maxValue != 0) {
        normalizationFactor = 1.0f / maxValue;
      }
    }

    delete[] realFFTFrameData;
    delete[] imaginaryFFTFrameData;

    VectorMath::multiplyByScalar(
        bandLimitedTables_[rangeIndex],
        normalizationFactor,
        bandLimitedTables_[rangeIndex],
        fftSize);
  }
}

float PeriodicWave::getWaveDataForFundamentalFrequency(
    float fundamentalFrequency,
    float *&lowerWaveData,
    float *&higherWaveData) {
  // negative frequencies are allowed and will be treated as positive.
  fundamentalFrequency = std::fabs(fundamentalFrequency);

  // calculating lower and higher range index for the given fundamental
  // frequency.
  float ratio = fundamentalFrequency > 0
      ? fundamentalFrequency / lowestFundamentalFrequency_
      : 0.5f;
  float centsAboveLowestFrequency = log2f(ratio) * 1200;

  float pitchRange = 1 + centsAboveLowestFrequency / CentsPerRange;

  pitchRange =
      std::clamp(pitchRange, 0.0f, static_cast<float>(numberOfRanges_ - 1));

  int lowerRangeIndex = static_cast<int>(pitchRange);
  int higherRangeIndex = lowerRangeIndex < numberOfRanges_ - 1
      ? lowerRangeIndex + 1
      : lowerRangeIndex;

  // get the wave data for the lower and higher range index.
  lowerWaveData = bandLimitedTables_[lowerRangeIndex];
  higherWaveData = bandLimitedTables_[higherRangeIndex];

  // calculate the interpolation factor between the lower and higher range data.
  return pitchRange - static_cast<float>(lowerRangeIndex);
}

float PeriodicWave::doInterpolation(
    float phase,
    float phaseIncrement,
    float waveTableInterpolationFactor,
    const float *lowerWaveData,
    const float *higherWaveData) const {
  float lowerWaveDataSample = 0;
  float higherWaveDataSample = 0;

  // We use linear, 3-point Lagrange, or 5-point Lagrange interpolation based on
  // the value of phase increment. https://dlmf.nist.gov/3.3#ii

  int index = static_cast<int>(phase);
  auto factor = phase - static_cast<float>(index);

  if (phaseIncrement >= interpolate2Point) { // linear interpolation
    int indices[2];

    for (int i = 0; i < 2; i++) {
      indices[i] = (index + i) &
          (getPeriodicWaveSize() -
           1); // more efficient alternative to % getPeriodicWaveSize()
    }

    auto lowerWaveDataSample1 = lowerWaveData[indices[0]];
    auto lowerWaveDataSample2 = lowerWaveData[indices[1]];
    auto higherWaveDataSample1 = higherWaveData[indices[0]];
    auto higherWaveDataSample2 = higherWaveData[indices[1]];

    lowerWaveDataSample =
        (1 - factor) * lowerWaveDataSample1 + factor * lowerWaveDataSample2;
    higherWaveDataSample =
        (1 - factor) * higherWaveDataSample1 + factor * higherWaveDataSample2;
  } else if (phaseIncrement >= interpolate3Point) { // 3-point Lagrange
                                                    // interpolation
    int indices[3];

    for (int i = 0; i < 3; i++) {
      indices[i] = (index + i - 1) & (getPeriodicWaveSize() - 1);
    }

    float A[3];

    A[0] = factor * (factor - 1) / 2;
    A[1] = 1 - factor * factor;
    A[2] = factor * (factor + 1) / 2;

    for (int i = 0; i < 3; i++) {
      lowerWaveDataSample += lowerWaveData[indices[i]] * A[i];
      higherWaveDataSample += higherWaveData[indices[i]] * A[i];
    }
  } else { // 5-point Lagrange interpolation
    int indices[5];

    for (int i = 0; i < 5; i++) {
      indices[i] = (index + i - 2) & (getPeriodicWaveSize() - 1);
    }

    float A[5];

    A[0] = factor * (factor * factor - 1) * (factor - 2) / 24;
    A[1] = -factor * (factor - 1) * (factor * factor - 4) / 6;
    A[2] = (factor * factor - 1) * (factor * factor - 4) / 4;
    A[3] = -factor * (factor + 1) * (factor * factor - 4) / 6;
    A[4] = factor * (factor * factor - 1) * (factor + 2) / 24;

    for (int i = 0; i < 5; i++) {
      lowerWaveDataSample += lowerWaveData[indices[i]] * A[i];
      higherWaveDataSample += higherWaveData[indices[i]] * A[i];
    }
  }

  return (1 - waveTableInterpolationFactor) * higherWaveDataSample +
      waveTableInterpolationFactor * lowerWaveDataSample;
}
} // namespace audioapi
