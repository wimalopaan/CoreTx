// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) EdgeTX; based on OpenTX, th9x, er9x and gruvin9x.
// Unmodified function bodies, captured by tools/sync_edgetx_reference.py.
// EdgeTX revision: 90e8f0c55cdee7984eb0e7f415108be94c4232fe
// This file is a test fixture, never part of coretx.

#line 52 "radio/src/edgetx_helpers.h"
inline int divRoundClosest(int n, int d)
{
  if (d == 0)
    return 0;
  else
    return ((n < 0) ^ (d < 0)) ? ((n - d/2)/d) : ((n + d/2)/d);
}

#line 369 "radio/src/edgetx.h"
inline int calc100to256(int x)
{
  return divRoundClosest(x*256, 100);
}

#line 374 "radio/src/edgetx.h"
inline int calc100toRESX(int x)
{
  return divRoundClosest(x*RESX, 100);
}

#line 379 "radio/src/edgetx.h"
inline int calc1000toRESX(int x)
{
  return divRoundClosest(x*RESX, 1000);
}

#line 146 "radio/src/mixer.cpp"
unsigned int expou(unsigned int x, unsigned int k)
{
#if defined(EXTENDED_EXPO)
  bool extended;
  if (k > 80) {
    extended=true;
  }
  else {
    k += (k>>2);  // use bigger values before extend, because the effect is anyway very very low
    extended=false;
  }
#endif

  k = calc100to256(k);

  uint32_t value = (uint32_t) x*x;
  value *= (uint32_t)k;
  value >>= 8;
  value *= (uint32_t)x;

#if defined(EXTENDED_EXPO)
  if (extended) {  // for higher values do more multiplications to get a stronger expo curve
    value >>= 16;
    value *= (uint32_t)x;
    value >>= 4;
    value *= (uint32_t)x;
  }
#endif

  value >>= 12;
  value += (uint32_t)(256-k) * x + 128;

  return value >> 8;
}

#line 181 "radio/src/mixer.cpp"
int expo(int x, int k)
{
  if (k == 0) {
    return x;
  }

  int y;
  bool neg = (x < 0);

  if (neg) {
    x = -x;
  }
  if (x > (int)RESXu) {
    x = RESXu;
  }
  if (k < 0) {
    y = RESXu - expou(RESXu-x, -k);
  }
  else {
    y = expou(x, k);
  }
  return neg ? -y : y;
}

#line 205 "radio/src/mixer.cpp"
void applyExpos(int16_t * anas, uint8_t mode, int16_t ovwrIdx, int16_t ovwrValue)
{
  int8_t cur_chn = -1;

  for (uint8_t i=0; i<MAX_EXPOS; i++) {
    if (mode == e_perout_mode_normal) mixState[i].activeExpo = false;
    ExpoData * ed = expoAddress(i);
    mixsrc_t srcRaw = ed->srcRaw;
    mixsrc_t src = abs(srcRaw);
    if (!EXPO_VALID(ed)) break; // end of list
    if (ed->chn == cur_chn)
      continue;
    if (ed->flightModes & (1<<mixerCurrentFlightMode))
      continue;
    if (src >= MIXSRC_FIRST_TRAINER && src <= MIXSRC_LAST_TRAINER && !isTrainerValid())
      continue;
    if (getSwitch(ed->swtch)) {
      int32_t v;
      if (srcRaw == ovwrIdx) {
        v = ovwrValue;
      }
      else {
        v = getValue(srcRaw);
        if (src >= MIXSRC_FIRST_TELEM && ed->scale > 0) {
          v = (v * 1024) / convertTelemValue(src-MIXSRC_FIRST_TELEM+1, ed->scale);
        }
        v = limit<int32_t>(-1024, v, 1024);
      }
      if (EXPO_MODE_ENABLE(ed, v)) {
        if (mode == e_perout_mode_normal) mixState[i].activeExpo = true;
        cur_chn = ed->chn;

        //========== CURVE=================
        if (ed->curve.value) {
          v = applyCurve(v, ed->curve);
        }

        //========== WEIGHT ===============
        int32_t weight = getSourceNumFieldValue(ed->weight, -100, 100);
        v = divRoundClosest((int32_t)v * weight, 1000);

        //========== OFFSET ===============
        int32_t offset = getSourceNumFieldValue(ed->offset, -100, 100);
        if (offset) v += divRoundClosest(calc100toRESX(offset), 10);

        //========== TRIMS ================
        if (ed->trimSource < TRIM_ON)
          virtualInputsTrims[cur_chn] = -ed->trimSource - 1;
        else if (ed->trimSource == TRIM_ON && src >= MIXSRC_FIRST_STICK &&
                 src <= MIXSRC_LAST_STICK)
          virtualInputsTrims[cur_chn] = src - MIXSRC_FIRST_STICK;
        else
          virtualInputsTrims[cur_chn] = -1;
        // a negative weight or a decreasing custom curve (incl. !CVn)
        // reverses the input, so its trim must follow
        bool reversed = (weight < 0);
        if (ed->curve.type == CURVE_REF_CUSTOM && ed->curve.value &&
            applyCurve(RESX, ed->curve) < applyCurve(-RESX, ed->curve))
          reversed = !reversed;
        virtualInputsTrimsReversed[cur_chn] = reversed;
        // if (srcRaw < 0) v = -v;
        anas[cur_chn] = v;
      } else {
        anas[ed->chn] = 0;
      }
    }
  }
}

#line 293 "radio/src/mixer.cpp"
int16_t applyLimits(uint8_t channel, int32_t value)
{
#if defined(OVERRIDE_CHANNEL_FUNCTION)
  if (safetyCh[channel] != OVERRIDE_CHANNEL_UNDEFINED) {
    // safety channel available for channel check
    return calc100toRESX(safetyCh[channel]);
  }
#endif

  if (isFunctionActive(FUNCTION_TRAINER_CHANNELS) && isTrainerValid()) {
    return trainerInput[channel] * 2;
  }

  LimitData * lim = limitAddress(channel);

  if (lim->curve) {
    // TODO we loose precision here, applyCustomCurve could work with int32_t on ARM boards...
    if (lim->curve > 0)
      value = 256 * applyCustomCurve(value/256, lim->curve-1);
    else
      value = 256 * applyCustomCurve(-value/256, -lim->curve-1);
  }

  int16_t ofs   = LIMIT_OFS_RESX(lim);
  int16_t lim_p = LIMIT_MAX_RESX(lim);
  int16_t lim_n = LIMIT_MIN_RESX(lim);

  if (ofs > lim_p) ofs = lim_p;
  if (ofs < lim_n) ofs = lim_n;

  // because the rescaling optimization would reduce the calculation reserve we activate this for all builds
  // it increases the calculation reserve from factor 20,25x to 32x, which it slightly better as original
  // without it we would only have 16x which is slightly worse as original, we should not do this

  // thanks to gbirkus, he motivated this change, which greatly reduces overruns
  // unfortunately the constants and 32bit compares generates about 50 bytes codes; didn't find a way to get it down.
  value = limit(int32_t(-RESXl*256), value, int32_t(RESXl*256));  // saves 2 bytes compared to other solutions up to now

#if defined(PPM_LIMITS_SYMETRICAL)
  if (value) {
    int16_t tmp;
    if (lim->symetrical)
      tmp = (value > 0) ? (lim_p) : (-lim_n);
    else
      tmp = (value > 0) ? (lim_p - ofs) : (-lim_n + ofs);
    value = (int32_t) value * tmp;   //  div by 1024*256 -> output = -1024..1024
#else
  if (value) {
    int16_t tmp = (value > 0) ? (lim_p - ofs) : (-lim_n + ofs);
    value = (int32_t) value * tmp;   //  div by 1024*256 -> output = -1024..1024
#endif

    // Round away from 0
    tmp = (value + (value < 0 ? (1<<17)-1 : (1<<17))) >> 18;

    ofs += tmp;  // ofs can to added directly because already recalculated,
  }

  if (ofs > lim_p)
    ofs = lim_p;
  if (ofs < lim_n)
    ofs = lim_n;
  if (lim->revert)
    ofs = -ofs; // finally do the reverse.

  return ofs;
}

#line 600 "radio/src/mixer.cpp"
void evalTrims()
{
  uint8_t phase = mixerCurrentFlightMode;
  for (uint8_t i = 0; i < keysGetMaxTrims(); i++) {
    // do trim -> throttle trim if applicable
    int16_t trim = getTrimValue(phase, i);
    if (trimsCheckTimer > 0) {
      trim = 0;
    }

    trims[i] = trim * 2;
  }
}

#line 737 "radio/src/mixer.cpp"
int getStickTrimValue(int stick, int stickValue)
{
  if (stick < 0)
    return 0;

  int trim = trims[stick];
  uint8_t thrTrimSw = g_model.getThrottleStickTrimSource() - MIXSRC_FIRST_TRIM;
  if (stick == thrTrimSw) {  // trim for throttle
    if (g_model.throttleReversed) trim = -trim;
    if (g_model.thrTrim) {   // throttle idle ON
      // evalTrims() store 2 * trim value in trims[]
      // so here, trim values range from -256 to 256
      // and extended trim values range from -1024 to 1024
      trim = (g_model.extendedTrims) ? 2 * TRIM_EXTENDED_MAX + trim
                                     : 2 * TRIM_MAX + trim;
      trim = trim * (1024 - stickValue) / (IDLE_TRIM_SCALE * RESX);
#if defined(SURFACE_RADIO)
      // Throttle Idle trim (g_model.thrTrim) should affect only forward stick (0 to 1024)
      // Return no trim for reverse side when throttle idle trim is enabled
      if (stickValue < 0) return 0;
#endif
    }
#if defined(SURFACE_RADIO)
    // divide throtle trim by two since since the full extend
    // of forward/reverse chan is only 1024 instead of 2048
    trim >>= 1;
#endif
  }
  return trim;
}

#line 768 "radio/src/mixer.cpp"
int getSourceTrimOrigin(int source)
{
  if (source >= MIXSRC_FIRST_STICK && source <= MIXSRC_LAST_STICK)
    return source - MIXSRC_FIRST_STICK;
  else if (source >= MIXSRC_FIRST_INPUT && source <= MIXSRC_LAST_INPUT)
    return virtualInputsTrims[source - MIXSRC_FIRST_INPUT];
  else
    return -1;
}

#line 778 "radio/src/mixer.cpp"
int getSourceTrimValue(int source, int stickValue=0)
{
  int rv = 0;
  auto absSource = abs(source);
  auto origin = getSourceTrimOrigin(absSource);
  if (origin >= 0) {
    if (absSource >= MIXSRC_FIRST_INPUT && absSource <= MIXSRC_LAST_INPUT &&
        virtualInputsTrimsReversed[absSource - MIXSRC_FIRST_INPUT]) {
      rv = -getStickTrimValue(origin, -stickValue);
    } else {
      rv = getStickTrimValue(origin, stickValue);
    }
    if (source < 0) rv = -rv;
  }
  return rv;
}

#line 797 "radio/src/mixer.cpp"
static inline bitfield_channels_t channel_bit(uint16_t ch)
{
  return (bitfield_channels_t)1 << ch;
}

#line 802 "radio/src/mixer.cpp"
static inline bitfield_channels_t channel_dirty(bitfield_channels_t mask, uint16_t ch)
{
  return mask & channel_bit(ch);
}

#line 807 "radio/src/mixer.cpp"
static inline bitfield_channels_t upper_channels_mask(uint16_t ch)
{
  // take the 1's complement to generate a bit pattern
  // that has all bits of 'ch' order and above set
  //
  // Examples (mask for max 8 channels):
  // - channel 0: 0b11111111
  // - channel 1: 0b11111110
  // - channel 2: 0b11111100

  return ~(channel_bit(ch)) + 1;
}

#line 822 "radio/src/mixer.cpp"
void evalFlightModeMixes(uint8_t mode, uint8_t tick10ms)
{
  evalInputs(mode);

  if (tick10ms)
    evalLogicalSwitches(mode==e_perout_mode_normal);

#if defined(HELI)
  if (modelHeliEnabled()) {
    int heliEleValue = getValue(g_model.swashR.elevatorSource);
    int heliAilValue = getValue(g_model.swashR.aileronSource);
    if (g_model.swashR.value) {
      uint32_t v = ((int32_t)heliEleValue*heliEleValue + (int32_t)heliAilValue*heliAilValue);
      uint32_t q = calc100toRESX(g_model.swashR.value);
      q *= q;
      if (v>q) {
        uint16_t d = isqrt32(v);
        int16_t tmp = calc100toRESX(g_model.swashR.value);
        heliEleValue = (int32_t) heliEleValue*tmp/d;
        heliAilValue = (int32_t) heliAilValue*tmp/d;
      }
    }

  #define REZ_SWASH_X(x)  ((x) - (x)/8 - (x)/128 - (x)/512)   //  1024*sin(60) ~= 886
  #define REZ_SWASH_Y(x)  ((x))   //  1024 => 1024

    if (g_model.swashR.type) {
      getvalue_t vp = heliEleValue + getSourceTrimValue(g_model.swashR.elevatorSource);
      getvalue_t vr = heliAilValue + getSourceTrimValue(g_model.swashR.aileronSource);
      getvalue_t vc = 0;
      if (g_model.swashR.collectiveSource)
        vc = getValue(g_model.swashR.collectiveSource);

      vp = (vp * g_model.swashR.elevatorWeight) / 100;
      vr = (vr * g_model.swashR.aileronWeight) / 100;
      vc = (vc * g_model.swashR.collectiveWeight) / 100;

      switch (g_model.swashR.type) {
        case SWASH_TYPE_120:
          vp = REZ_SWASH_Y(vp);
          vr = REZ_SWASH_X(vr);
          cyc_anas[0] = vc - vp;
          cyc_anas[1] = vc + vp/2 + vr;
          cyc_anas[2] = vc + vp/2 - vr;
          break;
        case SWASH_TYPE_120X:
          vp = REZ_SWASH_X(vp);
          vr = REZ_SWASH_Y(vr);
          cyc_anas[0] = vc - vr;
          cyc_anas[1] = vc + vr/2 + vp;
          cyc_anas[2] = vc + vr/2 - vp;
          break;
        case SWASH_TYPE_140:
          vp = REZ_SWASH_Y(vp);
          vr = REZ_SWASH_Y(vr);
          cyc_anas[0] = vc - vp;
          cyc_anas[1] = vc + vp + vr;
          cyc_anas[2] = vc + vp - vr;
          break;
        case SWASH_TYPE_90:
          vp = REZ_SWASH_Y(vp);
          vr = REZ_SWASH_Y(vr);
          cyc_anas[0] = vc - vp;
          cyc_anas[1] = vc + vr;
          cyc_anas[2] = vc - vr;
          break;
        default:
          break;
      }
    }
  } else {
    cyc_anas[0] = cyc_anas[1] = cyc_anas[2] = 0;
  }
#endif

  memclear(chans, sizeof(chans)); // all outputs to 0

  //========== MIXER LOOP ===============

  uint8_t pass = 0;
  uint8_t lv_mixWarning = 0;
  bitfield_channels_t dirtyChannels = all_channels_dirty;

  // Calculate locally and then copy to mixState array - prevent UI seeing phantom values while calculating
  bool activeMixes[MAX_MIXERS];

  do {
    bitfield_channels_t passDirtyChannels = 0;

    for (uint8_t i=0; i<MAX_MIXERS; i++) {
      if (mode == e_perout_mode_normal && pass == 0)
        activeMixes[i] = 0;

      MixData * md = mixAddress(i);
      mixsrc_t srcRaw = md->srcRaw;
      mixsrc_t srcRawAbs = abs(srcRaw);

      if (srcRaw == 0) {
#if defined(COLORLCD)
        continue;
#else
        break;
#endif
      }

      if (!channel_dirty(dirtyChannels, md->destCh))
        continue;

      // if this is the first calculation for the destination channel,
      // initialize it with 0 (otherwise would be random)
      if (i == 0 || md->destCh != (md - 1)->destCh)
        chans[md->destCh] = 0;

      //========== FLIGHT MODE && SWITCH =====
      bool mixCondition = (md->flightModes != 0 || md->swtch);
      bool fmEnabled = (md->flightModes & (1 << mixerCurrentFlightMode)) == 0;
      bool mixLineActive = fmEnabled && getSwitch(md->swtch);
      delayval_t mixEnabled = (mixLineActive) ? DELAY_POS_MARGIN+1 : 0;

      if (mixLineActive) {
        // disable mixer using trainer channels if not connected
        if (srcRawAbs >= MIXSRC_FIRST_TRAINER &&
            srcRawAbs <= MIXSRC_LAST_TRAINER && !isTrainerValid()) {
          mixCondition = true;
          mixEnabled = 0;
        }

#if defined(LUA_MODEL_SCRIPTS)
        // disable mixer if Lua script is used as source and script was killed
        if (srcRawAbs >= MIXSRC_FIRST_LUA && srcRawAbs <= MIXSRC_LAST_LUA) {
          div_t qr = div(int(srcRawAbs - MIXSRC_FIRST_LUA), MAX_SCRIPT_OUTPUTS);
          for (int n = 0; n < MAX_SCRIPTS; n += 1) {
            if ((scriptInternalData[n].reference == qr.quot) && (scriptInternalData[n].state != SCRIPT_OK)) {
              mixCondition = true;
              mixEnabled = 0;
            }
          }
        }
#endif
      }

      //========== VALUE ===============
      getvalue_t v = 0;

      if (mode > e_perout_mode_inactive_flight_mode) {
        if (mixEnabled)
          v = getValue(srcRaw);
        else
          continue;
      } else {
        v = getValue(srcRaw);

        if (srcRawAbs >= MIXSRC_FIRST_CH && srcRawAbs <= MIXSRC_LAST_CH) {

          auto srcChan = srcRawAbs - MIXSRC_FIRST_CH;
          if (srcChan <= MAX_OUTPUT_CHANNELS && md->destCh != srcChan) {

            // check whether we need to recompute the current channel later
            bitfield_channels_t upperChansMask = upper_channels_mask(md->destCh);
            bitfield_channels_t srcChanDirtyMask = channel_dirty(dirtyChannels, srcChan);

            // if the source is any of the channels marked as dirty
            // or contained in [ destCh, MAX_OUTPUT_CHANNELS [
            if (srcChanDirtyMask & (passDirtyChannels | upperChansMask)) {
              passDirtyChannels |= channel_bit(md->destCh);
            }

            // if the source has already be computed,
            // then use it!
            if (srcChan < md->destCh || pass > 0) {
              // channels are in [ -1024 * 256, 1024 * 256 ]
              v = chans[srcChan] >> 8;
            }
          }
        }
        if (!mixCondition)
          mixEnabled = v;
      }

      bool applyOffsetAndCurve = true;

      //========== DELAYS ===============
      delayval_t _swOn = mixState[i].now;
      delayval_t _swPrev = mixState[i].prev;
      bool swTog = (mixEnabled > _swOn+DELAY_POS_MARGIN || mixEnabled < _swOn-DELAY_POS_MARGIN);

      if (mode == e_perout_mode_normal && swTog) {
        if (!mixState[i].delay)
          _swPrev = _swOn;
        int32_t precMult = md->delayPrec ? 1 : 10;
        mixState[i].delay = (mixEnabled > _swOn ? md->delayUp : md->delayDown) * precMult;
        mixState[i].now = mixEnabled;
        mixState[i].prev = _swPrev;
      }
      if (mode == e_perout_mode_normal && mixState[i].delay > 0) {
        mixState[i].delay = max<int16_t>(0, (int16_t)mixState[i].delay - tick10ms);
        // Freeze value until delay expires
        if (!mixCondition)
          v = _swPrev;
        else if (mixEnabled)
          continue;
      }
      else {
        if (mode == e_perout_mode_normal) {
          mixState[i].now = mixState[i].prev = mixEnabled;
        }
        if (!mixEnabled) {
          if ((md->speedDown || md->speedUp) && md->mltpx != MLTPX_REPL) {
            if (mixCondition) {
              v = (md->mltpx == MLTPX_ADD ? 0 : RESX);
              applyOffsetAndCurve = false;
            }
          } else if (mixCondition) {
            continue;
          }
        }
      }

      if (mode == e_perout_mode_normal && (!mixCondition || mixEnabled || mixState[i].delay)) {
        if (md->mixWarn) lv_mixWarning |= 1 << (md->mixWarn - 1);
        activeMixes[i] = true;
      }

      if (applyOffsetAndCurve) {
        bool applyTrims = !(mode & e_perout_mode_notrims);
        if (!applyTrims && g_model.thrTrim) {
          auto origin = getSourceTrimOrigin(srcRawAbs);
          if (origin == g_model.getThrottleStickTrimSource() - MIXSRC_FIRST_TRIM) {
            applyTrims = true;
          }
        }
        if (applyTrims && md->carryTrim == 0) {
          v += getSourceTrimValue(srcRaw, v);
        }
      }

      int32_t weight = getSourceNumFieldValue(md->weight, -RESX, RESX);
      weight = calc100to256_16Bits(weight);
      //========== SPEED ===============
      // now its on input side, but without weight compensation. More like other remote controls
      // lower weight causes slower movement

      if (mode <= e_perout_mode_inactive_flight_mode && (md->speedUp || md->speedDown)) { // there are delay values
#define DEL_MULT_SHIFT 8
        // we recale to a mult 256 higher value for calculation
        int32_t tact = act[i];
        int16_t diff = v - (tact>>DEL_MULT_SHIFT);
        if (diff) {
          // open.20.fsguruh: speed is defined in % movement per second; In menu we specify the full movement (-100% to 100%) = 200% in total
          // the unit of the stored value is the value from md->speedUp or md->speedDown * 0.1s; e.g. value 4 means 0.4 seconds
          // because we get a tick each 10msec, we need 100 ticks for one second
          // the value in md->speedXXX gives the time it should take to do a full movement from -100 to 100 therefore 200%. This equals 2048 in recalculated internal range
          if (tick10ms || !s_mixer_first_run_done) {
            // only if already time is passed add or substract a value according the speed configured
            int32_t rate = (int32_t) tick10ms << (DEL_MULT_SHIFT+11);  // = DEL_MULT*2048*tick10ms
            // rate equals a full range for one second; if less time is passed rate is accordingly smaller
            // if one second passed, rate would be 2048 (full motion)*256(recalculated weight)*100(100 ticks needed for one second)
            int32_t currentValue = ((int32_t) v<<DEL_MULT_SHIFT);
            int32_t precMult = md->speedPrec ? 1 : 10;
            if (diff > 0) {
              if (s_mixer_first_run_done && md->speedUp > 0) {
                // if a speed upwards is defined recalculate the new value according configured speed; the higher the speed the smaller the add value is
                int32_t newValue = tact+rate/((int16_t)precMult*md->speedUp);
                if (newValue<currentValue) currentValue = newValue; // Endposition; prevent toggling around the destination
              }
            }
            else {  // if is <0 because ==0 is not possible
              if (s_mixer_first_run_done && md->speedDown > 0) {
                // see explanation in speedUp
                int32_t newValue = tact-rate/((int16_t)precMult*md->speedDown);
                if (newValue>currentValue) currentValue = newValue; // Endposition; prevent toggling around the destination
              }
            }
            act[i] = tact = currentValue;
            // open.20.fsguruh: this implementation would save about 50 bytes code
          } // endif tick10ms ; in case no time passed assign the old value, not the current value from source
          v = (tact >> DEL_MULT_SHIFT);
        }
      }

      //========== CURVES ===============
      if (applyOffsetAndCurve && md->curve.type != CURVE_REF_DIFF && md->curve.value) {
        v = applyCurve(v, md->curve);
      }

      //========== WEIGHT ===============
      int32_t dv = (int32_t)v * weight;
      dv = divRoundClosest(dv, 10);

      //========== OFFSET / AFTER ===============
      if (applyOffsetAndCurve) {
        int32_t offset = getSourceNumFieldValue(md->offset, -RESX, RESX);
        if (offset) dv += divRoundClosest(calc100toRESX_16Bits(offset), 10) << 8;
      }

      //========== DIFFERENTIAL =========
      if (md->curve.type == CURVE_REF_DIFF && md->curve.value) {
        dv = applyCurve(dv, md->curve);
      }

      int32_t * ptr = &chans[md->destCh]; // Save calculating address several times

      // If first mix line for a channel - ignore Multiplex setting
      if (i == 0 || mixAddress(i - 1)->destCh != md->destCh) {
        *ptr = dv;
      } else {
        switch (md->mltpx) {
          case MLTPX_REPL:
            *ptr = dv;
            if (mode == e_perout_mode_normal) {
              for (int8_t m = i - 1; m >= 0 && mixAddress(m)->destCh == md->destCh; m--)
                activeMixes[m] = false;
            }
            break;
          case MLTPX_MUL:
            // @@@2 we have to remove the weight factor of 256 in case of 100%; now we use the new base of 256
            dv >>= 8;
            dv *= *ptr;
            dv >>= RESX_SHIFT;   // same as dv /= RESXl;
            *ptr = dv;
            break;
          default: // MLTPX_ADD
            *ptr += dv; //Mixer output add up to the line (dv + (dv>0 ? 100/2 : -100/2))/(100);
            break;
        } // endswitch md->mltpx
      }
#ifdef PREVENT_ARITHMETIC_OVERFLOW
/*
      // a lot of assumptions must be true, for this kind of check; not really worth for only 4 bytes flash savings
      // this solution would save again 4 bytes flash
      int8_t testVar=(*ptr<<1)>>24;
      if ( (testVar!=-1) && (testVar!=0 ) ) {
        // this devices by 64 which should give a good balance between still over 100% but lower then 32x100%; should be OK
        *ptr >>= 6;  // this is quite tricky, reduces the value a lot but should be still over 100% and reduces flash need
      } */


      PACK( union u_int16int32_t {
        struct {
          int16_t lo;
          int16_t hi;
        } words_t;
        int32_t dword;
      });

      u_int16int32_t tmp;
      tmp.dword=*ptr;

      if (tmp.dword<0) {
        if ((tmp.words_t.hi&0xFF80)!=0xFF80) tmp.words_t.hi=0xFF86; // set to min nearly
      }
      else {
        if ((tmp.words_t.hi|0x007F)!=0x007F) tmp.words_t.hi=0x0079; // set to max nearly
      }
      *ptr = tmp.dword;
      // this implementation saves 18bytes flash

/*      dv=*ptr>>8;
      if (dv>(32767-RESXl)) {
        *ptr=(32767-RESXl)<<8;
      } else if (dv<(-32767+RESXl)) {
        *ptr=(-32767+RESXl)<<8;
      }*/
      // *ptr=limit( int32_t(int32_t(-1)<<23), *ptr, int32_t(int32_t(1)<<23));  // limit code cost 72 bytes
      // *ptr=limit( int32_t((-32767+RESXl)<<8), *ptr, int32_t((32767-RESXl)<<8));  // limit code cost 80 bytes
#endif
    } //endfor mixers

    tick10ms = 0;
    dirtyChannels &= passDirtyChannels;

  } while (++pass < 5 && dirtyChannels);

  for (uint8_t i=0; i<MAX_MIXERS; i++)
    mixState[i].activeMix = activeMixes[i];

  mixWarning = lv_mixWarning;
}

