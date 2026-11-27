// The master equalizer of the mix: 15 bands, the same centres, width (one octave) and presets as the one in hTV (and
// HaikuSuperMusicThingy). Used by hrecord (which applies it) and hrecord_mixer (which sets it).
#ifndef HRECORD_EQ_H
#define HRECORD_EQ_H

#include <math.h>

static const int kEqBands = 15;
static const float kEqRange = 15.0f;		// dB, each way

static const float kEqFrequencies[kEqBands] = {
	50, 100, 156, 220, 311, 440, 622, 880, 1250, 1750, 2500, 3500, 5000, 10000, 20000
};

static const char* const kEqFrequencyLabels[kEqBands] = {
	"50", "100", "156", "220", "311", "440", "622", "880", "1k2", "1k7", "2k5", "3k5", "5k", "10k", "20k"
};

static const int kEqPresets = 4;
static const char* const kEqPresetNames[kEqPresets] = { "Flat", "Rock", "Jazz", "Bass Boost" };
static const float kEqPresetValues[kEqPresets][kEqBands] = {
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 4.0, 3.5, 3.0, 2.5, 2.0, 1.0, -1.0, -1.0, 0.0, 1.0, 1.5, 2.0, 2.5, 3.5, 4.0 },
	{ 3.0, 2.5, 2.0, 1.5, 1.0, 2.0, -1.0, -1.0, -0.5, 0.0, 0.5, 1.0, 1.5, 2.5, 3.0 },
	{ 11.0, 9.0, 4.0, 2.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 3.0, 4.0, 7.0, 9.0 }
};


// Fifteen peaking filters in a row, on interleaved stereo floats. Nothing in it allocates or blocks.
class Equalizer {
public:
	// The filters for these gains (dB) at this sample rate. What the filters remember is kept, so that a change makes no click.
	void Set(const float* gains, float rate)
	{
		for (int band = 0; band < kEqBands; band++) {
			Band& b = fBands[band];
			// (a band at or over half the sample rate cannot be made: it is left alone)
			if (kEqFrequencies[band] >= rate * 0.49f) {
				b.b0 = 1; b.b1 = b.b2 = b.a1 = b.a2 = 0;
				continue;
			}
			const double w = 2.0 * M_PI * kEqFrequencies[band] / rate;
			const double amplitude = pow(10.0, gains[band] / 40.0);
			const double alpha = sin(w) * sinh(log(2.0) / 2.0 * w / sin(w));
			const double a0 = 1.0 + alpha / amplitude;
			b.b0 = (1.0 + alpha * amplitude) / a0;
			b.b1 = -2.0 * cos(w) / a0;
			b.b2 = (1.0 - alpha * amplitude) / a0;
			b.a1 = b.b1;
			b.a2 = (1.0 - alpha / amplitude) / a0;
		}
	}

	void Reset()
	{
		for (int band = 0; band < kEqBands; band++) {
			for (int side = 0; side < 2; side++)
				fBands[band].z1[side] = fBands[band].z2[side] = 0;
		}
	}

	void Process(float* samples, int frames)
	{
		for (int band = 0; band < kEqBands; band++) {
			Band& b = fBands[band];
			for (int side = 0; side < 2; side++) {
				double z1 = b.z1[side], z2 = b.z2[side];
				float* at = samples + side;
				for (int i = 0; i < frames; i++, at += 2) {
					const double in = *at;
					const double out = b.b0 * in + z1;
					z1 = b.b1 * in - b.a1 * out + z2;
					z2 = b.b2 * in - b.a2 * out;
					*at = (float)out;
				}
				// (very small numbers are slow to work with: silence ends as silence)
				if (fabs(z1) < 1e-20) z1 = 0;
				if (fabs(z2) < 1e-20) z2 = 0;
				b.z1[side] = z1;
				b.z2[side] = z2;
			}
		}
	}

private:
	struct Band {
		double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
		double z1[2] = { 0, 0 }, z2[2] = { 0, 0 };
	};
	Band fBands[kEqBands];
};


#endif	// HRECORD_EQ_H
