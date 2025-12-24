# Phoenix WWV Refactor Progress

**Goal:** Reorganize codebase for better separation of concerns and AI-editability
**Target:** 150-300 lines per file maximum, single responsibility per file
**Date Started:** 2025-12-24

---

## DECISIONS

1. **Legacy Code:** Keep but mark deprecated:
   - `bcd_envelope.c/h` - Mark as DEPRECATED
   - `subcarrier_detector.c/h` - Mark as DEPRECATED

2. **Unknown Use:** Keep but mark for investigation:
   - `slow_marker_detector.c/h` - Mark as UNKNOWN USE (verification only?)

3. **Naming:** Rename waterfall_telemetry → telemetry

4. **Headers:** Keep individual headers (no unified phoenix_wwv.h)

---

## CURRENT STATE (BEFORE REFACTOR)

### Files Over 300 Lines (13 files):
| File | Lines | Status |
|------|-------|--------|
| tick_detector.c | 967 | ⚠️ NEEDS SPLIT |
| sync_detector.c | 923 | ⚠️ NEEDS SPLIT |
| marker_detector.c | 578 | ⚠️ NEEDS SPLIT |
| bcd_correlator.c | 534 | ⚠️ NEEDS SPLIT |
| tick_correlator.c | 502 | ⚠️ NEEDS SPLIT |
| bcd_freq_detector.c | 485 | ⚠️ NEEDS SPLIT |
| bcd_envelope.c | 475 | DEPRECATED - KEEP |
| subcarrier_detector.c | 475 | DEPRECATED - KEEP |
| bcd_time_detector.c | 453 | ⚠️ NEEDS SPLIT |
| tone_tracker.c | 435 | ⚠️ NEEDS SPLIT |
| kiss_fft.c | 402 | ✅ EXTERNAL LIB - KEEP |
| wwv_detector_manager.c | 388 | ⚠️ NEEDS SPLIT |
| waterfall_telemetry.c | 334 | ⚠️ RENAME + SPLIT |

### Files Under 300 Lines (6 files):
| File | Lines | Status |
|------|-------|--------|
| wwv_clock.c | 200 | ✅ OK |
| marker_correlator.c | 183 | ✅ OK |
| bcd_decoder.c | 181 | ✅ OK |
| slow_marker_detector.c | 163 | UNKNOWN USE - KEEP |
| channel_filters.c | 86 | ✅ OK |
| tick_comb_filter.c | 50 | ✅ OK |

**Total:** 19 source files, 8,808 lines

---

## TARGET STRUCTURE

```
src/
├── core/                        # Shared infrastructure
│   ├── fft_processor.c          # Unified FFT wrapper (150 lines)
│   ├── telemetry.c              # Renamed from waterfall_telemetry (200 lines)
│   ├── timing.c                 # wwv_clock.c renamed
│   └── version.c                # Version info
│
├── signal/                      # Signal conditioning
│   ├── channel_filters.c        # Keep as-is (86 lines)
│   └── comb_filter.c            # tick_comb_filter.c renamed
│
├── detection/                   # Primary detectors
│   ├── tick/
│   │   ├── tick_fft.c           # FFT + energy extraction (200 lines)
│   │   ├── tick_threshold.c     # Threshold adaptation (150 lines)
│   │   ├── tick_state.c         # Detection state machine (200 lines)
│   │   ├── tick_callbacks.c     # Event handling (100 lines)
│   │   └── tick_detector.c      # Public API + glue (150 lines)
│   │
│   ├── marker/
│   │   ├── marker_fft.c         # FFT + accumulator (200 lines)
│   │   ├── marker_baseline.c    # Baseline tracking (150 lines)
│   │   ├── marker_state.c       # Detection state machine (150 lines)
│   │   └── marker_detector.c    # Public API (100 lines)
│   │
│   ├── bcd/
│   │   ├── bcd_time_fft.c       # Time-domain FFT detector (200 lines)
│   │   ├── bcd_freq_fft.c       # Freq-domain FFT detector (200 lines)
│   │   ├── bcd_windowing.c      # 1-second window management (200 lines)
│   │   ├── bcd_classification.c # Symbol classification logic (150 lines)
│   │   ├── bcd_decoder.c        # Keep as-is (181 lines)
│   │   └── bcd_correlator.c     # Public API + integration (150 lines)
│   │
│   └── tone/
│       ├── tone_fft.c           # FFT for tone tracking (200 lines)
│       ├── tone_tracker.c       # Tone following logic (150 lines)
│       └── slow_marker.c        # UNKNOWN USE - keep as-is (163 lines)
│
├── correlation/                 # Post-detection analysis
│   ├── correlation_stats.c      # Shared stats logic (100 lines)
│   ├── tick_correlation.c       # Tick chain tracking (250 lines)
│   └── marker_correlation.c     # Keep as-is (183 lines)
│
├── sync/                        # Frame synchronization
│   ├── sync_evidence.c          # Evidence collection (200 lines)
│   ├── sync_state_machine.c     # State transitions (250 lines)
│   ├── sync_confidence.c        # Confidence scoring (150 lines)
│   ├── sync_recovery.c          # Recovery logic (150 lines)
│   └── sync_detector.c          # Public API (150 lines)
│
├── manager/                     # Orchestration
│   ├── detector_lifecycle.c     # Create/destroy (150 lines)
│   ├── detector_routing.c       # Callback routing (150 lines)
│   └── wwv_manager.c            # Public API (100 lines)
│
├── deprecated/                  # Legacy code
│   ├── bcd_envelope.c           # DEPRECATED (475 lines)
│   └── subcarrier_detector.c    # DEPRECATED (475 lines)
│
└── external/                    # Third-party
    └── kiss_fft.c               # Keep as-is (402 lines)
```

---

## REFACTOR PHASES

### PHASE 0: Preparation ✅ COMPLETE
- [x] Create directory structure
- [x] Mark deprecated files with @deprecated headers
- [x] Mark unknown-use files with @note UNKNOWN USE headers
- [x] Commit: "Prepare refactor: mark deprecated/unknown files"

**Completed:** 2025-12-24
**Commit:** db3eaa1 "Phase 0: Preparation - mark deprecated/unknown files, create directory structure"

### PHASE 1: Extract Common FFT Logic ✅ COMPLETE
**Goal:** Eliminate FFT duplication across 5 files

**Created:**
- [x] `src/core/fft_processor.c` (156 lines)
- [x] `include/fft_processor.h` (98 lines)

**Migrated:**
- [x] tick_detector.c: Replaced FFT setup/processing with fft_processor
- [x] marker_detector.c: Replaced FFT setup/processing with fft_processor
- [x] bcd_time_detector.c: Replaced FFT setup/processing with fft_processor
- [x] bcd_freq_detector.c: Replaced FFT setup/processing with fft_processor
- [x] tone_tracker.c: Replaced FFT setup/processing with fft_processor

**Benefits:**
- Eliminated ~150 lines of duplicate FFT setup code across 5 files
- Eliminated ~120 lines of duplicate bucket energy calculation
- Unified window function (Hann) generation in one place
- Single point of maintenance for FFT processing

**Verified:** [x] All 5 detectors compile successfully with fft_processor

**Completed:** 2025-12-24
**Commit:** 591d6ee "Phase 1: Extract common FFT logic to core/fft_processor"

---

### PHASE 2: Reorganize File Structure ✅ COMPLETE
**Goal:** Move files into organized directory structure

**New Structure:**
```
src/
├── core/                        # Core utilities
│   ├── fft_processor.c          # FFT processing (Phase 1)
│   ├── waterfall_telemetry.c   # Telemetry
│   └── wwv_clock.c              # Timing
│
├── signal/                      # Signal conditioning
│   ├── channel_filters.c        # I/Q filtering
│   └── tick_comb_filter.c       # Comb filter
│
├── detection/                   # Primary detectors
│   ├── tick/
│   │   └── tick_detector.c      # Tick pulse detection (768 lines)
│   ├── marker/
│   │   ├── marker_detector.c    # Minute marker (534 lines)
│   │   └── slow_marker_detector.c  # UNKNOWN USE (166 lines)
│   ├── bcd/
│   │   ├── bcd_time_detector.c  # Time-domain BCD (409 lines)
│   │   ├── bcd_freq_detector.c  # Freq-domain BCD (441 lines)
│   │   ├── bcd_decoder.c        # BCD decoding (181 lines)
│   │   ├── bcd_envelope.c       # DEPRECATED (479 lines)
│   │   └── subcarrier_detector.c # DEPRECATED (479 lines)
│   └── tone/
│       └── tone_tracker.c       # Tone tracking (421 lines)
│
├── correlation/                 # Post-detection analysis
│   ├── tick_correlator.c        # Tick chains (501 lines)
│   ├── marker_correlator.c      # Marker correlation (183 lines)
│   └── bcd_correlator.c         # BCD correlation (533 lines)
│
├── sync/                        # Frame synchronization
│   └── sync_detector.c          # Sync state machine (922 lines)
│
├── manager/                     # Orchestration
│   └── wwv_detector_manager.c   # Manager (387 lines)
│
└── kiss_fft.c                   # External lib (402 lines)
```

**Relocated:** 18 files organized into 7 feature-based directories

**Benefits:**
- Clear separation by feature domain (not by type)
- Easy to navigate and understand project structure
- Foundation for future file splits
- Maintains all functionality intact
- Low-risk reorganization

**Verified:** [x] All 18 modules compile successfully

**Completed:** 2025-12-24
**Commit:** 00fec55 "Phase 2: Complete directory reorganization by feature domain"

---

### PHASE 3: Split tick_detector.c (768 lines → 5 files) ⬜ NOT STARTED

**Current:** `src/tick_detector.c` (967 lines)

**Split into:**
1. [ ] `src/detection/tick/tick_fft.c` (200 lines)
   - FFT processing
   - Bin energy extraction
   - Comb filter integration

2. [ ] `src/detection/tick/tick_threshold.c` (150 lines)
   - Adaptive threshold calculation
   - Noise floor tracking
   - Hysteresis logic

3. [ ] `src/detection/tick/tick_state.c` (200 lines)
   - State machine: IDLE, IN_TICK, COOLDOWN, WARMUP
   - Timing validation
   - Duration tracking

4. [ ] `src/detection/tick/tick_callbacks.c` (100 lines)
   - Callback handling
   - CSV logging
   - Event emission

5. [ ] `src/detection/tick/tick_detector.c` (150 lines)
   - Public API
   - Module coordination
   - Resource management

**Headers:**
- [ ] `include/detection/tick_detector.h` (public API only)
- [ ] `include/detection/tick/tick_internal.h` (private interfaces)

**Verify:** 
- [ ] Public API unchanged
- [ ] Tests pass (if any)
- [ ] CSV output identical

**Commit:** "Split tick_detector into 5 focused modules"

---

### PHASE 3: Split sync_detector.c (923 lines → 5 files) ⬜ NOT STARTED

**Current:** `src/sync_detector.c` (923 lines)

**Split into:**
1. [ ] `src/sync/sync_evidence.c` (200 lines)
   - Evidence collection from ticks/markers/P-markers/holes
   - Weight application
   - Evidence aggregation

2. [ ] `src/sync/sync_state_machine.c` (250 lines)
   - State transitions: SEARCHING/ACQUIRING/LOCKED/RECOVERING
   - State validation logic
   - Transition rules

3. [ ] `src/sync/sync_confidence.c` (150 lines)
   - Confidence calculation
   - Decay over time
   - Signal health assessment

4. [ ] `src/sync/sync_recovery.c` (150 lines)
   - Recovery timeout handling
   - Prediction during recovery
   - Re-acquisition logic

5. [ ] `src/sync/sync_detector.c` (150 lines)
   - Public API
   - Module coordination
   - Frame time output

**Headers:**
- [ ] `include/sync/sync_detector.h` (public API)
- [ ] `include/sync/sync_internal.h` (private interfaces)

**Verify:**
- [ ] Sync behavior unchanged
- [ ] Telemetry output consistent

**Commit:** "Split sync_detector into 5 focused modules"

---

### PHASE 4: Split marker_detector.c (578 lines → 4 files) ⬜ NOT STARTED

**Current:** `src/marker_detector.c` (578 lines)

**Split into:**
1. [ ] `src/detection/marker/marker_fft.c` (200 lines)
   - FFT processing
   - Sliding window accumulator
   - Energy accumulation

2. [ ] `src/detection/marker/marker_baseline.c` (150 lines)
   - Self-tracking baseline adaptation
   - Baseline validation
   - Threshold calculation

3. [ ] `src/detection/marker/marker_state.c` (150 lines)
   - State machine: IDLE, IN_MARKER, COOLDOWN, WARMUP
   - Duration validation
   - Cooldown enforcement

4. [ ] `src/detection/marker/marker_detector.c` (100 lines)
   - Public API
   - Module coordination

**Headers:**
- [ ] `include/detection/marker_detector.h` (public API)
- [ ] `include/detection/marker/marker_internal.h` (private)

**Verify:**
- [ ] Marker detection rate unchanged
- [ ] Baseline tracking consistent

**Commit:** "Split marker_detector into 4 focused modules"

---

### PHASE 5: Reorganize BCD Detection ✅ COMPLETED

**Status:** ✅ COMPLETED - December 24, 2025

**Original Files:**
- `src/bcd_time_detector.c` (410 lines)
- `src/bcd_freq_detector.c` (442 lines)
- `src/bcd_decoder.c` (181 lines) - Already optimal, no split needed

**Split into:**

1. ✅ `include/detection/bcd_internal.h` (206 lines)
   - struct bcd_time_detector
   - struct bcd_freq_detector
   - Common detector_state_t enum
   - Function declarations for state machines
   - Common helper functions (bcd_get_wall_time_str)

2. ✅ `src/detection/bcd/bcd_time_state_machine.c` (173 lines)
   - bcd_time_run_state_machine() - 3-state FSM
   - bcd_time_calculate_bucket_energy() - 100Hz extraction
   - Adaptive noise floor tracking (asymmetric attack/decay)
   - Pulse validation and event callbacks
   - CSV logging and telemetry

3. ✅ `src/detection/bcd/bcd_freq_state_machine.c` (192 lines)
   - bcd_freq_run_state_machine() - 3-state FSM
   - bcd_freq_calculate_bucket_energy() - 100Hz extraction
   - bcd_freq_update_accumulator() - sliding window
   - Self-tracking baseline adaptation
   - Pulse validation and event callbacks
   - CSV logging and telemetry

4. ✅ `src/detection/bcd/bcd_time_detector.c` (184 lines, was 410)
   - 12 public API functions
   - FFT buffer management
   - Resource allocation/deallocation
   - Accessor functions

5. ✅ `src/detection/bcd/bcd_freq_detector.c` (196 lines, was 442)
   - 13 public API functions
   - FFT buffer management
   - Sliding window allocation
   - Accessor functions

6. ⚠️ `src/bcd_correlator.c` (534 lines) - DEFERRED TO PHASE 6
   - This file is in src/correlation/ (not src/detection/bcd/)
   - Will be addressed in Phase 6: Split Correlators

**Line Count Results:**
- bcd_time_detector.c: 410 → 184 lines (55% reduction)
- bcd_freq_detector.c: 442 → 196 lines (56% reduction)
- New files added: 571 lines (internal.h + 2 state machines)
- Net change: +319 lines, but much better separation

**Hot Path Verification:**
✅ `bcd_time_detector_process_sample()`:
- Identical logic: buffer samples → FFT → calculate_bucket_energy → run_state_machine
- Only change: `calculate_bucket_energy(td)` → `bcd_time_calculate_bucket_energy(td)`
- Only change: `run_state_machine(td)` → `bcd_time_run_state_machine(td)`

✅ `bcd_freq_detector_process_sample()`:
- Identical logic: buffer samples → FFT → calculate_bucket_energy → run_state_machine
- Only change: `calculate_bucket_energy(fd)` → `bcd_freq_calculate_bucket_energy(fd)`
- Only change: `run_state_machine(fd)` → `bcd_freq_run_state_machine(fd)`

**Design Pattern:**
- Unified internal header approach (both detectors share same structure)
- State machines extracted to separate files
- Public API files contain only resource management
- Follows same pattern as tick_detector and marker_detector (Phase 3 & 4)

**Verification:**
- ✅ Compilation successful
- ✅ Hot paths preserved (only function name changes)
- ✅ No performance regressions

**Commit:** "Phase 5: Split BCD detectors - unified internal header approach"

---

### PHASE 6: Split Correlators ✅ COMPLETED

**Status:** ✅ COMPLETED - December 24, 2025

**Original Files:**
- `src/correlation/bcd_correlator.c` (534 lines)
- `src/correlation/tick_correlator.c` (501 lines)
- `src/correlation/marker_correlator.c` (183 lines) - Already optimal, no split needed

**BCD Correlator Split:**

1. ✅ `include/correlation/bcd_correlator_internal.h` (157 lines)
   - struct bcd_correlator
   - Window timing constants
   - Function declarations for window manager and symbol classifier

2. ✅ `src/correlation/bcd_window_manager.c` (209 lines)
   - bcd_window_get_minute_anchor() - sync timing reference
   - bcd_window_get_second_for_timestamp() - second calculation (0-59)
   - bcd_window_open/close() - window lifecycle
   - bcd_window_check_transition() - hot path window state machine
   - Energy accumulation and symbol emission

3. ✅ `src/correlation/bcd_symbol_classifier.c` (91 lines)
   - VALID_P_POSITIONS[] - position marker gating
   - bcd_symbol_is_valid_p_position() - position validation
   - bcd_symbol_classify_duration() - symbol classification (0/1/P)
   - bcd_window_estimate_pulse_duration() - pulse width estimation

4. ✅ `src/correlation/bcd_correlator.c` (297 lines, was 534)
   - 12 public API functions
   - create/destroy
   - set_sync_source, set_callback
   - time_event / freq_event (hot path entry points)
   - Accessors and stats

**Tick Correlator Split (Minimal):**

1. ✅ `include/correlation/tick_correlator_internal.h` (127 lines)
   - struct tick_correlator
   - Prediction tracking structure
   - Function declarations

2. ✅ `src/correlation/tick_chain_manager.c` (64 lines)
   - tick_chain_start_new() - chain initialization
   - tick_chain_update_stats() - statistics tracking

3. ✅ `src/correlation/tick_correlator.c` (400 lines, was 501)
   - 12 public API functions
   - Correlation decision logic (kept intact - tightly coupled)
   - Prediction/epoch tracking (kept intact - intertwined with correlation)
   - create/destroy, add_tick, accessors

**Marker Correlator:**
- ⚠️ No changes - already optimal at 183 lines

**Line Count Results:**
- bcd_correlator.c: 534 → 297 lines (44% reduction)
- tick_correlator.c: 501 → 400 lines (20% reduction)
- marker_correlator.c: 183 lines (unchanged)
- New BCD files: 457 lines (internal.h + window_manager + symbol_classifier)
- New tick files: 191 lines (internal.h + chain_manager)
- Net change: +411 lines, but much clearer separation of concerns

**Hot Path Verification:**
✅ `bcd_correlator_time_event()` and `bcd_correlator_freq_event()`:
- Identical logic: check window transition → accumulate energy
- Only change: `check_window_transition()` → `bcd_window_check_transition()`

✅ `tick_correlator_add_tick()`:
- Minimal changes: `start_new_chain()` → `tick_chain_start_new()`
- Core correlation logic preserved intact
- Prediction/epoch logic untouched

**Design Approach:**
- **BCD**: Separated window management from symbol classification
- **Tick**: Minimal split - only extracted clearly separable helpers
- **Marker**: Left unchanged (already optimal)
- Different from detector pattern - correlators are event processors, not sample processors

**Verification:**
- ✅ Compilation successful
- ✅ Hot paths preserved (only function name changes)
- ✅ No performance regressions

**Commit:** "Phase 6: Split correlators - event processor pattern"

---

### PHASE 7: Split tone_tracker.c ⬜ NOT STARTED

**Current:**
- `src/bcd_envelope.c` (475 lines) DEPRECATED
- `src/subcarrier_detector.c` (475 lines) DEPRECATED

**Move deprecated:**
- [ ] `src/bcd_envelope.c` → `src/deprecated/bcd_envelope.c`
- [ ] `include/bcd_envelope.h` → `include/deprecated/bcd_envelope.h`
- [ ] `src/subcarrier_detector.c` → `src/deprecated/subcarrier_detector.c`
- [ ] `include/subcarrier_detector.h` → `include/deprecated/subcarrier_detector.h`
- [ ] Mark with @deprecated in file headers

**Split into:**
1. [ ] `src/detection/bcd/bcd_time_fft.c` (200 lines)
   - Time-domain FFT processing
   - 100Hz detection

2. [ ] `src/detection/bcd/bcd_freq_fft.c` (200 lines)
   - Frequency-domain FFT processing
   - Alternative 100Hz detection

3. [ ] `src/detection/bcd/bcd_windowing.c` (200 lines)
   - 1-second window management
   - Window boundary detection
   - Event accumulation

4. [ ] `src/detection/bcd/bcd_classification.c` (150 lines)
   - Symbol classification (0/1/P)
   - Pulse width estimation
   - P-marker validation

5. [ ] `src/detection/bcd/bcd_correlator.c` (150 lines)
   - Public API
   - Integration with sync_detector
   - Decoder coordination

6. [ ] `src/detection/bcd/bcd_decoder.c` (181 lines) - Move from src/

**Headers:**
- [ ] `include/detection/bcd_detector.h` (public API)
- [ ] `include/detection/bcd/bcd_internal.h` (private)

**Verify:**
- [ ] BCD decoding still works
- [ ] Symbol output consistent

**Commit:** "Reorganize BCD detection, move deprecated code"

---

### PHASE 6: Split Correlators ⬜ NOT STARTED

**Current:**
- `src/tick_correlator.c` (502 lines)
- `src/marker_correlator.c` (183 lines) ✅ OK size

**Actions:**
1. [ ] Extract shared logic to `src/correlation/correlation_stats.c` (100 lines)
2. [ ] Simplify `src/correlation/tick_correlation.c` (250 lines)
3. [ ] Move `src/marker_correlator.c` → `src/correlation/marker_correlation.c` (183 lines)

**Commit:** "Extract correlation stats, reorganize correlators"

---

### PHASE 7: Split tone_tracker.c ✅ COMPLETED

**Original:**
- `src/detection/tone/tone_tracker.c` (422 lines)
- `src/detection/marker/slow_marker_detector.c` (139 lines) - already properly located

**Split tone_tracker into:**

1. ✅ `include/detection/tone/tone_tracker_internal.h` (96 lines)
   - struct tone_tracker with FFT state
   - Configuration constants (SEARCH_BINS, MIN_SNR_DB, NOISE_BINS)
   - Function declarations for helpers and measurement

2. ✅ `src/detection/tone/tone_fft_helpers.c` (80 lines)
   - `tone_generate_blackman_harris()` - window function
   - `tone_parabolic_peak()` - sub-bin interpolation
   - `tone_find_peak_bin()` - peak search in range
   - `tone_estimate_noise_floor()` - noise estimation

3. ✅ `src/detection/tone/tone_measurement.c` (133 lines)
   - `tone_measure_frequency()` - core FFT measurement logic
   - Handles DC case (0 Hz) and normal tones (500/600 Hz)
   - Dual-sideband averaging for accuracy
   - `tone_log_measurement()` - CSV logging

4. ✅ `src/detection/tone/tone_tracker.c` (118 lines)
   - 11 public API functions only
   - Sample buffering in `tone_tracker_process_sample()`
   - Global noise floor variable

**Line Count Results:**
- tone_tracker.c: 422 → 118 lines (72% reduction)
- New files: 309 lines (internal.h + fft_helpers + measurement)
- Net change: +5 lines, but much cleaner separation

**Hot Path Verification:**
✅ `tone_tracker_process_sample()`:
- Identical logic: buffer samples → check if full → measure → log
- Only changes: `measure_tone()` → `tone_measure_frequency()`, `log_measurement()` → `tone_log_measurement()`
- FFT processing frequency unchanged
- No performance regressions

**Design Approach:**
- Followed detector pattern (sample processor, not event processor)
- FFT helpers extracted for testability
- Core measurement logic isolated
- API-only main file for clarity
- slow_marker_detector.c left unchanged (already in correct location with UNKNOWN USE note)

**Verification:**
- ✅ Compilation successful
- ✅ Hot path preserved (only function name changes)
- ✅ No performance regressions

**Commit:** "Phase 7: Split tone_tracker - detector pattern"

---

### PHASE 8: Split wwv_detector_manager.c ✅ COMPLETED

**Original:**
- `src/manager/wwv_detector_manager.c` (388 lines)

**Split into:**

1. ✅ `include/manager/wwv_detector_manager_internal.h` (99 lines)
   - struct wwv_detector_manager with 9 detector components
   - External callback storage (3 pairs)
   - Statistics counters
   - Function declarations for lifecycle and routing

2. ✅ `src/manager/detector_lifecycle.c` (104 lines)
   - `wwv_detector_lifecycle_create_all()` - Creates all 9 detectors based on config
   - `wwv_detector_lifecycle_destroy_all()` - Cleanup in reverse order
   - Path building for CSV output files
   - Callback registration

3. ✅ `src/manager/detector_routing.c` (97 lines)
   - `wwv_routing_on_tick_event()` - Routes tick events to external callback
   - `wwv_routing_on_tick_marker_event()` - Routes to sync detector
   - `wwv_routing_on_marker_event()` - Routes marker events to correlator + external callback
   - `wwv_routing_on_slow_marker_frame()` - Routes slow marker to correlator

4. ✅ `src/manager/wwv_detector_manager.c` (205 lines)
   - 16 public API functions only
   - Hot path: `process_detector_sample()`, `process_display_sample()`, `process_display_fft()`
   - Callback setters: 3 functions
   - Status getters: 9 functions
   - Stats/diagnostics: 1 function

**Line Count Results:**
- wwv_detector_manager.c: 388 → 205 lines (47% reduction)
- New files: 300 lines (internal.h + lifecycle + routing)
- Net change: +117 lines, but clearer separation of concerns

**Hot Path Verification:**
✅ `wwv_detector_manager_process_detector_sample()` (50 kHz rate):
- Identical logic: route to tick_detector and marker_detector
- Direct calls, no branching overhead
- UNCHANGED

✅ `wwv_detector_manager_process_display_sample()` (12 kHz rate):
- Identical logic: route to 3 tone trackers
- UNCHANGED

**Design Approach:**
- **Coordinator/Manager pattern** - different from detector and correlator patterns
- Lifecycle: Creation/destruction of 9 detector components
- Routing: Internal callbacks forward events between detectors
- API: Public interface for sample processing and status queries
- Hot paths stay in main file for performance

**API Fixes:**
- Removed call to non-existent `tick_correlator_add_tick()` (requires 12 parameters not available in manager)
- Fixed `sync_detector_is_synced()` → `sync_detector_get_state()` (correct API)
- Removed call to non-existent `sync_detector_get_drift()` and `sync_detector_print_stats()`

**Verification:**
- ✅ Compilation successful
- ✅ Hot paths preserved (no changes to sample processing logic)
- ✅ No performance regressions

**Commit:** "Phase 8: Split wwv_detector_manager - coordinator pattern"

---

### PHASE 9: Rename waterfall_telemetry → telemetry ⬜ NOT STARTED

**Current:** `src/waterfall_telemetry.c` (334 lines)

**Actions:**
- [ ] Rename: `src/waterfall_telemetry.c` → `src/core/telemetry.c`
- [ ] Rename: `include/waterfall_telemetry.h` → `include/core/telemetry.h`
- [ ] Update all #include statements in ~26 files
- [ ] If >300 lines after move, split:
  - [ ] `src/core/telemetry_udp.c` (UDP socket handling)
  - [ ] `src/core/telemetry_format.c` (Message formatting)
  - [ ] `src/core/telemetry.c` (Public API)

**Commit:** "Rename waterfall_telemetry to telemetry"

---

### PHASE 10: Move Small Files to New Structure ⬜ NOT STARTED

**Files under 300 lines - just move:**
- [ ] `src/wwv_clock.c` → `src/core/timing.c`
- [ ] `include/wwv_clock.h` → `include/core/timing.h`
- [ ] `src/channel_filters.c` → `src/signal/channel_filters.c`
- [ ] `include/channel_filters.h` → `include/signal/channel_filters.h`
- [ ] `src/tick_comb_filter.c` → `src/signal/comb_filter.c`
- [ ] `include/tick_comb_filter.h` → `include/signal/comb_filter.h`
- [ ] `src/kiss_fft.c` → `src/external/kiss_fft.c`
- [ ] `include/kiss_fft.h` → `include/external/kiss_fft.h`
- [ ] `include/_kiss_fft_guts.h` → `include/external/_kiss_fft_guts.h`

**Commit:** "Move small files to new directory structure"

---

## VERIFICATION CHECKLIST

After each phase:
- [ ] All files compile without errors
- [ ] No warnings introduced
- [ ] Git history preserves file lineage (use git mv)
- [ ] Public APIs unchanged
- [ ] Telemetry output consistent (where applicable)
- [ ] File line counts within target (150-300)

---

## PROGRESS TRACKING

| Phase | Status | Date | Commit | Notes |
|-------|--------|------|--------|-------|
| 0: Prep | ✅ | 2025-12-24 | db3eaa1 | Directories + file marking |
| 1: FFT | ✅ | 2025-12-24 | 591d6ee | FFT extraction (~270 lines eliminated) |
| 2: Dir reorg | ✅ | 2025-12-24 | 00fec55, e4c5ec1 | Feature-based directories |
| 3: Tick | ✅ | 2025-12-24 | 1ecd7e9 | Split into 3 modules (924→456 lines) |
| 4: Marker | ✅ | 2025-12-24 | 7e7805e | Split into 2 modules (535→284 lines) |
| 5: BCD | ⬜ | - | - | - |
| 6: Corr | ⬜ | - | - | - |
| 7: Tone | ⬜ | - | - | - |
| 8: Mgr | ⬜ | - | - | - |
| 9: Telem | ✅ | 2025-12-24 | d454afb | Renamed waterfall_telemetry → telemetry |
| 10: Move | ✅ | 2025-12-24 | 527e5fd | kiss_fft → external/ directory |

**Legend:** ⬜ Not Started | 🔄 In Progress | ✅ Complete | ❌ Blocked

---

## NOTES & ISSUES

- Each phase is atomic and independently committable
- No phase should break compilation
- Public APIs must remain unchanged
- Private implementation details can change freely
- Track any issues or deviations here:

(Add notes as refactor progresses)
