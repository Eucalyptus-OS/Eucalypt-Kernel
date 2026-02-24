#![no_std]

use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

const MAX_COLS: usize = 80;
const MAX_LINES: usize = 30;

static mut CONSOLE_LINES: [ConsoleLine; MAX_LINES] =
    [const { ConsoleLine::new(0x00000000) }; MAX_LINES];

struct SpinLock {
    locked: AtomicBool,
}

impl SpinLock {
    const fn new() -> Self {
        Self {
            locked: AtomicBool::new(false),
        }
    }

    #[inline(always)]
    fn lock(&self) {
        while self.locked.swap(true, Ordering::Acquire) {
            while self.locked.load(Ordering::Relaxed) {
                core::hint::spin_loop();
            }
        }
    }

    #[inline(always)]
    fn unlock(&self) {
        self.locked.store(false, Ordering::Release);
    }
}

pub struct RendererCell {
    inner: UnsafeCell<Option<ScrollingTextRenderer>>,
    lock: SpinLock,
}

unsafe impl Sync for RendererCell {}

impl RendererCell {
    pub const fn new() -> Self {
        Self {
            inner: UnsafeCell::new(None),
            lock: SpinLock::new(),
        }
    }

    #[inline]
    pub fn set(&self, renderer: ScrollingTextRenderer) {
        self.lock.lock();
        unsafe {
            *self.inner.get() = Some(renderer);
        }
        self.lock.unlock();
    }

    #[inline]
    pub fn with<F, R>(&self, f: F) -> R
    where
        F: FnOnce(&mut ScrollingTextRenderer) -> R,
    {
        self.lock.lock();
        let result = unsafe {
            f((*self.inner.get())
                .as_mut()
                .expect("Renderer not initialized"))
        };
        self.lock.unlock();
        result
    }
}

pub static RENDERER: RendererCell = RendererCell::new();

#[derive(Copy, Clone, Debug)]
#[repr(C)]
pub struct ConsoleChar {
    pub ch: u8,
    pub fg_color: u32,
    pub bg_color: u32,
}

impl ConsoleChar {
    #[inline(always)]
    pub const fn new(ch: u8, fg_color: u32, bg_color: u32) -> Self {
        Self {
            ch,
            fg_color,
            bg_color,
        }
    }

    #[inline(always)]
    pub const fn blank(bg_color: u32) -> Self {
        Self {
            ch: b' ',
            fg_color: 0xFFFFFFFF,
            bg_color,
        }
    }
}

pub struct ConsoleLine {
    chars: [ConsoleChar; MAX_COLS],
    width: usize,
    dirty: AtomicBool,
}

impl ConsoleLine {
    pub const fn new(bg_color: u32) -> Self {
        Self {
            chars: [ConsoleChar::blank(bg_color); MAX_COLS],
            width: MAX_COLS,
            dirty: AtomicBool::new(false),
        }
    }

    #[inline]
    pub fn set_width(&mut self, width: usize) {
        self.width = if width < MAX_COLS { width } else { MAX_COLS };
    }

    #[inline]
    pub fn clear(&mut self, bg_color: u32) {
        let blank = ConsoleChar::blank(bg_color);
        for i in 0..self.width {
            unsafe {
                *self.chars.get_unchecked_mut(i) = blank;
            }
        }
        self.dirty.store(true, Ordering::Release);
    }

    #[inline(always)]
    pub fn set_char(&mut self, col: usize, ch: ConsoleChar) {
        if col < self.width {
            unsafe {
                *self.chars.get_unchecked_mut(col) = ch;
            }
            self.dirty.store(true, Ordering::Release);
        }
    }

    #[inline(always)]
    pub fn get_char(&self, col: usize) -> Option<ConsoleChar> {
        if col < self.width {
            Some(unsafe { *self.chars.get_unchecked(col) })
        } else {
            None
        }
    }

    #[inline(always)]
    pub fn is_dirty(&self) -> bool {
        self.dirty.load(Ordering::Acquire)
    }

    #[inline(always)]
    pub fn mark_clean(&self) {
        self.dirty.store(false, Ordering::Release);
    }

    #[inline(always)]
    pub fn mark_dirty(&self) {
        self.dirty.store(true, Ordering::Release);
    }
}

#[repr(C, packed)]
struct PSF2Header {
    magic: [u8; 4],
    version: u32,
    headersize: u32,
    flags: u32,
    numglyph: u32,
    bytesperglyph: u32,
    height: u32,
    width: u32,
}

#[repr(C, packed)]
struct PSF1Header {
    magic: [u8; 2],
    mode: u8,
    charsize: u8,
}

pub struct ScrollingTextRenderer {
    lines: &'static mut [ConsoleLine; MAX_LINES],
    line_count: AtomicUsize,
    start_line: AtomicUsize,
    visible_lines: usize,
    cursor_col: usize,
    cursor_line: usize,
    cols: usize,
    fb_addr: *mut u32,
    pitch: usize,
    fb_width: usize,
    fb_height: usize,
    line_height: usize,
    char_width: usize,
    fg_color: u32,
    bg_color: u32,
    left_margin: usize,
    top_margin: usize,
    line_spacing: usize,
    font_data: &'static [u8],
    bytes_per_glyph: usize,
    header_size: usize,
}

impl ScrollingTextRenderer {
    pub fn init(
        fb_addr: *mut u8,
        fb_width: usize,
        fb_height: usize,
        pitch: usize,
        _bpp: usize,
        font: &'static [u8],
    ) {
        let (char_width, charsize, bytes_per_glyph, header_size) = Self::parse_psf(font);

        let line_height = charsize;
        let left_margin = 10;
        let top_margin = 10;
        let line_spacing = 2;
        let line_stride = line_height + line_spacing;
        let available_height = fb_height.saturating_sub(top_margin);
        let rows = if line_stride > 0 {
            available_height / line_stride
        } else {
            0
        };
        let available_width = fb_width.saturating_sub(left_margin);
        let cols = if char_width > 0 {
            available_width / char_width
        } else {
            80
        };
        let bg_color = 0x00000000;

        let lines: &'static mut [ConsoleLine; MAX_LINES] = unsafe {
            let cols_clamped = if cols < MAX_COLS { cols } else { MAX_COLS };
            let ptr = core::ptr::addr_of_mut!(CONSOLE_LINES);
            for i in 0..MAX_LINES {
                (*ptr)[i] = ConsoleLine::new(bg_color);
                (*ptr)[i].set_width(cols_clamped);
            }
            &mut *ptr
        };

        let initial_lines = if rows < MAX_LINES { rows } else { MAX_LINES };
        let visible = if rows < MAX_LINES { rows } else { MAX_LINES };

        let renderer = Self {
            lines,
            line_count: AtomicUsize::new(initial_lines),
            start_line: AtomicUsize::new(0),
            visible_lines: visible,
            cursor_col: 0,
            cursor_line: 0,
            cols: if cols < MAX_COLS { cols } else { MAX_COLS },
            fb_addr: fb_addr as *mut u32,
            pitch,
            fb_width,
            fb_height,
            line_height,
            char_width,
            fg_color: 0xFFFFFFFF,
            bg_color,
            left_margin,
            top_margin,
            line_spacing,
            font_data: font,
            bytes_per_glyph,
            header_size,
        };

        RENDERER.set(renderer);
    }

    #[inline]
    fn parse_psf(data: &[u8]) -> (usize, usize, usize, usize) {
        if data.len() >= 32 && &data[0..4] == b"\x72\xb5\x4a\x86" {
            let header = unsafe { &*(data.as_ptr() as *const PSF2Header) };
            return (
                header.width as usize,
                header.height as usize,
                header.bytesperglyph as usize,
                header.headersize as usize,
            );
        }

        if data.len() >= 4 && &data[0..2] == b"\x36\x04" {
            let header = unsafe { &*(data.as_ptr() as *const PSF1Header) };
            let height = header.charsize as usize;
            return (8, height, height, 4);
        }

        (8, 16, 16, 4)
    }

    #[inline(always)]
    fn physical_index(&self, logical_line: usize) -> usize {
        let start = self.start_line.load(Ordering::Relaxed);
        (start + logical_line) % MAX_LINES
    }

    #[inline(always)]
    fn get_line(&self, logical_line: usize) -> Option<&ConsoleLine> {
        let count = self.line_count.load(Ordering::Relaxed);
        if logical_line < count {
            Some(unsafe { self.lines.get_unchecked(self.physical_index(logical_line)) })
        } else {
            None
        }
    }

    #[inline(always)]
    fn get_line_mut(&mut self, logical_line: usize) -> Option<&mut ConsoleLine> {
        let count = self.line_count.load(Ordering::Relaxed);
        if logical_line < count {
            let idx = self.physical_index(logical_line);
            Some(unsafe { self.lines.get_unchecked_mut(idx) })
        } else {
            None
        }
    }

    #[inline]
    pub fn write_char(&mut self, ch: u8) {
        match ch {
            b'\n' => {
                self.cursor_col = 0;
                self.cursor_line += 1;
                let count = self.line_count.load(Ordering::Relaxed);
                if self.cursor_line >= count {
                    self.scroll_up();
                }
            }
            b'\r' => {
                self.cursor_col = 0;
            }
            b'\t' => {
                let spaces = 4 - (self.cursor_col & 3);
                for _ in 0..spaces {
                    self.write_char(b' ');
                }
            }
            _ => {
                let console_char = ConsoleChar::new(ch, self.fg_color, self.bg_color);
                let col = self.cursor_col;
                if let Some(line) = self.get_line_mut(self.cursor_line) {
                    line.set_char(col, console_char);
                }

                self.cursor_col += 1;

                if self.cursor_col >= self.cols {
                    self.cursor_col = 0;
                    self.cursor_line += 1;

                    let count = self.line_count.load(Ordering::Relaxed);
                    if self.cursor_line >= count {
                        self.scroll_up();
                    }
                }
            }
        }
    }

    pub fn write_text(&mut self, text: &[u8]) {
        for &byte in text {
            self.write_char(byte);
        }
        self.render_dirty();
    }

    pub fn scroll_up(&mut self) {
        let count = self.line_count.load(Ordering::Relaxed);
        if count < MAX_LINES {
            let new_count = count + 1;
            self.line_count.store(new_count, Ordering::Release);
            let new_line_idx = self.physical_index(new_count - 1);
            self.lines[new_line_idx].clear(self.bg_color);
            self.cursor_line = new_count - 1;

            for i in 0..new_count {
                if let Some(line) = self.get_line(i) {
                    line.mark_dirty();
                }
            }
        } else {
            let old_start = self.start_line.load(Ordering::Relaxed);
            self.lines[old_start].clear(self.bg_color);
            self.start_line
                .store((old_start + 1) % MAX_LINES, Ordering::Release);
            self.cursor_line = count - 1;

            for i in 0..count {
                if let Some(line) = self.get_line(i) {
                    line.mark_dirty();
                }
            }
        }
    }

    pub fn render_dirty(&mut self) {
        let count = self.line_count.load(Ordering::Relaxed);
        let visible_count = if self.visible_lines < count {
            self.visible_lines
        } else {
            count
        };
        let display_start = if count > visible_count {
            count - visible_count
        } else {
            0
        };

        for logical_line in display_start..count {
            let screen_row = logical_line - display_start;

            if let Some(line) = self.get_line(logical_line) {
                if line.is_dirty() {
                    let mut chars = [ConsoleChar::blank(0); MAX_COLS];
                    let width = line.width;
                    for i in 0..width {
                        if let Some(ch) = line.get_char(i) {
                            chars[i] = ch;
                        }
                    }
                    self.render_line(screen_row, &chars, width);
                    line.mark_clean();
                }
            }
        }
    }

    #[inline(always)]
    fn render_line(&self, screen_row: usize, chars: &[ConsoleChar; MAX_COLS], width: usize) {
        let y = self.top_margin + screen_row * (self.line_height + self.line_spacing);
        if y >= self.fb_height {
            return;
        }

        unsafe {
            let fb_base = self.fb_addr as usize;
            let pixels_per_row = self.pitch >> 2;
            let max_glyphs = (self.font_data.len() - self.header_size) / self.bytes_per_glyph;
            let bytes_per_line = (self.char_width + 7) >> 3;

            for py in 0..self.line_height {
                let row_y = y + py;
                if row_y >= self.fb_height {
                    break;
                }

                let row_ptr =
                    (fb_base + row_y * pixels_per_row * 4 + self.left_margin * 4) as *mut u32;

                for col in 0..self.cols {
                    let x_offset = col * self.char_width;

                    if col < width {
                        let console_char = *chars.get_unchecked(col);
                        let ch = console_char.ch as usize;
                        let glyph_idx = if ch < max_glyphs { ch } else { 0 };
                        let glyph_offset = self.header_size + glyph_idx * self.bytes_per_glyph;
                        let line_offset = py * bytes_per_line;

                        for gx in 0..self.char_width {
                            if x_offset + gx >= (self.fb_width - self.left_margin) {
                                break;
                            }

                            let byte_idx = glyph_offset + line_offset + (gx >> 3);
                            let bit_idx = 7 - (gx & 7);

                            let color = if byte_idx < self.font_data.len() {
                                let bit = (*self.font_data.get_unchecked(byte_idx) >> bit_idx) & 1;
                                if bit == 1 {
                                    console_char.fg_color
                                } else {
                                    console_char.bg_color
                                }
                            } else {
                                console_char.bg_color
                            };

                            *row_ptr.add(x_offset + gx) = color;
                        }
                    } else {
                        for gx in 0..self.char_width {
                            if x_offset + gx >= (self.fb_width - self.left_margin) {
                                break;
                            }
                            *row_ptr.add(x_offset + gx) = self.bg_color;
                        }
                    }
                }
            }
        }
    }

    #[inline]
    pub fn set_colors(&mut self, fg: u32, bg: u32) {
        self.fg_color = fg;
        self.bg_color = bg;
    }
}

#[inline]
pub fn write_global(text: &[u8]) {
    RENDERER.with(|r| r.write_text(text));
}

static mut LINE_WRITER_BUFFER: [u8; 512] = [0; 512];
static mut LINE_WRITER_POS: usize = 0;

pub struct LineWriter;

impl LineWriter {
    #[inline]
    pub fn new() -> Self {
        unsafe {
            *core::ptr::addr_of_mut!(LINE_WRITER_POS) = 0;
        }
        Self
    }

    #[inline]
    pub fn finish(&self) -> &[u8] {
        unsafe {
            let pos = *core::ptr::addr_of!(LINE_WRITER_POS);
            let buf_ptr = core::ptr::addr_of!(LINE_WRITER_BUFFER);
            core::slice::from_raw_parts((*buf_ptr).as_ptr(), pos)
        }
    }
}

impl core::fmt::Write for LineWriter {
    #[inline]
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        unsafe {
            let bytes = s.as_bytes();
            let buf_ptr = core::ptr::addr_of_mut!(LINE_WRITER_BUFFER);
            let pos_ptr = core::ptr::addr_of_mut!(LINE_WRITER_POS);
            let pos = *pos_ptr;
            let remaining = (*buf_ptr).len() - pos;
            let to_copy = if bytes.len() < remaining {
                bytes.len()
            } else {
                remaining
            };
            core::ptr::copy_nonoverlapping(
                bytes.as_ptr(),
                (*buf_ptr).as_mut_ptr().add(pos),
                to_copy,
            );
            *pos_ptr += to_copy;
            Ok(())
        }
    }
}

#[macro_export]
macro_rules! panic_print {
    ($($arg:tt)*) => {{
        let mut writer = $crate::LineWriter::new();
        use ::core::fmt::Write;
        let _ = ::core::write!(&mut writer, $($arg)*);
        $crate::write_global(&writer.finish());
    }};
}

#[macro_export]
macro_rules! kprintln {
    ($($arg:tt)*) => {{
        let mut writer = $crate::LineWriter::new();
        use ::core::fmt::Write;
        let _ = ::core::writeln!(&mut writer, $($arg)*);
        $crate::write_global(&writer.finish());
    }};
}

#[macro_export]
macro_rules! kprint {
    ($($arg:tt)*) => {{
        let mut writer = $crate::LineWriter::new();
        use ::core::fmt::Write;
        let _ = ::core::write!(&mut writer, $($arg)*);
        $crate::write_global(&writer.finish());
    }};
}

#[macro_export]
macro_rules! println {
    ($($arg:tt)*) => {
        $crate::kprintln!($($arg)*)
    };
}

#[macro_export]
macro_rules! print {
    ($($arg:tt)*) => {
        $crate::kprint!($($arg)*)
    };
}
