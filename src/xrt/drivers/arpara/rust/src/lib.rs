// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
//! C ABI for Monado's HID abstraction. Codec and fixed requests come from the
//! pinned vrg9080-protocol-rs source, not a second implementation of the protocol.
use codec::{Event, parse_report};

// The upstream fixed-request module is private. Compile that exact module at
// the pinned submodule revision, without making an arbitrary-write API upstream.
#[allow(dead_code)]
#[path = "../../../../../external/vrg9080-protocol-rs/src/codec.rs"]
mod codec;
#[allow(dead_code)]
#[path = "../../../../../external/vrg9080-protocol-rs/src/transport/protocol.rs"]
mod fixed;

#[repr(C)]
pub struct Decoded {
    pub kind: u32, // 1 IMU, 2 status, 3 command response, 4 unsupported
    pub values: [f32; 9],
    pub counter: u16,
    pub command: u8,
    pub status: u8,
    pub ack_requested: u8,
    pub ack: [u8; 65],
}

impl Decoded {
    fn new() -> Self {
        Self {
            kind: 0,
            values: [0.; 9],
            counter: 0,
            command: 0,
            status: 0,
            ack_requested: 0,
            ack: [0; 65],
        }
    }
}

/// Decode a report with the upstream codec.
///
/// # Safety
/// Input must point to `len` readable bytes and output to one writable Decoded.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn arpara_rs_decode(input: *const u8, len: usize, out: *mut Decoded) -> bool {
    if input.is_null() || out.is_null() || len != 64 {
        return false;
    }
    // SAFETY: caller supplies a readable 64-byte HID buffer for this call.
    let bytes = unsafe { std::slice::from_raw_parts(input, len) };
    let Ok(event) = parse_report(bytes) else {
        return false;
    };
    let mut result = Decoded::new();
    match event {
        Event::Imu(frame) => {
            result.kind = 1;
            result.values[..3].copy_from_slice(&frame.acceleration_g);
            result.values[3..6].copy_from_slice(&frame.angular_velocity_deg_s);
            result.values[6..].copy_from_slice(&frame.magnetic_field_raw);
            result.counter = frame.timestamp_ticks;
        }
        Event::Status(status) => {
            result.kind = 2;
            if status.acknowledgement_requested {
                result.ack_requested = 1;
                result.ack = fixed::acknowledgement(&status);
            }
        }
        Event::StartupResponse(response) | Event::UnsupportedResponse(response) => {
            result.kind = 3;
            result.command = response.command_id;
            result.status = response.status;
        }
        Event::CurrentProximity(response) => {
            result.kind = 3;
            result.command = 0x4a;
            result.status = response.status;
        }
        Event::UnsupportedReport { .. } => result.kind = 4,
    }
    // SAFETY: caller supplies one writable Decoded, with the shared C layout.
    unsafe { out.write(result) };
    true
}

/// Return the fixed startup step count and optionally copy one step.
///
/// # Safety
/// Non-null out must hold 65 writable bytes and timeout one writable u32.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn arpara_rs_startup(index: usize, out: *mut u8, timeout: *mut u32) -> usize {
    if let Some(step) = fixed::STARTUP.get(index) {
        if !out.is_null() && !timeout.is_null() {
            // SAFETY: caller provides 65 writable bytes and one writable u32.
            unsafe {
                std::ptr::copy_nonoverlapping(step.report.as_ptr(), out, 65);
                timeout.write(step.timeout.as_millis() as u32);
            }
        }
    }
    fixed::STARTUP.len()
}

#[unsafe(no_mangle)]
pub extern "C" fn arpara_rs_counter_delta_ns(now: u16, prev: u16) -> u64 {
    let ticks = now.wrapping_sub(prev);
    if (1..=150).contains(&ticks) {
        u64::from(ticks) * 100_000
    } else {
        0
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn c_abi_layout() {
        assert_eq!(std::mem::size_of::<Decoded>(), 112);
        assert_eq!(std::mem::align_of::<Decoded>(), 4);
        assert_eq!(std::mem::offset_of!(Decoded, counter), 40);
        assert_eq!(std::mem::offset_of!(Decoded, ack), 45);
    }
    #[test]
    fn fixed_sampling_vector_and_wrap() {
        assert_eq!(
            &fixed::STARTUP[14].report[..10],
            &[0, 0xaa, 1, 0x81, 8, 0xe8, 3, 0xc8, 0, 0xe7]
        );
        assert_eq!(arpara_rs_counter_delta_ns(4, 65530), 1_000_000);
        assert_eq!(arpara_rs_counter_delta_ns(10, 10), 0);
        assert_eq!(arpara_rs_counter_delta_ns(500, 10), 0);
    }
    #[test]
    fn ffi_rejects_null_and_short_input() {
        let mut out = Decoded::new();
        let bytes = [0; 64];
        // SAFETY: all non-null test pointers refer to live arrays/objects.
        unsafe {
            assert!(!arpara_rs_decode(std::ptr::null(), 64, &mut out));
            assert!(!arpara_rs_decode(bytes.as_ptr(), 3, &mut out));
            assert!(!arpara_rs_decode(bytes.as_ptr(), 64, std::ptr::null_mut()));
        }
    }
}
