/*
 * PicoDrive
 * (c) Copyright Dave, 2004
 * (C) notaz, 2006-2009
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */

#include <string.h>
#include "ym2612.h"
#include "sn76496.h"
#include "../pico_int.h"
#include "../cd/cue.h"
#include "mix.h"

#include "3dsopt.h"
#include "3dssoundqueue.h"
#include "3dsfilereadahead.h"

extern SSoundQueue soundQueue;
extern SDACQueue dacQueue;
extern SDACQueue cddaQueue;

cd_read_ahead_struct cdda_read_ahead;

void (*PsndMix_32_to_16l)(short *dest, int *src, int count) = mix_32_to_16l_stereo;

// master int buffer to mix to
static int PsndBuffer[2*(44100+100)/50];
static int PsndBufferPCM[2*(44100+100)/50];
static int PsndBufferCDDA[2*(44100+100)/50];

// dac, psg
static unsigned short dac_info[312+4]; // pos in sample buffer

// cdda output buffer
short cdda_out_buffer[2*1152];

// sn76496
extern int *sn76496_regs;


static void dac_recalculate(void)
{
  int lines = Pico.m.pal ? 313 : 262;
  int mid = Pico.m.pal ? 68 : 93;
  int i, dac_cnt, pos, len;

  if (Pico.snd.len <= lines)
  {
    // shrinking algo
    dac_cnt = -Pico.snd.len;
    len=1; pos=0;
    dac_info[225] = 1;

    for(i=226; i != 225; i++)
    {
      if (i >= lines) i = 0;
      if(dac_cnt < 0) {
        pos++;
        dac_cnt += lines;
      }
      dac_cnt -= Pico.snd.len;
      dac_info[i] = pos;
    }
  }
  else
  {
    // stretching
    dac_cnt = Pico.snd.len;
    pos=0;
    for(i = 225; i != 224; i++)
    {
      if (i >= lines) i = 0;
      len=0;
      while(dac_cnt >= 0) {
        dac_cnt -= lines;
        len++;
      }
      if (i == mid) // midpoint
        while(pos+len < Pico.snd.len/2) {
          dac_cnt -= lines;
          len++;
        }
      dac_cnt += Pico.snd.len;
      pos += len;
      dac_info[i] = pos;
    }
  }
  for (i = lines; i < sizeof(dac_info) / sizeof(dac_info[0]); i++)
    dac_info[i] = dac_info[0];
}


int lowpass_counter = 0;
int prev_l1, prev_r1;
int prev_l2, prev_r2;
int prev_l3, prev_r3;

PICO_INTERNAL void PsndReset(void)
{
  // PsndRerate calls YM2612Init, which also resets
  PsndRerate(0);
  prev_l1 = 0;
  prev_r1 = 0;
  prev_l2 = 0;
  prev_r2 = 0;
  prev_l3 = 0;
  prev_r3 = 0;
  lowpass_counter = 0;
  timers_reset();

  read_ahead_init(&cdda_read_ahead);
}

int sampleCurrentTo44100[1000];  // Max 882 samples per frame (PAL).

// to be called after changing sound rate or chips
void PsndRerate(int preserve_state)
{
  void *state = NULL;
  int target_fps = Pico.m.pal ? 50 : 60;

  if (preserve_state) {
    state = malloc(0x204);
    if (state == NULL) return;
    ym2612_pack_state();
    memcpy(state, YM2612GetRegs(), 0x204);
  }
  YM2612Init(Pico.m.pal ? OSC_PAL/7 : OSC_NTSC/7, PicoIn.sndRate);
  if (preserve_state) {
    // feed it back it's own registers, just like after loading state
    memcpy(YM2612GetRegs(), state, 0x204);
    ym2612_unpack_state();
  }

  if (preserve_state) memcpy(state, sn76496_regs, 28*4); // remember old state
  SN76496_init(Pico.m.pal ? OSC_PAL/15 : OSC_NTSC/15, PicoIn.sndRate);
  if (preserve_state) memcpy(sn76496_regs, state, 28*4); // restore old state

  if (state)
    free(state);

  // calculate Pico.snd.len
  Pico.snd.len = PicoIn.sndRate / target_fps;
  Pico.snd.len_e_add = ((PicoIn.sndRate - Pico.snd.len * target_fps) << 16) / target_fps;
  Pico.snd.len_e_cnt = 0;

  // recalculate dac info
  dac_recalculate();

  // clear all buffers
  memset32(PsndBuffer, 0, sizeof(PsndBuffer)/4);
  memset(cdda_out_buffer, 0, sizeof(cdda_out_buffer));
  if (PicoIn.sndOut)
    PsndClear();

  // set mixer
  PsndMix_32_to_16l = (PicoIn.opt & POPT_EN_STEREO) ? mix_32_to_16l_stereo : mix_32_to_16_mono;

  if (PicoIn.AHW & PAHW_PICO)
    PicoReratePico();

    // Calculate the sampled PCM/CDDA indexes for
    // rates != 44100khz.
    //
    int samplesPerFrameBasedOn44100 = 44100 / target_fps;
    for (int i = 0; i < Pico.snd.len; i++)
    {
      sampleCurrentTo44100[i] = i * samplesPerFrameBasedOn44100 / Pico.snd.len;
    }
}


PICO_INTERNAL void PsndStartFrame(void)
{
  // compensate for float part of Pico.snd.len
  Pico.snd.len_use = Pico.snd.len;
  Pico.snd.len_e_cnt += Pico.snd.len_e_add;
  if (Pico.snd.len_e_cnt >= 0x10000) {
    Pico.snd.len_e_cnt -= 0x10000;
    Pico.snd.len_use++;
  }

  Pico.snd.dac_line = Pico.snd.psg_line = 0;
  Pico.m.status &= ~1;
  dac_info[224] = Pico.snd.len_use;
}


PICO_INTERNAL void PsndDoDAC(int line_to)
{
  int pos, pos1, len;
  int dout = ym2612.dacout;
  int line_from = Pico.snd.dac_line;

  if (line_to >= 313)
    line_to = 312;

  pos  = dac_info[line_from];
  pos1 = dac_info[line_to + 1];
  len = pos1 - pos;
  if (len <= 0)
    return;

  Pico.snd.dac_line = line_to + 1;

  if (!PicoIn.sndOut)
    return;

  if (PicoIn.opt & POPT_EN_STEREO) {
    short *d = PicoIn.sndOut + pos*2;
    for (; len > 0; len--, d+=2) *d += dout;
  } else {
    short *d = PicoIn.sndOut + pos;
    for (; len > 0; len--, d++)  *d += dout;
  }
}

PICO_INTERNAL void PsndDoPSG(int line_to)
{
  int line_from = Pico.snd.psg_line;
  int pos, pos1, len;
  int stereo = 0;

  if (line_to >= 313)
    line_to = 312;

  pos  = dac_info[line_from];
  pos1 = dac_info[line_to + 1];
  len = pos1 - pos;
  //elprintf(EL_STATUS, "%3d %3d %3d %3d %3d",
  //  pos, pos1, len, line_from, line_to);
  if (len <= 0)
    return;

  Pico.snd.psg_line = line_to + 1;

  if (!PicoIn.sndOut || !(PicoIn.opt & POPT_EN_PSG))
    return;

  if (PicoIn.opt & POPT_EN_STEREO) {
    stereo = 1;
    pos <<= 1;
  }
  SN76496Update(PicoIn.sndOut + pos, len, stereo);
}

// cdda
void cdda_raw_update(int *buffer, int length)
{
  t3dsStartTiming(60, "cdda_raw_update");
  int ret, cdda_bytes, mult = 1;

  //cdda_bytes = length*4;
  //if (PicoIn.sndRate <= 22050 + 100) mult = 2;
  //if (PicoIn.sndRate <  22050 - 100) mult = 4;
  //cdda_bytes *= mult;

  if (Pico.m.pal)
    cdda_bytes = (44100 / 50) * 4;
  else
    cdda_bytes = (44100 / 60) * 4;
  ret = 0;
  //ret = pm_read(cdda_out_buffer, cdda_bytes, Pico_mcd->cdda_stream);
  ret = read_ahead_fread(&cdda_read_ahead, cdda_out_buffer, cdda_bytes, ((pm_file *)Pico_mcd->cdda_stream)->file);
  if (ret < cdda_bytes) {
    memset((char *)cdda_out_buffer + ret, 0, cdda_bytes - ret);
    Pico_mcd->cdda_stream = NULL;
    return;
  }

  if (PicoIn.sndRate == 44100)
  {
    for (int i = 0; i < length; i++)
    {
      buffer[i*2] += cdda_out_buffer[i*2] >> 1;
      buffer[i*2+1] += cdda_out_buffer[i*2+1] >> 1;
    }
    //mix_16h_to_32(buffer, cdda_out_buffer, length*2);
  }
  else
  {
    int incr = 44100 * 16384 / PicoIn.sndRate;
    int idx = 0;
    for (int i = 0; i < length; i++)
    {
      buffer[i*2] += cdda_out_buffer[(idx / 16384)*2] >> 1;
      buffer[i*2+1] += cdda_out_buffer[(idx / 16384)*2+1] >> 1;
      idx = idx + incr;
    }
    /*
    for (int i = 0; i < length; i++)
    {
      buffer[i*2] += cdda_out_buffer[sampleCurrentTo44100[i]*2] >> 1;
      buffer[i*2+1] += cdda_out_buffer[sampleCurrentTo44100[i]*2+1] >> 1;
    }
    */
  }
  /*
  // now mix
  switch (mult) {
    case 1: mix_16h_to_32(buffer, cdda_out_buffer, length*2); break;
    case 2: mix_16h_to_32_s1(buffer, cdda_out_buffer, length*2); break;
    case 4: mix_16h_to_32_s2(buffer, cdda_out_buffer, length*2); break;
  }
  */
  t3dsEndTiming(60);
}


void cdda_start_play(int lba_base, int lba_offset, int lb_len)
{
  if (Pico_mcd->cdda_type == CT_MP3)
  {
    int pos1024 = 0;

    if (lba_offset)
      pos1024 = lba_offset * 1024 / lb_len;

    mp3_start_play(Pico_mcd->cdda_stream, pos1024);
    return;
  }

  //pm_seek(Pico_mcd->cdda_stream, (lba_base + lba_offset) * 2352, SEEK_SET);
  dacQueueReset(&cddaQueue);
  read_ahead_fseek(&cdda_read_ahead, ((pm_file *)Pico_mcd->cdda_stream)->file, (lba_base + lba_offset) * 2352, SEEK_SET);
  if (Pico_mcd->cdda_type == CT_WAV)
  {
    // skip headers, assume it's 44kHz stereo uncompressed
    //pm_seek(Pico_mcd->cdda_stream, 44, SEEK_CUR);
    read_ahead_fseek(&cdda_read_ahead, ((pm_file *)Pico_mcd->cdda_stream)->file, 44, SEEK_CUR);
  }
}


PICO_INTERNAL void PsndClear(void)
{
  int len = Pico.snd.len;
  if (Pico.snd.len_e_add) len++;
  if (PicoIn.opt & POPT_EN_STEREO)
    memset32((int *) PicoIn.sndOut, 0, len); // assume PicoIn.sndOut to be aligned
  else {
    short *out = PicoIn.sndOut;
    if ((long)out & 2) { *out++ = 0; len--; }
    memset32((int *) out, 0, len/2);
    if (len & 1) out[len-1] = 0;
  }
}


// For PicoDrive for 3DS.
static int PsndRender(int offset, int length)
{
  return 0;
}

// For PicoDrive for 3DS.
// This is called by the 2nd core.
//
int PsndRender3DS(short *leftBuffer, short *rightBuffer, int length)
{
  t3dsStartTiming(54, "PsndRender");
  int  buf32_updated = 0;
  int *buf32 = PsndBuffer;
  int stereo = (PicoIn.opt & 8) >> 3;

  pprof_start(sound);

/*
  if (PicoIn.AHW & PAHW_PICO) {
    PicoPicoPCMUpdate(PicoIn.sndOut+offset, length, stereo);
    t3dsEndTiming(54);
    return length;
  }
*/

  // Add in the stereo FM buffer
  if (PicoIn.opt & POPT_EN_FM) {
    buf32_updated = YM2612UpdateOne(buf32, length, stereo, 1);
  } else
    memset32(buf32, 0, length<<stereo);

//printf("active_chs: %02x\n", buf32_updated);
  //(void)buf32_updated;

/*
  // CD: PCM sound
  if (PicoIn.AHW & PAHW_MCD) {
    pcd_pcm_update(buf32, length, stereo);
    //buf32_updated = 1;
  }
*/
/*
  // CD: CDDA audio
  // CD mode, cdda enabled, not data track, CDC is reading
  if ((PicoIn.AHW & PAHW_MCD) && (PicoIn.opt & POPT_EN_MCD_CDDA)
      && Pico_mcd->cdda_stream != NULL
      && !(Pico_mcd->s68k_regs[0x36] & 1))
  {
    // note: only 44, 22 and 11 kHz supported, with forced stereo
    //if (Pico_mcd->cdda_type == CT_MP3)
    //  mp3_update(buf32, length, stereo);
    //else
      cdda_raw_update(buf32, length);
  }
*/
/*
  if ((PicoIn.AHW & PAHW_32X) && (PicoIn.opt & POPT_EN_PWM))
    p32x_pwm_update(buf32, length, stereo);
*/
  // convert + limit to normal 16bit output
  //PsndMix_32_to_16l(PicoIn.sndOut+offset, buf32, length);

  #define MAXOUT		(+32767)
  #define MINOUT		(-32768)

  /* limitter */
  #define Limit(val, max,min) { \
    if ( val > max )      val = max; \
    else if ( val < min ) val = min; \
  }

  int l, r;
  int count = length;
  int *src = buf32;
  short lsample = 0, rsample = 0;

  #define READ_SAMPLE                                                       \
      l = *src++;                                                           \
      r = *src++;                                                           \
                                                                            \
      int hasDAC = dacQueueReadStereo(&dacQueue, &lsample, &rsample);       \
      if (hasDAC)                                                           \
      {                                                                     \
        l += lsample;                                                       \
        r += rsample;                                                       \
      }                                                                     \

      /*hasDAC = dacQueueReadStereo(&cddaQueue, &lsample, &rsample);          \
      if (hasDAC)                                                           \
      {                                                                     \
        l += lsample;                                                       \
        r += rsample;                                                       \
      }                                                                     \*/

  #define WRITE_SAMPLE                                                      \
      l = l * PicoIn.sndVolumeMul / 32;                                     \
      r = r * PicoIn.sndVolumeMul / 32;                                     \
                                                                            \
      Limit( l, MAXOUT, MINOUT );                                           \
      Limit( r, MAXOUT, MINOUT );                                           \
      *leftBuffer++ = l;                                                    \
      *rightBuffer++ = r;                                                   \


  if (PicoIn.lowPassFilter == 0)
  {
    // No low pass filter.
    //
    for (; count > 0; count--)
    {
      READ_SAMPLE
      WRITE_SAMPLE
    }
  }
  else if (PicoIn.lowPassFilter == 3)
  {
    // Low pass filter for Old 3DS (30 KHz)
    //
    for (; count > 0; count--)
    {
      READ_SAMPLE

      // Simple low pass filter.
      int cur_l = l;
      int cur_r = r;
      l = (l + prev_l1 + prev_l2) / 3;
      r = (r + prev_r1 + prev_r2) / 3;
      if (lowpass_counter == 0)
      {
        prev_l1 = cur_l;
        prev_r1 = cur_r;
        lowpass_counter = 1;
      }
      else if (lowpass_counter == 1)
      {
        prev_l2 = cur_l;
        prev_r2 = cur_r;
        lowpass_counter = 0;
      }

      WRITE_SAMPLE
    }
  }
  else if (PicoIn.lowPassFilter == 4)
  {
    // Low pass filter for New 3DS (44 KHz)
    for (; count > 0; count--)
    {
      READ_SAMPLE

      // Simple low pass filter.
      int cur_l = l;
      int cur_r = r;
      l = (l + prev_l1 + prev_l2 + prev_l3) / 4;
      r = (r + prev_r1 + prev_r2 + prev_l3) / 4;
      if (lowpass_counter == 0)
      {
        prev_l1 = cur_l;
        prev_r1 = cur_r;
        lowpass_counter = 1;
      }
      else if (lowpass_counter == 1)
      {
        prev_l2 = cur_l;
        prev_r2 = cur_r;
        lowpass_counter = 2;
      }
      else if (lowpass_counter == 2)
      {
        prev_l3 = cur_l;
        prev_r3 = cur_r;
        lowpass_counter = 0;
      }

      WRITE_SAMPLE
    }
  }

  pprof_end(sound);

  t3dsEndTiming(54);
  return length;
}

/*
static int PsndRender(int offset, int length)
{
  t3dsStartTiming(54, "PsndRender");
  int  buf32_updated = 0;
  int *buf32 = PsndBuffer+offset;
  int stereo = (PicoIn.opt & 8) >> 3;

  offset <<= stereo;

  pprof_start(sound);

  if (PicoIn.AHW & PAHW_PICO) {
    PicoPicoPCMUpdate(PicoIn.sndOut+offset, length, stereo);
    t3dsEndTiming(54);
    return length;
  }

  // Add in the stereo FM buffer
  if (PicoIn.opt & POPT_EN_FM) {
    buf32_updated = YM2612UpdateOne(buf32, length, stereo, 1);
  } else
    memset32(buf32, 0, length<<stereo);

//printf("active_chs: %02x\n", buf32_updated);
  (void)buf32_updated;

  // CD: PCM sound
  if (PicoIn.AHW & PAHW_MCD) {
    pcd_pcm_update(buf32, length, stereo);
    //buf32_updated = 1;
  }

  // CD: CDDA audio
  // CD mode, cdda enabled, not data track, CDC is reading
  if ((PicoIn.AHW & PAHW_MCD) && (PicoIn.opt & POPT_EN_MCD_CDDA)
      && Pico_mcd->cdda_stream != NULL
      && !(Pico_mcd->s68k_regs[0x36] & 1))
  {
    // note: only 44, 22 and 11 kHz supported, with forced stereo
    if (Pico_mcd->cdda_type == CT_MP3)
      mp3_update(buf32, length, stereo);
    else
      cdda_raw_update(buf32, length);
  }

  if ((PicoIn.AHW & PAHW_32X) && (PicoIn.opt & POPT_EN_PWM))
    p32x_pwm_update(buf32, length, stereo);

  // convert + limit to normal 16bit output
  PsndMix_32_to_16l(PicoIn.sndOut+offset, buf32, length);

  pprof_end(sound);

    t3dsEndTiming(54);
  return length;
}
*/


// to be called on 224 or line_sample scanlines only
PICO_INTERNAL void PsndGetSamples(int y)
{
  static int curr_pos = 0;
  int length = Pico.snd.len;

  if (ym2612.dacen && Pico.snd.dac_line < y)
    PsndDoDAC(y - 1);
  PsndDoPSG(y - 1);

  if (y != 224)
  {
    if (PicoIn.AHW & (PAHW_MCD | PAHW_32X))
      memset32(PsndBufferPCM, 0, length * 2);

    if ((PicoIn.AHW & PAHW_32X) && (PicoIn.opt & POPT_EN_PWM))
      p32x_pwm_update(PsndBufferPCM, length / 2, true);
  }
  if (y == 224)
  {
    int *buf32 = PsndBufferPCM;

    // CD: PCM sound
    if (PicoIn.AHW & PAHW_MCD) {
      pcd_pcm_update(PsndBufferPCM, length, true);
    }

    // CD: CDDA audio
    // CD mode, cdda enabled, not data track, CDC is reading
    if ((PicoIn.AHW & PAHW_MCD) && (PicoIn.opt & POPT_EN_MCD_CDDA)
        && Pico_mcd->cdda_stream != NULL
        && !(Pico_mcd->s68k_regs[0x36] & 1))
    {
      // note: only 44, 22 and 11 kHz supported, with forced stereo
      if (Pico_mcd->cdda_type == CT_MP3)
        mp3_update(PsndBufferPCM, length, true);
      else
        cdda_raw_update(PsndBufferPCM, length);
    }

    if ((PicoIn.AHW & PAHW_32X) && (PicoIn.opt & POPT_EN_PWM))
      p32x_pwm_update(&PsndBufferPCM[(length / 2) * 2], length - length / 2, true);

    /*if (Pico.m.status & 2)
         curr_pos += PsndRender(curr_pos, Pico.snd.len-Pico.snd.len/2);
    else curr_pos  = PsndRender(0, Pico.snd.len_use);
    if (Pico.m.status & 1)
         Pico.m.status |=  2;
    else Pico.m.status &= ~2;
    if (PicoIn.writeSound)
      PicoIn.writeSound(curr_pos * ((PicoIn.opt & POPT_EN_STEREO) ? 4 : 2));*/

    // For 3DS
    //
    if (emulator.isReal3DS)
    {
      if (PicoIn.AHW & (PAHW_MCD | PAHW_32X))
      {
        if (!emulator.fastForwarding)
            dacQueueWaitUntilLength(&dacQueue, Pico.snd.len * 2, 20, 1000000);
        for (int i = 0; i < length; i++)
            dacQueueAddStereo(&dacQueue, PicoIn.sndOut[i*2] + buf32[i*2], PicoIn.sndOut[i*2] + (buf32[i*2 + 1]));
      }
      else
      {
        if (!emulator.fastForwarding)
            dacQueueWaitUntilLength(&dacQueue, Pico.snd.len * 2, 20, 1000000);
        for (int i = 0; i < length; i++)
            dacQueueAddStereo(&dacQueue, PicoIn.sndOut[i*2], PicoIn.sndOut[i*2]);
      }
    }

    // clear sound buffer
    PsndClear();
    Pico.snd.dac_line = 224;
    dac_info[224] = 0;
  }
  /*else if (Pico.m.status & 3) {
    Pico.m.status |=  2;
    Pico.m.status &= ~1;
    curr_pos = PsndRender(0, Pico.snd.len/2);
  }*/
}

PICO_INTERNAL void PsndGetSamplesMS(void)
{
  int length = Pico.snd.len_use;

  PsndDoPSG(223);

  // upmix to "stereo" if needed
  if (PicoIn.opt & POPT_EN_STEREO) {
    int i, *p;
    for (i = length, p = (void *)PicoIn.sndOut; i > 0; i--, p++)
      *p |= *p << 16;
  }

  // For 3DS
  //
  if (emulator.isReal3DS)
  {
    if (!emulator.fastForwarding)
        dacQueueWaitUntilLength(&dacQueue, Pico.snd.len * 2, 20, 1000000);

    for (int i = 0; i < Pico.snd.len; i++)
      dacQueueAddStereo(&dacQueue, PicoIn.sndOut[i*2], PicoIn.sndOut[i*2]);
  }

  if (PicoIn.writeSound != NULL)
    PicoIn.writeSound(length * ((PicoIn.opt & POPT_EN_STEREO) ? 4 : 2));
  PsndClear();

  dac_info[224] = 0;
}

// vim:shiftwidth=2:ts=2:expandtab
