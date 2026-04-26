#![no_std]

use core::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use core::cell::UnsafeCell;

const MAX_COLS: usize = 80;
const MAX_LINES: usize = 70;

pub mod color {
    pub const BLACK:      u32 = 0x00000000;
    pub const WHITE:      u32 = 0xFFFFFFFF;
    pub const RED:        u32 = 0xFFFF5555;
    pub const GREEN:      u32 = 0xFF55FF55;
    pub const BLUE:       u32 = 0xFF5555FF;
    pub const YELLOW:     u32 = 0xFFFFFF55;
    pub const CYAN:       u32 = 0xFF55FFFF;
    pub const MAGENTA:    u32 = 0xFFFF55FF;
    pub const ORANGE:     u32 = 0xFFFF8800;
    pub const PINK:       u32 = 0xFFFF88CC;
    pub const GRAY:       u32 = 0xFF888888;
    pub const DARK_GRAY:  u32 = 0xFF444444;
    pub const DARK_RED:   u32 = 0xFFAA0000;
    pub const DARK_GREEN: u32 = 0xFF00AA00;
    pub const DARK_BLUE:  u32 = 0xFF000082;
    pub const LIME:       u32 = 0xFF00FF00;
    pub const TEAL:       u32 = 0xFF008888;
    pub const PURPLE:     u32 = 0xFF8800FF;
    pub const BROWN:      u32 = 0xFF885500;
    pub const SKY_BLUE:   u32 = 0xFF87CEEB;
}

pub trait Colorize {
    fn colored(&self, fg: u32, bg: u32) -> ColoredStr<'_>;
    fn red(&self)       -> ColoredStr<'_> { self.colored(color::RED,     color::BLACK) }
    fn green(&self)     -> ColoredStr<'_> { self.colored(color::GREEN,   color::BLACK) }
    fn blue(&self)      -> ColoredStr<'_> { self.colored(color::BLUE,    color::BLACK) }
    fn yellow(&self)    -> ColoredStr<'_> { self.colored(color::YELLOW,  color::BLACK) }
    fn cyan(&self)      -> ColoredStr<'_> { self.colored(color::CYAN,    color::BLACK) }
    fn magenta(&self)   -> ColoredStr<'_> { self.colored(color::MAGENTA, color::BLACK) }
    fn orange(&self)    -> ColoredStr<'_> { self.colored(color::ORANGE,  color::BLACK) }
    fn pink(&self)      -> ColoredStr<'_> { self.colored(color::PINK,    color::BLACK) }
    fn white(&self)     -> ColoredStr<'_> { self.colored(color::WHITE,   color::BLACK) }
    fn gray(&self)      -> ColoredStr<'_> { self.colored(color::GRAY,    color::BLACK) }
    fn on_red(&self)    -> ColoredStr<'_> { self.colored(color::WHITE,   color::RED)   }
    fn on_green(&self)  -> ColoredStr<'_> { self.colored(color::BLACK,   color::GREEN) }
    fn on_blue(&self)   -> ColoredStr<'_> { self.colored(color::WHITE,   color::BLUE)  }
    fn on_yellow(&self) -> ColoredStr<'_> { self.colored(color::BLACK,   color::YELLOW)}
    fn on_black(&self)  -> ColoredStr<'_> { self.colored(color::WHITE,   color::BLACK) }
}

impl Colorize for str {
    fn colored(&self, fg: u32, bg: u32) -> ColoredStr<'_> {
        ColoredStr { text: self, fg, bg }
    }
}

pub struct ColoredStr<'a> {
    pub text: &'a str,
    pub fg:   u32,
    pub bg:   u32,
}

impl<'a> ColoredStr<'a> {
    pub fn print(&self) {
        RENDERER.with(|r| {
            let prev_fg = r.fg_color;
            let prev_bg = r.bg_color;
            r.fg_color = self.fg;
            r.bg_color = self.bg;
            r.write_text(self.text.as_bytes());
            r.fg_color = prev_fg;
            r.bg_color = prev_bg;
        });
    }
}

static mut CONSOLE_LINES: [ConsoleLine; MAX_LINES] = [const { ConsoleLine::new(0x00000000) }; MAX_LINES];

struct InterruptSpinLock {
    locked: AtomicBool,
}

impl InterruptSpinLock {
    const fn new() -> Self {
        Self { locked: AtomicBool::new(false) }
    }

    #[inline(always)]
    fn lock(&self) -> bool {
        let rflags: u64;
        unsafe { core::arch::asm!("pushfq; pop {}", out(reg) rflags, options(nomem)); }
        let interrupts_were_enabled = (rflags & 0x200) != 0;
        unsafe { core::arch::asm!("cli", options(nomem, nostack)); }
        while self.locked.swap(true, Ordering::Acquire) {
            while self.locked.load(Ordering::Relaxed) {
                core::hint::spin_loop();
            }
        }
        interrupts_were_enabled
    }

    #[inline(always)]
    fn unlock(&self, restore_interrupts: bool) {
        self.locked.store(false, Ordering::Release);
        if restore_interrupts {
            unsafe { core::arch::asm!("sti", options(nomem, nostack)); }
        }
    }
}

pub struct RendererCell {
    inner: UnsafeCell<Option<ScrollingTextRenderer>>,
    lock:  InterruptSpinLock,
}

unsafe impl Sync for RendererCell {}

impl RendererCell {
    pub const fn new() -> Self {
        Self { inner: UnsafeCell::new(None), lock: InterruptSpinLock::new() }
    }

    #[inline]
    pub fn set(&self, renderer: ScrollingTextRenderer) {
        let irq = self.lock.lock();
        unsafe { *self.inner.get() = Some(renderer); }
        self.lock.unlock(irq);
    }

    #[inline]
    pub fn with<F, R>(&self, f: F) -> R
    where F: FnOnce(&mut ScrollingTextRenderer) -> R {
        let irq = self.lock.lock();
        let result = unsafe {
            f((*self.inner.get()).as_mut().expect("Renderer not initialized"))
        };
        self.lock.unlock(irq);
        result
    }
}

pub static RENDERER: RendererCell = RendererCell::new();

#[derive(Copy, Clone, Debug)]
#[repr(C)]
pub struct ConsoleChar {
    pub ch:       u8,
    pub fg_color: u32,
    pub bg_color: u32,
}

impl ConsoleChar {
    #[inline(always)]
    pub const fn new(ch: u8, fg_color: u32, bg_color: u32) -> Self {
        Self { ch, fg_color, bg_color }
    }

    #[inline(always)]
    pub const fn blank(bg_color: u32) -> Self {
        Self { ch: b' ', fg_color: 0xFFFFFFFF, bg_color }
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
            unsafe { *self.chars.get_unchecked_mut(i) = blank; }
        }
        self.dirty.store(true, Ordering::Release);
    }

    #[inline(always)]
    pub fn set_char(&mut self, col: usize, ch: ConsoleChar) {
        if col < self.width {
            unsafe { *self.chars.get_unchecked_mut(col) = ch; }
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

    #[inline(always)] pub fn is_dirty(&self)   -> bool { self.dirty.load(Ordering::Acquire) }
    #[inline(always)] pub fn mark_clean(&self)         { self.dirty.store(false, Ordering::Release); }
    #[inline(always)] pub fn mark_dirty(&self)         { self.dirty.store(true,  Ordering::Release); }
}

#[repr(C, packed)]
struct PSF2Header {
    magic:         [u8; 4],
    version:       u32,
    headersize:    u32,
    flags:         u32,
    numglyph:      u32,
    bytesperglyph: u32,
    height:        u32,
    width:         u32,
}

#[repr(C, packed)]
struct PSF1Header {
    magic:    [u8; 2],
    mode:     u8,
    charsize: u8,
}

const BACK_BUFFER_PIXELS: usize = 1920 * 1080;
static mut BACK_BUFFER: [u32; BACK_BUFFER_PIXELS] = [0u32; BACK_BUFFER_PIXELS];

pub struct ScrollingTextRenderer {
    lines:           &'static mut [ConsoleLine; MAX_LINES],
    line_count:      AtomicUsize,
    start_line:      AtomicUsize,
    visible_lines:   usize,
    cursor_col:      usize,
    cursor_line:     usize,
    cols:            usize,
    pub fb_addr:         *mut u32,
    pub back_buffer:     *mut u32,
    pub pitch:           usize,
    fb_width:        usize,
    fb_height:       usize,
    line_height:     usize,
    char_width:      usize,
    pub fg_color:    u32,
    pub bg_color:    u32,
    left_margin:     usize,
    top_margin:      usize,
    line_spacing:    usize,
    font_data:       &'static [u8],
    bytes_per_glyph: usize,
    header_size:     usize,
}

impl ScrollingTextRenderer {
    pub fn init(
        fb_addr:   *mut u8,
        fb_width:  usize,
        fb_height: usize,
        pitch:     usize,
        _bpp:      usize,
        font:      &'static [u8],
    ) {
        let (char_width, charsize, bytes_per_glyph, header_size) = Self::parse_psf(font);
        let line_height      = charsize;
        let left_margin      = 10;
        let top_margin       = 10;
        let line_spacing     = 2;
        let line_stride      = line_height + line_spacing;
        let available_height = fb_height.saturating_sub(top_margin);
        let rows = if line_stride > 0 { available_height / line_stride } else { 0 };
        let available_width  = fb_width.saturating_sub(left_margin);
        let cols = if char_width > 0 { available_width / char_width } else { 80 };
        let bg_color         = color::BLACK;

        let lines: &'static mut [ConsoleLine; MAX_LINES] = unsafe {
            let cols_clamped = if cols < MAX_COLS { cols } else { MAX_COLS };
            let ptr = core::ptr::addr_of_mut!(CONSOLE_LINES);
            for i in 0..MAX_LINES {
                (*ptr)[i] = ConsoleLine::new(bg_color);
                (*ptr)[i].set_width(cols_clamped);
            }
            &mut *ptr
        };

        let visible     = if rows < MAX_LINES { rows } else { MAX_LINES };
        let back_buffer = core::ptr::addr_of_mut!(BACK_BUFFER) as *mut u32;

        let renderer = Self {
            lines,
            line_count:      AtomicUsize::new(if rows < MAX_LINES { rows } else { MAX_LINES }),
            start_line:      AtomicUsize::new(0),
            visible_lines:   visible,
            cursor_col:      0,
            cursor_line:     0,
            cols:            if cols < MAX_COLS { cols } else { MAX_COLS },
            fb_addr:         fb_addr as *mut u32,
            back_buffer,
            pitch,
            fb_width,
            fb_height,
            line_height,
            char_width,
            fg_color:        color::WHITE,
            bg_color,
            left_margin,
            top_margin,
            line_spacing,
            font_data:       font,
            bytes_per_glyph,
            header_size,
        };

        RENDERER.set(renderer);
    }

    #[inline]
    fn parse_psf(data: &[u8]) -> (usize, usize, usize, usize) {
        if data.len() >= 32 && &data[0..4] == b"\x72\xb5\x4a\x86" {
            let h = unsafe { &*(data.as_ptr() as *const PSF2Header) };
            return (h.width as usize, h.height as usize, h.bytesperglyph as usize, h.headersize as usize);
        }
        if data.len() >= 4 && &data[0..2] == b"\x36\x04" {
            let h = unsafe { &*(data.as_ptr() as *const PSF1Header) };
            return (8, h.charsize as usize, h.charsize as usize, 4);
        }
        (8, 16, 16, 4)
    }

    pub fn fill_screen(&mut self, fill_color: u32) {
        let pixels_per_row = self.pitch >> 2;
        let total          = self.fb_height * pixels_per_row;

        unsafe {
            let bb = self.back_buffer;
            let mut i = 0usize;
            while i + 8 <= total {
                *bb.add(i)     = fill_color;
                *bb.add(i + 1) = fill_color;
                *bb.add(i + 2) = fill_color;
                *bb.add(i + 3) = fill_color;
                *bb.add(i + 4) = fill_color;
                *bb.add(i + 5) = fill_color;
                *bb.add(i + 6) = fill_color;
                *bb.add(i + 7) = fill_color;
                i += 8;
            }
            while i < total {
                *bb.add(i) = fill_color;
                i += 1;
            }
            core::ptr::copy_nonoverlapping(self.back_buffer, self.fb_addr, total);
        }

        self.bg_color    = fill_color;
        self.cursor_col  = 0;
        self.cursor_line = 0;
        for i in 0..MAX_LINES {
            self.lines[i].clear(fill_color);
        }
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
                self.cursor_col   = 0;
                self.cursor_line += 1;
                if self.cursor_line >= self.line_count.load(Ordering::Relaxed) {
                    self.scroll_up();
                }
            }
            b'\r' => { self.cursor_col = 0; }
            b'\t' => {
                let spaces = 4 - (self.cursor_col & 3);
                for _ in 0..spaces { self.write_char(b' '); }
            }
            0x08 => {
                if self.cursor_col > 0 {
                    self.cursor_col -= 1;
                } else if self.cursor_line > 0 {
                    self.cursor_line -= 1;
                    self.cursor_col = self.cols - 1;
                } else {
                    return;
                }
                let col  = self.cursor_col;
                let line = self.cursor_line;
                let blank = ConsoleChar::new(b' ', self.fg_color, self.bg_color);
                if let Some(l) = self.get_line_mut(line) {
                    l.set_char(col, blank);
                }
            }
            _ => {
                let console_char = ConsoleChar::new(ch, self.fg_color, self.bg_color);
                let col          = self.cursor_col;
                if let Some(line) = self.get_line_mut(self.cursor_line) {
                    line.set_char(col, console_char);
                }
                self.cursor_col += 1;
                if self.cursor_col >= self.cols {
                    self.cursor_col   = 0;
                    self.cursor_line += 1;
                    if self.cursor_line >= self.line_count.load(Ordering::Relaxed) {
                        self.scroll_up();
                    }
                }
            }
        }
    }

    pub fn write_text(&mut self, text: &[u8]) {
        for &byte in text { self.write_char(byte); }
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
                if let Some(line) = self.get_line(i) { line.mark_dirty(); }
            }
        } else {
            let old_start = self.start_line.load(Ordering::Relaxed);
            self.lines[old_start].clear(self.bg_color);
            self.start_line.store((old_start + 1) % MAX_LINES, Ordering::Release);
            self.cursor_line = count - 1;
            for i in 0..count {
                if let Some(line) = self.get_line(i) { line.mark_dirty(); }
            }
        }
    }

    pub fn render_dirty(&mut self) {
        let count          = self.line_count.load(Ordering::Relaxed);
        let visible_count  = if self.visible_lines < count { self.visible_lines } else { count };
        let display_start  = if count > visible_count { count - visible_count } else { 0 };
        let pixels_per_row = self.pitch >> 2;

        for logical_line in display_start..count {
            let screen_row = logical_line - display_start;
            let idx        = self.physical_index(logical_line);
            let line       = unsafe { self.lines.get_unchecked(idx) };

            if !line.is_dirty() { continue; }

            let width = line.width;
            let mut chars = [ConsoleChar::blank(0); MAX_COLS];
            for i in 0..width {
                if let Some(ch) = line.get_char(i) { chars[i] = ch; }
            }

            self.render_line_to_backbuffer(screen_row, &chars, width);
            self.flush_line(screen_row, pixels_per_row);
            unsafe { self.lines.get_unchecked(idx).mark_clean(); }
        }
    }

    #[inline(always)]
    fn render_line_to_backbuffer(&mut self, screen_row: usize, chars: &[ConsoleChar; MAX_COLS], width: usize) {
        let y = self.top_margin + screen_row * (self.line_height + self.line_spacing);
        if y >= self.fb_height { return; }

        unsafe {
            let bb_base        = self.back_buffer as usize;
            let pixels_per_row = self.pitch >> 2;
            let max_glyphs     = (self.font_data.len() - self.header_size) / self.bytes_per_glyph;
            let bytes_per_line = (self.char_width + 7) >> 3;

            for py in 0..self.line_height {
                let row_y = y + py;
                if row_y >= self.fb_height { break; }

                let row_ptr = (bb_base + row_y * pixels_per_row * 4 + self.left_margin * 4) as *mut u32;

                for col in 0..self.cols {
                    let x_offset = col * self.char_width;

                    if col < width {
                        let cc          = *chars.get_unchecked(col);
                        let glyph_idx   = if (cc.ch as usize) < max_glyphs { cc.ch as usize } else { 0 };
                        let glyph_off   = self.header_size + glyph_idx * self.bytes_per_glyph;
                        let line_off    = py * bytes_per_line;

                        for gx in 0..self.char_width {
                            if x_offset + gx >= self.fb_width - self.left_margin { break; }
                            let byte_idx = glyph_off + line_off + (gx >> 3);
                            let bit_idx  = 7 - (gx & 7);
                            let color = if byte_idx < self.font_data.len() {
                                if (*self.font_data.get_unchecked(byte_idx) >> bit_idx) & 1 == 1 {
                                    cc.fg_color
                                } else {
                                    cc.bg_color
                                }
                            } else {
                                cc.bg_color
                            };
                            *row_ptr.add(x_offset + gx) = color;
                        }
                    } else {
                        for gx in 0..self.char_width {
                            if x_offset + gx >= self.fb_width - self.left_margin { break; }
                            *row_ptr.add(x_offset + gx) = self.bg_color;
                        }
                    }
                }
            }
        }
    }

    #[inline(always)]
    fn flush_line(&self, screen_row: usize, pixels_per_row: usize) {
        let y = self.top_margin + screen_row * (self.line_height + self.line_spacing);
        if y >= self.fb_height { return; }

        unsafe {
            for py in 0..self.line_height {
                let row_y = y + py;
                if row_y >= self.fb_height { break; }
                let src = self.back_buffer.add(row_y * pixels_per_row);
                let dst = self.fb_addr.add(row_y * pixels_per_row);
                core::ptr::copy_nonoverlapping(src, dst, pixels_per_row);
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

pub fn fill_screen(fill_color: u32) {
    RENDERER.with(|r| r.fill_screen(fill_color));
}

pub struct LineWriter {
    buf: [u8; 512],
    pos: usize,
}

impl LineWriter {
    #[inline]
    pub const fn new() -> Self {
        Self { buf: [0u8; 512], pos: 0 }
    }

    #[inline]
    pub fn finish(&self) -> &[u8] {
        &self.buf[..self.pos]
    }
}

impl core::fmt::Write for LineWriter {
    #[inline]
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        let bytes     = s.as_bytes();
        let remaining = self.buf.len() - self.pos;
        let to_copy   = if bytes.len() < remaining { bytes.len() } else { remaining };
        self.buf[self.pos..self.pos + to_copy].copy_from_slice(&bytes[..to_copy]);
        self.pos += to_copy;
        Ok(())
    }
}

#[macro_export]
macro_rules! kprintln {
    ($($arg:tt)*) => {{
        let mut writer = $crate::LineWriter::new();
        use ::core::fmt::Write;
        let _ = ::core::writeln!(&mut writer, $($arg)*);
        $crate::write_global(writer.finish());
    }};
}

#[macro_export]
macro_rules! kprint {
    ($($arg:tt)*) => {{
        let mut writer = $crate::LineWriter::new();
        use ::core::fmt::Write;
        let _ = ::core::write!(&mut writer, $($arg)*);
        $crate::write_global(writer.finish());
    }};
}

#[macro_export]
macro_rules! println {
    ($($arg:tt)*) => { $crate::kprintln!($($arg)*) };
}

#[macro_export]
macro_rules! print {
    ($($arg:tt)*) => { $crate::kprint!($($arg)*) };
}

#[macro_export]
macro_rules! panic_print {
    ($($arg:tt)*) => { $crate::kprint!($($arg)*) };
}

#[macro_export]
macro_rules! cprintln {
    ($text:expr, $fg:expr) => {{
        use $crate::Colorize;
        $text.colored($fg, $crate::color::BLACK).print();
        $crate::write_global(b"\n");
    }};
    ($text:expr, $fg:expr, $bg:expr) => {{
        use $crate::Colorize;
        $text.colored($fg, $bg).print();
        $crate::write_global(b"\n");
    }};
}