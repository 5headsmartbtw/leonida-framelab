// SPDX-License-Identifier: MIT
//! Shared code of the native Rust D3D12 consumers (`sr` and `fg` binaries). They load the installed
//! FrameLab library at run time through `framelab-sys` and drive one component each.

pub mod gpu;
pub mod host;

pub type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

/// Uploaded to the output before FrameLab writes it, so an untouched texel is detectable.
pub const SENTINEL: u32 = 0xffff00ff;

pub fn require(condition: bool, message: impl Into<String>) -> Result<()> {
    if condition {
        Ok(())
    } else {
        Err(message.into().into())
    }
}

/// IEEE 754 binary16 bits of `value`, rounded to nearest even. Normal and zero values only, which
/// covers the motion vectors used here.
pub fn half(value: f32) -> u16 {
    let bits = value.to_bits();
    let sign = ((bits >> 16) & 0x8000) as u16;
    let exponent = ((bits >> 23) & 0xff) as i32 - 127 + 15;
    let mantissa = bits & 0x7f_ffff;
    if value == 0.0 || exponent <= 0 {
        return sign;
    }
    assert!(exponent < 31, "value out of binary16 range");
    let mut result = (exponent as u32) << 10 | mantissa >> 13;
    let rest = mantissa & 0x1fff;
    if rest > 0x1000 || (rest == 0x1000 && result & 1 == 1) {
        result += 1;
    }
    sign | result as u16
}

#[cfg(test)]
mod tests {
    #[test]
    fn half_matches_known_values() {
        assert_eq!(super::half(0.0), 0);
        assert_eq!(super::half(1.0), 0x3c00);
        assert_eq!(super::half(-2.0), 0xc000);
        assert_eq!(super::half(-0.025), 0xa666);
        assert_eq!(super::half(0.1), 0x2e66);
    }
}
