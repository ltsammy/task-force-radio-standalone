namespace Tfrs.VoiceClient.Audio.Dsp;

/// <summary>
/// Direct-Form-I biquad, RBJ Audio EQ Cookbook coefficient convention (a0-normalized).
/// See docs/dsp-audio-pipeline.md section 4.
/// </summary>
internal sealed class BiquadFilter
{
    private double _b0, _b1, _b2, _a1, _a2;
    private double _x1, _x2, _y1, _y2;

    public static BiquadFilter RbjLowPass(double sampleRate, double cutoffHz, double q)
    {
        double w0 = 2 * Math.PI * cutoffHz / sampleRate;
        double cs = Math.Cos(w0), sn = Math.Sin(w0), al = sn / (2 * q);
        double b0 = (1 - cs) / 2, b1 = 1 - cs, b2 = (1 - cs) / 2;
        double a0 = 1 + al, a1 = -2 * cs, a2 = 1 - al;
        return new BiquadFilter(b0, b1, b2, a0, a1, a2);
    }

    public static BiquadFilter RbjHighPass(double sampleRate, double cutoffHz, double q)
    {
        double w0 = 2 * Math.PI * cutoffHz / sampleRate;
        double cs = Math.Cos(w0), sn = Math.Sin(w0), al = sn / (2 * q);
        double b0 = (1 + cs) / 2, b1 = -(1 + cs), b2 = (1 + cs) / 2;
        double a0 = 1 + al, a1 = -2 * cs, a2 = 1 - al;
        return new BiquadFilter(b0, b1, b2, a0, a1, a2);
    }

    /// <summary>Constant skirt-gain bandpass (RBJ cookbook), parameterized by center freq + Q.</summary>
    public static BiquadFilter RbjBandPass(double sampleRate, double centerHz, double q)
    {
        double w0 = 2 * Math.PI * centerHz / sampleRate;
        double cs = Math.Cos(w0), sn = Math.Sin(w0), al = sn / (2 * q);
        double b0 = sn / 2, b1 = 0, b2 = -sn / 2;
        double a0 = 1 + al, a1 = -2 * cs, a2 = 1 - al;
        return new BiquadFilter(b0, b1, b2, a0, a1, a2);
    }

    private BiquadFilter(double b0, double b1, double b2, double a0, double a1, double a2)
    {
        _b0 = b0 / a0; _b1 = b1 / a0; _b2 = b2 / a0;
        _a1 = a1 / a0; _a2 = a2 / a0;
    }

    public void Reset() => _x1 = _x2 = _y1 = _y2 = 0;

    public float Process(float input)
    {
        double x0 = input;
        double y0 = _b0 * x0 + _b1 * _x1 + _b2 * _x2 - _a1 * _y1 - _a2 * _y2;
        _x2 = _x1; _x1 = x0;
        _y2 = _y1; _y1 = y0;
        return (float)y0;
    }

    public void Process(Span<float> buffer)
    {
        for (int i = 0; i < buffer.Length; i++)
            buffer[i] = Process(buffer[i]);
    }
}

/// <summary>
/// Cascaded biquad stages for filter orders above 2 (higher-order Butterworth via standard
/// pole-angle Q factorization + bilinear-transformed RBJ sections). See docs/dsp-audio-pipeline.md
/// section 4.
/// </summary>
internal sealed class CascadedFilter
{
    private readonly BiquadFilter[] _stages;

    private CascadedFilter(BiquadFilter[] stages) => _stages = stages;

    /// <summary>Standard Butterworth lowpass built from order/2 RBJ lowpass biquads, each at the
    /// same cutoff with Q = 1 / (2 * cos(theta_k)), theta_k = (2k-1)*pi/(2*order).</summary>
    public static CascadedFilter ButterworthLowPass(double sampleRate, double cutoffHz, int order)
    {
        if (order % 2 != 0 || order < 2)
            throw new ArgumentException("Only even Butterworth orders >= 2 are supported.", nameof(order));

        int sections = order / 2;
        var stages = new BiquadFilter[sections];
        for (int k = 1; k <= sections; k++)
        {
            double theta = (2 * k - 1) * Math.PI / (2 * order);
            double q = 1.0 / (2.0 * Math.Cos(theta));
            stages[k - 1] = BiquadFilter.RbjLowPass(sampleRate, cutoffHz, q);
        }
        return new CascadedFilter(stages);
    }

    /// <summary>Approximation for the DSPFilters "order N Butterworth bandpass" stages used by
    /// the original TS3 plugin: N cascaded constant-skirt-gain RBJ bandpass biquads at the same
    /// center/Q. Not a literal Butterworth bandpass derivation, but audibly equivalent for the
    /// narrow effect bands used here — see docs/dsp-audio-pipeline.md section 4/8.</summary>
    public static CascadedFilter BandPass(double sampleRate, double centerHz, double bandwidthHz, int order)
    {
        double q = centerHz / bandwidthHz;
        var stages = new BiquadFilter[Math.Max(1, order)];
        for (int i = 0; i < stages.Length; i++)
            stages[i] = BiquadFilter.RbjBandPass(sampleRate, centerHz, q);
        return new CascadedFilter(stages);
    }

    public void Reset()
    {
        foreach (var s in _stages) s.Reset();
    }

    public void Process(Span<float> buffer)
    {
        foreach (var stage in _stages)
            stage.Process(buffer);
    }
}
