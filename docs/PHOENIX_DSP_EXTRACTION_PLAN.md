# Phoenix-DSP Extraction Plan

**Goal:** Extract production-tested DSP algorithms from phoenix-wwv to phoenix-dsp library  
**Impact:** +400% functionality increase for phoenix-dsp (from 5 to 13 primitives)  
**Timeline:** 4 weeks (19 hours total effort)  
**Date Started:** 2025-12-24  
**Status:** 🔄 **IN PROGRESS - Prerequisites**

---

## OVERVIEW

Extract 8 battle-tested DSP algorithms (~450 lines) from phoenix-wwv to create shared foundation for Phoenix suite.

**Approved Specifications:**
- ✅ Build test infrastructure FIRST (before extraction)
- ✅ Support BOTH runtime AND precomputed coefficient modes
- ✅ Support BOTH heap (malloc) AND stack (user buffer) allocation

**Target Releases:**
- phoenix-dsp v0.2.0 - FFT Analysis Tools (Week 1)
- phoenix-dsp v0.3.0 - Advanced Filtering (Week 2)
- phoenix-dsp v0.4.0 - Specialized Algorithms (Week 3)
- Integration validation (Week 4)

---

## PROGRESS TRACKER

### Prerequisites: Test Infrastructure (2 hours) - IN PROGRESS
- [ ] Create phoenix-dsp/test/test_advanced_dsp.c
- [ ] Generate reference test vectors (Python: MATLAB/scipy/numpy)
- [ ] Test framework: cascade, Goertzel, windows, peaks, comb, noise
- [ ] All tests run and fail (no implementations yet)

### Week 1: FFT Analysis Tools (3 hours) → v0.2.0
- [ ] Extract Hann window (fft_processor.c:35-41)
- [ ] Extract Blackman-Harris window (tone_fft_helpers.c:16-28)
- [ ] Add Hamming window (new)
- [ ] Add window application helpers
- [ ] Extract parabolic interpolation (tone_fft_helpers.c:34-50)
- [ ] Extract peak finding (tone_fft_helpers.c:55-72)
- [ ] Add combined peak+interpolation function
- [ ] Validate all windows vs numpy/scipy
- [ ] Test parabolic interpolation with synthetic peaks
- [ ] Documentation: windowing theory, spectrum analyzer example
- [ ] Release phoenix-dsp v0.2.0

### Week 2: Advanced Filtering (8 hours) → v0.3.0
**Cascaded Biquad:**
- [ ] Extract biquad_process (channel_filters.c:32-40)
- [ ] Extract Butterworth LP coefficients (bcd_envelope.c:87-108)
- [ ] Add Butterworth HP coefficients (new)
- [ ] Create pn_cascade_t struct
- [ ] Implement pn_cascade_init (runtime mode) ✅ APPROVED
- [ ] Implement pn_cascade_init_sos (precomputed mode) ✅ APPROVED
- [ ] Implement pn_cascade_process
- [ ] Test 2nd/4th/6th/8th order vs MATLAB butter()
- [ ] Test precomputed mode with WWV coefficients

**Goertzel Detector:**
- [ ] Deduplicate from bcd_envelope.c:155-181 and subcarrier_detector.c
- [ ] Create pn_goertzel_t struct
- [ ] Implement pn_goertzel_init
- [ ] Implement pn_goertzel_process (return bool on block complete)
- [ ] Implement pn_goertzel_magnitude
- [ ] Implement pn_goertzel_reset
- [ ] Test 100Hz @ 2400Hz vs scipy.signal.goertzel
- [ ] Test edge cases (DC, Nyquist-1, various block sizes)
- [ ] Documentation: Goertzel theory, DTMF example
- [ ] Release phoenix-dsp v0.3.0

### Week 3: Specialized Algorithms (4 hours) → v0.4.0
**Comb Filter:**
- [ ] Extract from tick_comb_filter.c
- [ ] Create pn_comb_t struct
- [ ] Implement pn_comb_create (heap allocation) ✅ APPROVED
- [ ] Implement pn_comb_init (stack/user buffer) ✅ APPROVED
- [ ] Parameterize delay_samples (remove COMB_STAGES constant)
- [ ] Parameterize alpha (remove hardcoded 0.99)
- [ ] Implement pn_comb_process
- [ ] Implement pn_comb_reset, pn_comb_destroy
- [ ] Test impulse response (comb pattern)
- [ ] Test 1000Hz enhancement at 50kHz
- [ ] Test memory leaks (create/destroy 1000x)
- [ ] Document memory requirements

**Noise Floor:**
- [ ] Extract pn_estimate_noise_floor (tone_fft_helpers.c:77-103)
- [ ] Generalize bin sampling (remove hardcoded ranges)
- [ ] Add pn_calculate_snr_db helper
- [ ] Test with known signal + noise
- [ ] Test edge cases (signal at bin 0, bin N-1)
- [ ] Release phoenix-dsp v0.4.0

### Week 4: Integration & Validation (2 hours)
**Phoenix-WWV Updates:**
- [ ] Add phoenix-dsp as git submodule
- [ ] Update CMakeLists.txt (link pn_dsp)
- [ ] Replace channel_filters.c → pn_cascade_t
- [ ] Replace bcd_envelope.c Goertzel → pn_goertzel_t
- [ ] Replace subcarrier_detector.c Goertzel → pn_goertzel_t
- [ ] Replace fft_processor.c window → pn_window_hann
- [ ] Replace tone_fft_helpers.c window → pn_window_blackman_harris
- [ ] Replace tone_fft_helpers.c peaks → pn_parabolic_interpolate
- [ ] Replace tick_comb_filter.c → pn_comb_t

**Validation:**
- [ ] Process known WWV recording (wwv_5mhz_clean.wav)
- [ ] Compare CSV outputs vs baseline (must be identical)
- [ ] Performance benchmark (hot paths ±2% tolerance)
- [ ] Memory leak check (valgrind)
- [ ] Update phoenix-wwv README (add phoenix-dsp dependency)
- [ ] Update CONTRIBUTING_TO_PHOENIX_DSP.md (mark completed)

**Phoenix-DSP Documentation:**
- [ ] README: Add all new algorithms
- [ ] API reference: Document all new functions
- [ ] Examples: DTMF decoder, pitch detector, spectrum analyzer
- [ ] Release notes for v0.2.0, v0.3.0, v0.4.0
- [ ] Git tags: v0.2.0, v0.3.0, v0.4.0

---

## ALGORITHM INVENTORY

| Priority | Algorithm | Source | Lines | Effort | Deliverable |
|----------|-----------|--------|-------|--------|-------------|
| P0 | Hann Window | fft_processor.c:35-41 | 10 | 0.5h | v0.2.0 |
| P0 | Blackman-Harris | tone_fft_helpers.c:16-28 | 15 | 0.5h | v0.2.0 |
| P0 | Parabolic Interp | tone_fft_helpers.c:34-50 | 20 | 0.5h | v0.2.0 |
| P0 | Peak Finding | tone_fft_helpers.c:55-72 | 20 | 0.5h | v0.2.0 |
| P0 | Cascaded Biquad | channel_filters.c:32-40 | 60 | 3h | v0.3.0 |
| P0 | Goertzel DFT | bcd_envelope.c:155-181 | 45 | 3h | v0.3.0 |
| P1 | Comb Filter | tick_comb_filter.c | 50 | 2h | v0.4.0 |
| P2 | Noise Floor | tone_fft_helpers.c:77-103 | 30 | 1h | v0.4.0 |

**Total:** 250 lines of pure DSP + ~200 lines infrastructure = 450 lines

---

## TESTING STRATEGY

### Reference Implementations

| Algorithm | Reference | Validation Method |
|-----------|-----------|-------------------|
| Butterworth Coeffs | MATLAB `butter()` | Coefficient comparison |
| Hann Window | numpy.hanning | Coefficient comparison |
| Blackman-Harris | scipy.signal.blackmanharris | Sidelobe levels (-92dB) |
| Goertzel | scipy.signal.goertzel | Magnitude accuracy |
| Parabolic Interp | Synthetic peaks | Sub-bin accuracy |
| Comb Filter | MATLAB `comb()` | Frequency response |
| Noise Floor | Synthetic signal+noise | SNR accuracy |

---

## EFFORT SUMMARY

| Phase | Hours | Deliverable |
|-------|-------|-------------|
| **Prerequisites** | 2 | Test suite ready |
| **Week 1** | 3 | phoenix-dsp v0.2.0 |
| **Week 2** | 8 | phoenix-dsp v0.3.0 |
| **Week 3** | 4 | phoenix-dsp v0.4.0 |
| **Week 4** | 2 | Integration complete |
| **Total** | **19 hours** | Complete ecosystem |

---

## RELATED DOCUMENTS

- [CONTRIBUTING_TO_PHOENIX_DSP.md](CONTRIBUTING_TO_PHOENIX_DSP.md) - Analysis of contribution value
- [PHOENIX_DSP_INTEGRATION_EVALUATION.md](PHOENIX_DSP_INTEGRATION_EVALUATION.md) - Why NOT to integrate phoenix-dsp into WWV
- [DSP_FFT_ALGORITHMS.md](DSP_FFT_ALGORITHMS.md) - Complete algorithm reference with line numbers
- [refactor_progress.md](refactor_progress.md) - Phoenix-WWV internal refactoring (Phases 0-10)

---

## ISSUES / NOTES

**2025-12-24:** Plan created, all specifications approved. Ready to begin prerequisites.

**Next Steps:**
1. Clone phoenix-dsp repository
2. Create feature branch: `feature/wwv-dsp-extraction`
3. Set up test infrastructure
4. Generate reference test vectors

---

**Last Updated:** 2025-12-24  
**Status:** 🔄 Prerequisites in progress  
**Next Milestone:** Test Infrastructure Setup (2 hours)
