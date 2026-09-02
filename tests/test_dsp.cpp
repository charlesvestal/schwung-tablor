/* Phase 2 DSP validation — numeric golden checks for every ported block.
 *
 * The checks assert MEASURED behaviour (frequency response in dB, envelope
 * timing, waveform values), not just "doesn't crash" — the ER-99 lesson.
 * Runs natively inside the build container AND cross-compiled on the Move.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "../src/ported/filter.h"
#include "../src/ported/adsr.h"
#include "../src/ported/analog_tables.h"
#include "../src/ported/wt_oscillator.h"
#include "../src/ported/wav.h"

using namespace tb;

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
    else         {           printf("  ok: " __VA_ARGS__); printf("\n"); } \
} while (0)

static double nowMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1e3 + (double) ts.tv_nsec / 1e6;
}

/* Steady-state gain of `filter` at `hz`: feed a sine, RMS after settling. */
static float filterGainDb(Filter &f, float hz, float sr = 44100.0f)
{
    f.reset();
    const int settle = (int) (sr * 0.2f), measure = (int) (sr * 0.5f);
    std::vector<float> l((size_t) (settle + measure)), r(l.size());
    for (size_t i = 0; i < l.size(); i++)
        l[i] = r[i] = std::sin(2.0 * M_PI * hz * (double) i / sr);
    f.process(l.data(), r.data(), (int) l.size());
    double acc = 0.0;
    for (int i = settle; i < settle + measure; i++)
        acc += (double) l[(size_t) i] * l[(size_t) i];
    double rms = std::sqrt(acc / measure);
    double inRms = 1.0 / std::sqrt(2.0);
    return (float) (20.0 * std::log10(rms / inRms + 1e-12));
}

static void testFilter()
{
    printf("== filter ==\n");
    Filter f;
    f.setSampleRate(44100.0);

    /* LP24 @ 1 kHz: flat passband, ~-6 dB corner (2 cascaded -3 dB stages),
     * -24 dB/oct slope. */
    f.setType(Filter::lowpass);
    f.setSlope(Filter::db24);
    f.setParams(1000.0f, kFilterQ);
    float g100 = filterGainDb(f, 100.0f);
    float g1k  = filterGainDb(f, 1000.0f);
    float g2k  = filterGainDb(f, 2000.0f);
    float g4k  = filterGainDb(f, 4000.0f);
    CHECK(std::fabs(g100) < 0.5f, "LP24 passband flat (100 Hz: %+.2f dB)", g100);
    CHECK(g1k > -7.5f && g1k < -4.5f, "LP24 corner ~-6 dB (1 kHz: %+.2f dB)", g1k);
    float slope = g2k - g4k;
    CHECK(slope > 20.0f && slope < 28.0f, "LP24 slope ~24 dB/oct (2k->4k: %.1f dB)", slope);

    /* LP12: ~-3 dB corner, ~12 dB/oct. */
    f.setSlope(Filter::db12);
    f.setParams(1000.0f, kFilterQ);
    g1k = filterGainDb(f, 1000.0f);
    slope = filterGainDb(f, 2000.0f) - filterGainDb(f, 4000.0f);
    CHECK(g1k > -4.0f && g1k < -2.0f, "LP12 corner ~-3 dB (%+.2f dB)", g1k);
    CHECK(slope > 10.0f && slope < 14.0f, "LP12 slope ~12 dB/oct (%.1f dB)", slope);

    /* Resonance: the voice's q law, res=100% -> q = Q/(1-0.99) — expect a
     * tall peak at the corner. */
    f.setParams(1000.0f, kFilterQ / (1.0f - 0.99f * 0.9f));
    float peak = filterGainDb(f, 1000.0f);
    CHECK(peak > 6.0f, "resonance peaks at corner (%+.1f dB at 90%% res)", peak);

    /* HP24 mirrors LP24. */
    f.setType(Filter::highpass);
    f.setSlope(Filter::db24);
    f.setParams(1000.0f, kFilterQ);
    float hLo = filterGainDb(f, 250.0f), hHi = filterGainDb(f, 4000.0f);
    CHECK(hLo < -40.0f, "HP24 rejects below corner (250 Hz: %+.1f dB)", hLo);
    CHECK(std::fabs(hHi) < 1.0f, "HP24 passes above corner (4 kHz: %+.2f dB)", hHi);

    /* Bandpass peaks at center; notch nulls it. */
    f.setType(Filter::bandpass);
    f.setSlope(Filter::db12);
    f.setParams(1000.0f, kFilterQ);
    float bC = filterGainDb(f, 1000.0f), bLo = filterGainDb(f, 125.0f);
    CHECK(bC > -1.5f && bLo < -15.0f, "BP peaks at center (1k %+.1f, 125 %+.1f dB)", bC, bLo);

    f.setType(Filter::notch);
    f.setParams(1000.0f, kFilterQ);
    float nC = filterGainDb(f, 1000.0f), nLo = filterGainDb(f, 125.0f);
    CHECK(nC < -25.0f && std::fabs(nLo) < 1.0f, "notch nulls center (1k %+.1f, 125 %+.1f dB)", nC, nLo);
}

static void testADSR()
{
    printf("== adsr ==\n");
    AnalogADSR env;
    env.setSampleRate(44100.0);
    env.setAttack(0.1f);
    env.setDecay(0.2f);
    env.setSustainLevel(0.5f);
    env.setRelease(0.15f);

    env.noteOn();
    /* attack: reach ~1.0 around the set time (analog curve hits 1.0 at t_a) */
    int i = 0;
    while (env.getState() == AnalogADSR::State::attack && i < 44100) { env.process(); i++; }
    float tA = (float) i / 44100.0f;
    CHECK(tA > 0.07f && tA < 0.13f, "attack reaches peak in ~0.1 s (%.3f s)", tA);
    CHECK(env.getOutput() >= 0.999f, "peak is 1.0 (%.4f)", env.getOutput());

    /* decay to sustain */
    i = 0;
    while (env.getState() == AnalogADSR::State::decay && i < 44100) { env.process(); i++; }
    float tD = (float) i / 44100.0f;
    CHECK(tD > 0.1f && tD < 0.3f, "decay lands near 0.2 s (%.3f s)", tD);
    CHECK(std::fabs(env.getOutput() - 0.5f) < 0.02f, "sustain level 0.5 (%.3f)", env.getOutput());

    /* sustain holds */
    env.process(4410);
    CHECK(std::fabs(env.getOutput() - 0.5f) < 0.02f, "sustain holds (%.3f)", env.getOutput());

    /* release to idle */
    env.noteOff();
    i = 0;
    while (env.getState() == AnalogADSR::State::release && i < 44100) { env.process(); i++; }
    float tR = (float) i / 44100.0f;
    CHECK(tR > 0.05f && tR < 0.25f, "release lands near 0.15 s (%.3f s)", tR);
    CHECK(env.getState() == AnalogADSR::State::idle && env.getOutput() == 0.0f,
          "ends idle at 0");

    /* attack==0 fast path */
    AnalogADSR fast;
    fast.setSampleRate(44100.0);
    fast.setAttack(0.0f);
    fast.noteOn();
    CHECK(fast.getOutput() == 1.0f && fast.getState() == AnalogADSR::State::decay,
          "attack=0 jumps to peak");

    /* multiply path actually multiplies */
    AnalogADSR mul;
    mul.setSampleRate(44100.0);
    mul.setAttack(0.5f);
    mul.noteOn();
    float l[64], r[64];
    for (auto &v : l) v = 1.0f;
    for (auto &v : r) v = 1.0f;
    mul.processMultiplying(l, r, 64);
    CHECK(l[63] > l[0] && l[63] < 0.05f, "VCA multiply follows early attack (%.4f)", l[63]);
}

/* Short and odd-length wavetables must PLAY, and play at the right pitch.
 *
 * Reported as "shorter wavetable lengths aren't playing" and "the playback is
 * kind of buzzy - makes me wonder if there is a preferential length". Both came
 * from frame-size inference that stopped at 256: a short cycle inferred
 * nothing and the 2048 fallback made nFrames 0 (silent), and a file whose
 * LENGTH divided by 2048 loaded with each frame spanning many real cycles
 * (buzz). Adventure Kid's own AKWF format is 600 samples per cycle and hit the
 * first case exactly. */
static void testShortWavetables()
{
    printf("== short wavetables ==\n");

    /* the inference itself */
    WavData w;
    w.sampleRate = 44100.0f;
    w.samples.assign(600, 0.0f);
    CHECK(wavInferFrameSize(w) == 0,
          "a 600-sample AKWF cycle fits no power-of-two frame (got %d)",
          wavInferFrameSize(w));
    w.samples.assign(128, 0.0f);
    CHECK(wavInferFrameSize(w) == 128, "a 128-sample cycle is found (got %d)",
          wavInferFrameSize(w));
    w.samples.assign(4096, 0.0f);
    CHECK(wavInferFrameSize(w) == 2048,
          "4096 samples still reads as two 2048 frames (got %d)",
          wavInferFrameSize(w));

    /* a short file that REPEATS is several frames, not one long cycle --
     * reported as "the shorter wavetables now loop during playback" */
    {
        std::vector<float> eight(256);
        for (int f = 0; f < 8; f++)
            for (int i = 0; i < 32; i++)
                eight[(size_t)(f * 32 + i)] =
                    std::sin((float) i / 32.0f * 2.0f * (float) M_PI) * (1.0f - 0.05f * f);
        CHECK(wavDetectCycle(eight, 256) == 32,
              "eight 32-sample cycles are found as 32 (got %d)",
              wavDetectCycle(eight, 256));

        std::vector<float> one(256);
        for (int i = 0; i < 256; i++)
            one[(size_t) i] = std::sin((float) i / 256.0f * 2.0f * (float) M_PI);
        CHECK(wavDetectCycle(one, 256) == 0,
              "a genuine single cycle reports no repeat (got %d)",
              wavDetectCycle(one, 256));
    }

    /* a 600-sample sine cycle, resampled, must survive as a sine */
    std::vector<float> cyc(600);
    for (int i = 0; i < 600; i++)
        cyc[(size_t) i] = std::sin((float) i / 600.0f * 2.0f * (float) M_PI);
    std::vector<float> up = wavResampleFrames(cyc, 1, 600, 2048);
    float worst = 0.0f;
    for (int i = 0; i < 2048; i++) {
        float want = std::sin((float) i / 2048.0f * 2.0f * (float) M_PI);
        worst = std::max(worst, std::fabs(up[(size_t) i] - want));
    }
    CHECK(up.size() == 2048 && worst < 0.01f,
          "a 600-sample cycle stretches to 2048 without distorting it (worst %.4f)",
          worst);

    /* and it must build and play at the right pitch */
    Wavetable wt;
    bool built = wtBuild(wt, up.data(), 1, 2048, 44100.0f, 44100.0f);
    CHECK(built && wt.size() == 1, "the stretched cycle builds a table");

    if (built) {
        WTOscillator osc;
        osc.setSampleRate(44100.0);
        osc.setWavetable(&wt);
        WTOscillator::Params op;
        op.position = 0.0f; op.leftGain = 1.0f; op.rightGain = 1.0f;
        float l[8192] = { 0 }, r[8192] = { 0 };
        osc.processAdding(69.0f, op, l, r, 8192);   /* A4 = 440 Hz */
        int crossings = 0;
        for (int i = 1; i < 8192; i++)
            if (l[i - 1] <= 0.0f && l[i] > 0.0f) crossings++;
        float hz = (float) crossings * 44100.0f / 8192.0f;
        CHECK(hz > 420.0f && hz < 460.0f,
              "it sounds at the note it was asked for (%.0f Hz for A4)", hz);
    }
}

/* Short-frame wavetables must play at the pitch they were written at.
 *
 * wavInferFrameSize takes the largest power of two that divides the file, so
 * 16 frames of 256 (4096 samples) read as 2 frames of 2048 -- each holding 8
 * cycles -- and played 8x too high. Reported as "shorter wavetables just
 * sound wrong", with the conclusion that tables MUST be 2048. They must not:
 * they must be READ right. */
static void testShortFrameTables()
{
    printf("== short-frame wavetables ==\n");
    struct C { const char *what; int frame, frames; int morph; int want; };
    const C cases[] = {
        { "16 x 256",              256,  16, 0,  256 },
        { "32 x 512",              512,  32, 0,  512 },
        { "8 x 1024",             1024,   8, 0, 1024 },
        { "16 x 256 morphing",     256,  16, 1,  256 },
        /* the guard: a real 2048 table must NOT be mistaken for short frames,
         * and slow content must not read as a tiny period */
        { "64 x 2048 morphing",   2048,  64, 2, 2048 },
        { "8 x 2048 morphing",    2048,   8, 2, 2048 },
    };
    for (const auto &c : cases) {
        std::vector<float> s((size_t) c.frame * c.frames);
        for (int f = 0; f < c.frames; f++)
            for (int i = 0; i < c.frame; i++) {
                float u = (float) i / c.frame, v;
                if (c.morph == 2) { v = 0; int H = 1 + f * 2;
                    for (int h = 1; h <= H; h++) v += std::sin(2*(float)M_PI*h*u)/h; v *= 0.5f; }
                else if (c.morph == 1) { float m = (float) f / c.frames;
                    v = ((1-m)*std::sin(2*(float)M_PI*u)
                       + m*(std::sin(2*(float)M_PI*u)+0.5f*std::sin(6*(float)M_PI*u))) * 0.7f; }
                else v = std::sin(2*(float)M_PI*u) * 0.8f;
                s[(size_t) f * c.frame + i] = v;
            }
        int got = wavRefineFrameSize(s, wavInferFrameSizeForLength((int) s.size()));
        CHECK(got == c.want, "%s reads as frame %d (got %d)", c.what, c.want, got);
    }

    /* the author's own declaration outranks any guess from the content */
    CHECK(wavFrameSizeFromName("/x/ADD Low FM 001 2048.wav") == 2048, "name: trailing 2048");
    CHECK(wavFrameSizeFromName("/x/Growl 512.wav") == 512, "name: trailing 512");
    CHECK(wavFrameSizeFromName("/x/Pad_256.wav") == 256, "name: underscore 256");
    CHECK(wavFrameSizeFromName("/x/AKWP 0042.wav") == 0, "name: 0042 is an index, not a size");
    CHECK(wavFrameSizeFromName("/x/Bass 600.wav") == 0, "name: 600 is not a legal frame");
    CHECK(wavFrameSizeFromName("/x/render float32.wav") == 0,
          "name: the 32 in \"float32\" is not a frame size");
    CHECK(wavFrameSizeFromName("/x/Sweep-1024.wav") == 1024, "name: hyphen 1024");
}

static void put(FILE*f,const void*p,size_t n){fwrite(p,n,1,f);}
static void p32(FILE*f,uint32_t v){put(f,&v,4);}
static void p16(FILE*f,uint16_t v){put(f,&v,2);}

/* Every wav flavour a wavetable can arrive in must load AND play at the
 * pitch it was written at. A file rejected by the loader, or read at the
 * wrong frame size, is the same thing to a user: "this wavetable does not
 * work". WAVE_FORMAT_EXTENSIBLE is the one that used to be rejected outright
 * -- it is what every tool writing through the Windows API produces. */
/* tag: 1=pcm 3=float 0xFFFE=extensible(sub) ; bits: 8/16/24/32/64 */
static void writeWav(const char*path,const std::vector<float>&s,int tag,int bits,
                     int channels,int sub,const char*clm)
{
    FILE*f=fopen(path,"wb");
    const int bytesPer=bits/8;
    const uint32_t dataBytes=(uint32_t)s.size()*bytesPer*channels;
    const uint32_t fmtLen=(tag==0xFFFE)?40u:16u;
    uint32_t clmLen=clm?(uint32_t)strlen(clm):0; if(clmLen&1) clmLen++;
    fwrite("RIFF",1,4,f); p32(f,4+8+fmtLen+(clm?8+clmLen:0)+8+dataBytes); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); p32(f,fmtLen);
    p16(f,(uint16_t)tag); p16(f,(uint16_t)channels); p32(f,44100);
    p32(f,44100*bytesPer*channels); p16(f,(uint16_t)(bytesPer*channels)); p16(f,(uint16_t)bits);
    if(tag==0xFFFE){ p16(f,22); p16(f,(uint16_t)bits); p32(f,0);
        p16(f,(uint16_t)sub); p16(f,0); p32(f,0); p32(f,0); p32(f,0); }
    if(clm){ fwrite("clm ",1,4,f); p32(f,clmLen);
        fwrite(clm,1,strlen(clm),f); for(uint32_t i=(uint32_t)strlen(clm);i<clmLen;i++) fputc(0,f); }
    fwrite("data",1,4,f); p32(f,dataBytes);
    for(float v : s) for(int c=0;c<channels;c++){
        if(tag==3||sub==3){ if(bits==64){double d=v;put(f,&d,8);} else put(f,&v,4); }
        else if(bits==8){ uint8_t b=(uint8_t)(v*127.0f+128.0f); put(f,&b,1); }
        else if(bits==16){ int16_t x=(int16_t)(v*32767.0f); put(f,&x,2); }
        else if(bits==24){ int32_t x=(int32_t)(v*8388607.0f); put(f,&x,3); }
        else { int32_t x=(int32_t)(v*2147483000.0f); put(f,&x,4); }
    }
    fclose(f);
}
static float heardHz(const std::vector<float>&data,int fs,int nf){
    Wavetable wt; if(!wtBuild(wt,data.data(),nf,fs,44100.0f,44100.0f)) return -1;
    WTOscillator osc; osc.setSampleRate(44100.0); osc.setWavetable(&wt);
    WTOscillator::Params p; p.position=0; p.leftGain=1; p.rightGain=1;
    std::vector<float> l(44100,0),r(44100,0);
    osc.processAdding(57.0f,p,l.data(),r.data(),44100);
    int zc=0; for(size_t i=1;i<l.size();i++) if(l[i-1]<=0&&l[i]>0) zc++;
    return zc*44100.0f/l.size();
}

static void testWavFormats()
{
    printf("== wav formats ==\n");

    struct F { const char*name; int tag,bits,ch,sub; const char*clm; int frame,frames; };
    const F fs[] = {
      {"float32 mono",            3,32,1,0,nullptr,2048,8},
      {"float64 mono",            3,64,1,0,nullptr,2048,8},
      {"pcm16 mono",              1,16,1,0,nullptr,2048,8},
      {"pcm24 mono",              1,24,1,0,nullptr,2048,8},
      {"pcm32 mono",              1,32,1,0,nullptr,2048,8},
      {"pcm8  mono",              1, 8,1,0,nullptr,2048,8},
      {"pcm16 STEREO",            1,16,2,0,nullptr,2048,8},
      {"EXTENSIBLE pcm16",   0xFFFE,16,1,1,nullptr,2048,8},
      {"EXTENSIBLE float32", 0xFFFE,32,1,3,nullptr,2048,8},
      {"EXTENSIBLE stereo24",0xFFFE,24,2,1,nullptr,2048,8},
      {"clm says 1024",           3,32,1,0,"<!>1024 00000000 wavetable",1024,16},
      {"clm says 256",            3,32,1,0,"<!>256 00000000 wavetable",  256,16},
      {"short frames, no clm",    3,32,1,0,nullptr, 256,16},
      {"single cycle 600",        3,32,1,0,nullptr, 600,1},
    };
    
    int bad=0;
    for (auto &c : fs) {
        std::vector<float> s((size_t)c.frame*c.frames);
        for(int f=0;f<c.frames;f++) for(int i=0;i<c.frame;i++)
            s[(size_t)f*c.frame+i]=std::sin(2*(float)M_PI*i/c.frame)*0.8f;
        char path[128]; snprintf(path,sizeof path,"/tmp/fmt_%s.wav",c.name);
        for(char*q=path;*q;q++) if(*q==' ') *q='_';
        writeWav(path,s,c.tag,c.bits,c.ch,c.sub,c.clm);
        WavData w;
        if(!wavLoad(path,w)){ CHECK(false, "%s: REJECTED by the loader", c.name); bad++; continue; }
        int frame = w.clmFrameSize>0 ? w.clmFrameSize : wavFrameSizeFromName(path);
        if(!frame) frame = wavInferFrameSize(w);
        if(w.clmFrameSize<=0 && (int)w.samples.size()<=2048){
            int cyc=wavDetectCycle(w.samples,(int)w.samples.size()); if(cyc>0) frame=cyc; }
        if(w.clmFrameSize<=0 && !wavFrameSizeFromName(path))
            frame = wavRefineFrameSize(w.samples, frame);
        if(!frame) frame=(int)w.samples.size();
        int nf = (int)w.samples.size()/frame;
        std::vector<float> d=w.samples;
        if(!wavIsPow2(frame)||frame<32){ d=wavResampleFrames(d,nf,frame,2048); frame=2048; }
        float hz = nf ? heardHz(d,frame,nf) : -1;
        bool ok = hz>205 && hz<236;
        if(!ok) bad++;
        CHECK(ok, "%s -> frame %d, %d frames, %.1f Hz", c.name, frame, nf, hz);
    }
    
    
    (void) bad;
}

/* A real additive table that MUST NOT be "corrected".
 *
 * DigiAdd07 is 13824 samples of 16-bit PCM, no clm, and morphs from a full
 * harmonic series to even harmonics only. Its later frames therefore consist
 * of two identical half-cycles -- legitimately, because that is what an
 * even-harmonics-only waveform looks like. Any detector that halves a frame
 * on seeing repeated halves destroys this table: the EARLY frames are not
 * repeated, so halving reads two different cycles as one.
 *
 * The distribution is what separates the two cases. A table whose real frame
 * is half its apparent one repeats UNIFORMLY; this one grades smoothly from
 * 1.91 down to 0.00 across its frames. wavRefineFrameSize requires the whole
 * file to repeat, so it correctly leaves this alone. */
static void testMorphToEvenHarmonicsKeepsItsFrame()
{
    printf("== a table morphing to even harmonics keeps its frame ==\n");
    const int F = 512, N = 27;
    std::vector<float> s((size_t) F * N);
    for (int k = 0; k < N; k++) {
        float m = (float) k / (N - 1);          /* 0 = all harmonics, 1 = even only */
        for (int i = 0; i < F; i++) {
            float u = (float) i / F, v = 0.0f;
            for (int h = 1; h <= 16; h++) {
                float amp = (h & 1) ? (1.0f - m) / h : 1.0f / h;
                v += amp * std::sin(2.0f * (float) M_PI * h * u);
            }
            s[(size_t) k * F + i] = v * 0.4f;
        }
    }
    int got = wavRefineFrameSize(s, 512);
    CHECK(got == 512, "frames stay 512, not halved to 256 (got %d)", got);

    /* The measured gap the threshold sits in, from real files:
     *   must NOT halve   DigiAdd07 0.478, WAVETABLE Factory 0.999,
     *                    a genuine 2048 table 1.000
     *   MUST halve       128-frame table 0.056/0.043/0.026/0.056,
     *                    256-frame table 0.011
     * 0.15 is ~3x clear of both sides. These two synthetic tables stand in
     * for each end, so a future tweak that closes the gap fails here. */
    {
        /* a 2048 frame that is really 16 cycles of 128, morphing */
        const int F = 2048, N = 4;
        std::vector<float> t((size_t) F * N);
        for (int k = 0; k < N * 16; k++) {
            float m = (float) k / (N * 16 - 1);
            for (int i = 0; i < 128; i++) {
                float u = (float) i / 128, v = 0.0f;
                for (int h = 1; h <= 1 + (int) (m * 15); h++)
                    v += std::sin(2.0f * (float) M_PI * h * u) / h;
                t[(size_t) k * 128 + i] = v * 0.4f;
            }
        }
        CHECK(wavRefineFrameSize(t, 2048) == 128,
              "a 2048 frame holding 16 morphing cycles resolves to 128 (got %d)",
              wavRefineFrameSize(t, 2048));
    }

    /* and the genuine case still IS caught: every frame two identical halves */
    std::vector<float> t((size_t) F * N);
    for (int k = 0; k < N; k++)
        for (int i = 0; i < F; i++)
            t[(size_t) k * F + i] =
                std::sin(2.0f * (float) M_PI * 2.0f * i / F) * 0.8f;   /* 2 cycles */
    CHECK(wavRefineFrameSize(t, 512) == 256,
          "a frame that really is two cycles still halves");
}

static void testAnalogTables()
{
    printf("== analog tables ==\n");
    double t0 = nowMs();
    AnalogTables tables(44100.0);
    double buildMs = nowMs() - t0;
    printf("  build: %.0f ms, %.2f MB\n", buildMs, tables.bytes() / 1e6);
    CHECK(buildMs < 250.0, "analog tables build fast (%.0f ms; was 2743 additive)", buildMs);
    CHECK(tables.bytes() < 2e6, "analog tables decimated (%.2f MB)", tables.bytes() / 1e6);

    /* Spectrum-built tables must equal the additive formula wherever the
     * table can hold every harmonic. Note 60's level is built for top note
     * 60.5 (311 Hz, ~70 harmonics — well under the table Nyquist). */
    float maxErr = 0.0f;
    const float refNote = 60.5f, refHz = midiNoteToHz(60.5f);
    for (int i = 0; i < 64; i++) {
        float ph = (float) i / 64.0f;
        float gotT = tables.processTriangle(60.0f, ph);
        float wantT = waves::triangle(ph, refHz, 44100.0f);
        float gotS = tables.processSawUp(60.0f, ph);
        float wantS = waves::sawUp(ph, refHz, 44100.0f);
        maxErr = std::max({ maxErr, std::fabs(gotT - wantT), std::fabs(gotS - wantS) });
    }
    (void) refNote;
    CHECK(maxErr < 0.01f, "FFT-built tables match additive formula (err %.4f)", maxErr);

    /* Square via saws ~ +/-1; pulse pw=0.5 has ~0 DC. */
    float sq = tables.processSquare(60.0f, 0.1f);
    CHECK(sq > 0.7f && sq < 1.3f, "square high half ~ +1 (%.2f)", sq);
    double dc = 0.0;
    for (int i = 0; i < 512; i++)
        dc += tables.processPulse(60.0f, (float) i / 512.0f, 0.5f);
    CHECK(std::fabs(dc / 512.0) < 0.02, "pulse pw=0.5 DC ~ 0 (%.4f)", dc / 512.0);

    /* Noise: white RMS ~ 0.1 (gin's stddev), pink bounded. */
    WhiteNoise wn;
    PinkNoise pn;
    double wAcc = 0.0; float pPeak = 0.0f;
    for (int i = 0; i < 44100; i++) {
        float w = wn.nextSample(); wAcc += (double) w * w;
        pPeak = std::max(pPeak, std::fabs(pn.nextSample()));
    }
    float wRms = (float) std::sqrt(wAcc / 44100.0);
    CHECK(wRms > 0.08f && wRms < 0.12f, "white noise RMS ~ 0.1 (%.3f)", wRms);
    CHECK(pPeak > 0.05f && pPeak < 1.5f, "pink noise bounded (peak %.2f)", pPeak);

    /* Sub oscillator renders sanely. */
    StereoOscillator sub(tables);
    sub.setSampleRate(44100.0);
    sub.noteOn(0.0f);
    StereoOscillator::Params sp;
    sp.wave = Wave::square;
    float l[512] = {0}, r[512] = {0};
    sub.processAdding(48.0f, sp, l, r, 512);
    float pk = 0.0f; bool finite = true;
    for (auto v : l) { pk = std::max(pk, std::fabs(v)); finite &= std::isfinite(v); }
    CHECK(finite && pk > 0.5f && pk < 2.0f, "sub osc renders square (peak %.2f)", pk);
}

int main()
{
    testFilter();
    testADSR();
    testShortWavetables();
    testShortFrameTables();
    testWavFormats();
    testMorphToEvenHarmonicsKeepsItsFrame();
    testAnalogTables();
    printf("\n%s (%d failures)\n", g_fail ? "DSP TESTS FAILED" : "DSP TESTS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
