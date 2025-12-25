#!/usr/bin/env python3
"""
Generate reference test vectors for phoenix-dsp advanced DSP tests.
Validates against MATLAB/scipy/numpy reference implementations.
"""

import numpy as np
import scipy.signal
import json

def generate_butterworth_coefficients():
    """Generate reference Butterworth filter coefficients."""
    fs = 50000.0  # Sample rate
    
    tests = {
        'butter_2nd_lp_1400hz': {
            'type': 'lowpass',
            'order': 2,
            'cutoff': 1400.0,
            'fs': fs,
            'sos': scipy.signal.butter(2, 1400.0, 'lp', fs=fs, output='sos').tolist()
        },
        'butter_4th_lp_150hz': {
            'type': 'lowpass',
            'order': 4,
            'cutoff': 150.0,
            'fs': fs,
            'sos': scipy.signal.butter(4, 150.0, 'lp', fs=fs, output='sos').tolist()
        },
        'butter_4th_hp_800hz': {
            'type': 'highpass',
            'order': 4,
            'cutoff': 800.0,
            'fs': fs,
            'sos': scipy.signal.butter(4, 800.0, 'hp', fs=fs, output='sos').tolist()
        }
    }
    
    return tests

def generate_window_coefficients():
    """Generate reference window function coefficients."""
    sizes = [256, 512, 1024, 2048]
    
    windows = {}
    for N in sizes:
        windows[f'hann_{N}'] = np.hanning(N).tolist()
        windows[f'hamming_{N}'] = np.hamming(N).tolist()
        windows[f'blackman_harris_{N}'] = scipy.signal.windows.blackmanharris(N).tolist()
    
    return windows

def generate_goertzel_test_vectors():
    """Generate test signals for Goertzel detector."""
    fs = 2400.0
    block_size = 24
    target_freq = 100.0
    
    # Generate 100 Hz tone
    t = np.arange(block_size) / fs
    tone = np.sin(2 * np.pi * target_freq * t)
    
    # Manual Goertzel calculation (scipy.signal.goertzel not available in all versions)
    k = block_size * target_freq / fs
    coeff = 2.0 * np.cos(2 * np.pi * k / block_size)
    
    s1, s2 = 0.0, 0.0
    for sample in tone:
        s0 = sample + coeff * s1 - s2
        s2 = s1
        s1 = s0
    
    magnitude = np.sqrt(s1*s1 + s2*s2 - coeff*s1*s2)
    
    return {
        'fs': fs,
        'block_size': block_size,
        'target_freq': target_freq,
        'input_signal': tone.tolist(),
        'expected_magnitude': float(magnitude)
    }

def generate_parabolic_interpolation_tests():
    """Generate synthetic peak tests for parabolic interpolation."""
    # Create synthetic FFT-like peak
    bins = np.zeros(128)
    peak_bin = 64
    peak_offset = 0.3  # True peak at bin 64.3
    
    # Parabolic peak shape
    for i in range(peak_bin - 2, peak_bin + 3):
        x = i - (peak_bin + peak_offset)
        bins[i] = 10.0 - x*x  # Parabola with peak at offset
    
    alpha = bins[peak_bin - 1]
    beta = bins[peak_bin]
    gamma = bins[peak_bin + 1]
    
    # Expected interpolation result
    expected_offset = 0.5 * (alpha - gamma) / (alpha - 2*beta + gamma)
    
    return {
        'peak_bin': peak_bin,
        'alpha': float(alpha),
        'beta': float(beta),
        'gamma': float(gamma),
        'expected_offset': float(expected_offset),
        'expected_true_bin': peak_bin + expected_offset
    }

def main():
    """Generate all test vectors and save to header file."""
    
    print("Generating reference test vectors...")
    
    # Generate all test data
    butter_coeffs = generate_butterworth_coefficients()
    windows = generate_window_coefficients()
    goertzel = generate_goertzel_test_vectors()
    parabolic = generate_parabolic_interpolation_tests()
    
    # Save as JSON for now (convert to C header later)
    test_data = {
        'butterworth': butter_coeffs,
        'windows': windows,
        'goertzel': goertzel,
        'parabolic_interpolation': parabolic
    }
    
    with open('test_vectors.json', 'w') as f:
        json.dump(test_data, f, indent=2)
    
    print("✓ Generated test_vectors.json")
    print(f"  - {len(butter_coeffs)} Butterworth filter tests")
    print(f"  - {len(windows)} window function tests")
    print(f"  - 1 Goertzel detector test")
    print(f"  - 1 parabolic interpolation test")
    
    # Print sample for verification
    print("\nSample Butterworth 2nd order LP @ 1400 Hz:")
    sos = butter_coeffs['butter_2nd_lp_1400hz']['sos']
    print(f"  SOS: {sos[0]}")

if __name__ == '__main__':
    main()
