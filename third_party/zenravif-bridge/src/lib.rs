use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::slice;

use zenravif::{AlphaColorMode, BitDepth, ChromaSubsampling, Encoder, Img, RGBA8};

#[repr(C)]
pub struct ZenravifOutput {
    pub data: *mut u8,
    pub size: usize,
}

fn bit_depth_from_i32(value: i32) -> Result<BitDepth, &'static str> {
    match value {
        8 => Ok(BitDepth::Eight),
        10 => Ok(BitDepth::Ten),
        12 => Ok(BitDepth::Twelve),
        _ => Err("zenravif only supports 8, 10, or 12-bit output"),
    }
}

fn chroma_from_i32(value: i32) -> Result<ChromaSubsampling, &'static str> {
    match value {
        420 => Ok(ChromaSubsampling::Yuv420),
        444 => Ok(ChromaSubsampling::Yuv444),
        _ => Err("zenravif only supports 420 or 444 chroma"),
    }
}

unsafe fn write_error(error_out: *mut u8, error_capacity: usize, message: &str) {
    if error_out.is_null() || error_capacity == 0 {
        return;
    }
    let bytes = message.as_bytes();
    let len = bytes.len().min(error_capacity.saturating_sub(1));
    unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr(), error_out, len);
        *error_out.add(len) = 0;
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zenravif_bridge_encode_rgba8(
    pixels: *const u8,
    width: usize,
    height: usize,
    stride: usize,
    quality: i32,
    speed: i32,
    bit_depth: i32,
    chroma: i32,
    preserve_alpha: bool,
    threads: usize,
    keyint: i32,
    still_picture: bool,
    enable_qm: bool,
    vaq_strength: f64,
    enable_trellis: bool,
    rdo_tx_decision: bool,
    out: *mut ZenravifOutput,
    error_out: *mut u8,
    error_capacity: usize,
) -> i32 {
    let result = catch_unwind(AssertUnwindSafe(|| unsafe {
        if pixels.is_null() || out.is_null() {
            return Err("zenravif input/output pointer is null".to_string());
        }
        let row_bytes = width
            .checked_mul(4)
            .ok_or_else(|| "zenravif RGBA row byte count overflows".to_string())?;
        if width == 0 || height == 0 || stride < row_bytes {
            return Err("zenravif RGBA buffer shape is invalid".to_string());
        }
        let pixel_count = width
            .checked_mul(height)
            .ok_or_else(|| "zenravif RGBA pixel count overflows".to_string())?;
        let bit_depth = bit_depth_from_i32(bit_depth).map_err(str::to_string)?;
        let chroma = chroma_from_i32(chroma).map_err(str::to_string)?;
        let quality = quality.clamp(1, 100) as f32;
        let speed = speed.clamp(1, 10) as u8;
        if keyint != 1 {
            return Err("zenravif bridge currently supports still-image keyint=1 only".to_string());
        }
        if !still_picture {
            return Err("zenravif bridge currently supports still_picture=true only".to_string());
        }
        let mut rgba = Vec::<RGBA8>::new();
        rgba.try_reserve_exact(pixel_count)
            .map_err(|_| "zenravif RGBA buffer allocation failed".to_string())?;
        for y in 0..height {
            let offset = y
                .checked_mul(stride)
                .ok_or_else(|| "zenravif RGBA row offset overflows".to_string())?;
            offset
                .checked_add(row_bytes)
                .ok_or_else(|| "zenravif RGBA row range overflows".to_string())?;
            let row = slice::from_raw_parts(pixels.add(offset), row_bytes);
            for px in row.chunks_exact(4) {
                let alpha = if preserve_alpha { px[3] } else { 255 };
                rgba.push(RGBA8::new(px[0], px[1], px[2], alpha));
            }
        }
        let image = Img::new(rgba.as_slice(), width, height);
        let encoded = Encoder::new()
            .with_quality(quality)
            .with_alpha_quality(quality)
            .with_speed(speed)
            .with_bit_depth(bit_depth)
            .with_chroma_subsampling(chroma)
            .with_alpha_color_mode(AlphaColorMode::UnassociatedDirty)
            .with_num_threads(Some(threads.max(1)))
            .with_still_image_tuning(still_picture)
            .with_qm(enable_qm)
            .with_vaq(true, vaq_strength)
            .with_trellis(enable_trellis)
            .with_rdo_tx_decision(Some(rdo_tx_decision))
            .encode_rgba(image)
            .map_err(|err| err.to_string())?;
        let mut bytes = encoded.avif_file.into_boxed_slice();
        let data = bytes.as_mut_ptr();
        let size = bytes.len();
        std::mem::forget(bytes);
        (*out).data = data;
        (*out).size = size;
        Ok(())
    }));

    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => unsafe {
            write_error(error_out, error_capacity, &message);
            1
        },
        Err(_) => unsafe {
            write_error(error_out, error_capacity, "zenravif panicked during encode");
            1
        },
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zenravif_bridge_free(data: *mut u8, size: usize) {
    if data.is_null() {
        return;
    }
    unsafe {
        let _ = Box::from_raw(slice::from_raw_parts_mut(data, size));
    }
}
