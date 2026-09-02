/* Full-engine CPU check: 8-voice chord, everything on. Run on the Move. */
#include "../src/dsp/engine.h"
#include <cstdio>
#include <ctime>
using namespace tb;
static double nowMs(){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec*1e3 + ts.tv_nsec/1e6; }
int main(){
    Engine e;
    /* worst realistic patch: both oscs on, unison 4, bend+formant, sub+noise,
       filter resonant */
    e.pots[TB_P_WT2_LEVEL]=100; e.pots[TB_P_WT1_UNI]=4; e.pots[TB_P_WT2_UNI]=4;
    e.pots[TB_P_WT1_DETUNE]=40; e.pots[TB_P_WT2_DETUNE]=40;
    e.pots[TB_P_WT1_BEND]=90;   e.pots[TB_P_WT2_FORMANT]=90;
    e.pots[TB_P_SUB_LEVEL]=90;  e.pots[TB_P_NOISE_LEVEL]=60;
    e.pots[TB_P_FLT_RES]=80;    e.pots[TB_P_FLT_FREQ]=90;
    const uint8_t notes[8]={36,43,48,55,60,64,67,72};
    for (auto n : notes){ uint8_t on[]={0x90,n,100}; e.onMidi(on,3); }
    int16_t out[256];
    for (int b=0;b<100;b++) e.renderBlock(out,128);   /* warm */
    const int blocks = 3444;                           /* 10 s of audio */
    double t0=nowMs();
    for (int b=0;b<blocks;b++) e.renderBlock(out,128);
    double wall=nowMs()-t0;
    double audio=blocks*128.0/44.1;
    printf("full engine, 8 voices x uni4 + sub + noise + filter: %.2fx realtime "
           "(%.3f ms/block, %.0f%% of 2.90 ms budget)\n",
           audio/wall, wall/blocks, 100.0*(wall/blocks)/2.902);
    return 0;
}
