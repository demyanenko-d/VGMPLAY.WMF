; One-shot large OPL4 loader, overlaid into unused space of plugin-relative
; page 5 (the command-block page, not WC physical page #05).  Logical address
; #9800 means offset #1800, above WC's #1000..#1447 screen scratch range.
        DEVICE  NOSLOT64K
        ORG     #9800

PORT_W0         EQU #10AF
PORT_W2         EQU #12AF
PORT_W3         EQU #13AF
OPL_STAT        EQU #C4
OPL_FM1_REG     EQU #C6
OPL_FM1_DAT     EQU #C7
OPL_WAVE_REG    EQU #7E
OPL_WAVE_DAT    EQU #7F
PORT_SYSCONF    EQU #20AF   ; System Config (turbo bits [1:0]), see isr.s
TURBO_14MHZ     EQU #02

; Return: A bit 0..6 = compacted 16K page count (0 = reject),
;         A bit 7 = ROM-image flag,
;         H = first VPL page for the unread tail,
;         L = high byte of its destination address (#C0..#FF).
opl4_large_entry:
        ; Page 5 replaces Win2, which normally contains the resident plugin
        ; ISR.  The resident wrapper keeps interrupts disabled until #6028
        ; has restored page 0; never execute EI on this page.
        push    ix
        push    iy

        ; Loading a large file spends most of its time in the busy-waited
        ; SRAM upload below (upload_block/wave_reg/wave_dat) -- run it at
        ; full speed instead of whatever rate WC's own file I/O left active.
        ld      bc,PORT_SYSCONF
        ld      a,TURBO_14MHZ
        out     (c),a

        xor     a
        ld      (rom_flag),a
        ld      (block_count),a
        call    map_page0

        ; Basic VGM + OPL4 header validation.
        ld      hl,#C000
        ld      de,sig_vgm
        ld      b,4
.sig:   ld      a,(de)
        cp      (hl)
        jp      nz,loader_fail
        inc     de
        inc     hl
        djnz    .sig
        ld      hl,(#C060)
        ld      de,(#C062)
        ld      a,h
        or      l
        or      d
        or      e
        jp      z,loader_fail

        ld      hl,#0040
        ld      de,(#C008)
        ld      a,d
        cp      #01
        jr      c,.data_ready
        jr      nz,.data_new
        ld      a,e
        cp      #50
        jr      c,.data_ready
.data_new:
        ld      a,(#C036)
        or      a
        jp      nz,loader_fail
        ld      a,(#C037)
        or      a
        jp      nz,loader_fail
        ld      hl,(#C034)
        ld      a,h
        or      l
        jr      z,.data_zero
        ld      de,#0034
        add     hl,de
        jr      .data_ready
.data_zero:
        ld      hl,#0040
.data_ready:
        ld      a,h
        cp      #04                  ; initial blocks must begin in first 1K
        jp      nc,loader_fail
        ld      de,#C000
        add     hl,de
        ld      (src_ptr),hl
        xor     a
        ld      (src_page),a

        ; NEW2 is required before any wave-memory access.
        call    chip_wait
        ld      a,#05
        out     (OPL_FM1_REG),a
        call    chip_wait
        ld      a,#03
        out     (OPL_FM1_DAT),a

.next_block:
        ld      hl,(src_ptr)
        ld      (block_end_ptr),hl
        ld      a,(src_page)
        ld      (block_end_page),a
        call    read_byte
        cp      #67
        jr      nz,.blocks_done
        call    read_byte
        cp      #66
        jp      nz,loader_fail
        call    read_byte
        ld      (block_type),a
        cp      #84
        jr      z,.read_size
        cp      #87
        jr      nz,.blocks_done
.read_size:
        call    read_byte
        ld      (block_len),a
        call    read_byte
        ld      (block_len+1),a
        call    read_byte
        ld      (block_len+2),a
        call    read_byte
        ld      (block_len+3),a
        call    block_fits_loaded
        jp      c,loader_fail

        ; ROM/RAM size is informational.
        call    read_byte
        call    read_byte
        call    read_byte
        call    read_byte
        ; Start address, little endian.
        call    read_byte
        ld      (start_lo),a
        call    read_byte
        ld      (start_mid),a
        call    read_byte
        ld      (start_hi),a
        call    read_byte
        or      a
        jp      nz,loader_fail

        ; payload length = block length - 8.
        ld      hl,(block_len)
        ld      de,8
        or      a
        sbc     hl,de
        ld      (block_len),hl
        jr      nc,.len_ready
        ld      hl,(block_len+2)
        dec     hl
        ld      (block_len+2),hl
.len_ready:
        call    block_fits_sram
        jp      c,loader_fail
        call    upload_block
        ld      hl,block_count
        inc     (hl)
        jp      .next_block

.blocks_done:
        ld      a,(block_count)
        or      a
        jp      z,loader_fail
        call    compact_and_patch
        or      a
        jp      z,loader_fail
        ld      c,a                   ; page count, keep returned HL intact
        ld      a,(rom_flag)
        or      a
        ld      a,c
        jr      z,.no_rom_flag
        or      #80
.no_rom_flag:
        jr      loader_return

loader_fail:
        xor     a
        ld      h,a
        ld      l,a
loader_return:
        pop     iy
        pop     ix
        jp      page0_return

; CF=1 if current src position + block_len exceeds the loaded 1 MiB.
block_fits_loaded:
        ld      hl,(src_ptr)
        ld      a,h
        and     #3F
        ld      h,a
        ld      a,(src_page)
        ld      d,a
        and     3
        rrca
        rrca
        and     #C0
        or      h
        ld      h,a
        ld      a,d
        srl     a
        srl     a
        ld      e,a
        ld      d,0
        ld      bc,(block_len)
        add     hl,bc
        ex      de,hl
        ld      bc,(block_len+2)
        adc     hl,bc
        ld      a,h
        or      a
        scf
        ret     nz
        ld      a,l
        cp      #10
        jr      nc,.too_far
        or      a
        ret
.too_far:
        scf
        ret

; CF=1 if start address + payload length exceeds 1 MiB SRAM.
block_fits_sram:
        ld      hl,(block_len)
        ld      a,(start_lo)
        ld      e,a
        ld      a,(start_mid)
        ld      d,a
        add     hl,de
        ld      c,l
        ld      b,h
        ld      hl,(block_len+2)
        ld      a,(start_hi)
        ld      e,a
        ld      d,0
        adc     hl,de
        ld      a,h
        or      a
        scf
        ret     nz
        ld      a,l
        cp      #10
        jr      nc,.at_limit
        or      a
        ret
.at_limit:
        jr      nz,.bad
        ld      a,b
        or      c
        ret     z
.bad:   scf
        ret

upload_block:
        ; Address high/mid/low plus SRAM base #200000.
        ld      a,#03
        call    wave_reg
        ld      a,(start_hi)
        add     a,#20
        call    wave_dat
        ld      a,#04
        call    wave_reg
        ld      a,(start_mid)
        call    wave_dat
        ld      a,#05
        call    wave_reg
        ld      a,(start_lo)
        call    wave_dat
        ld      a,#02
        call    wave_reg
        ld      a,#11
        call    wave_dat
        ld      a,#06
        call    wave_reg

        xor     a
        ld      (hdr_pos),a
        ld      (hdr_left),a
        ld      a,(block_type)
        cp      #84
        jr      nz,.fast_test
        ld      a,(start_lo)
        ld      hl,start_mid
        or      (hl)
        inc     hl
        or      (hl)
        jr      nz,.fast_test
        ld      a,#80
        ld      (hdr_left),a
        ld      (rom_flag),a

.patch_test:
        ld      a,(hdr_left)
        or      a
        jr      z,.fast_test
        call    len_is_zero
        jr      z,.finish
        call    read_byte
        ld      e,a
        ld      a,(hdr_pos)
        or      a
        jr      nz,.patch_count
        ld      a,e
        or      #20
        ld      e,a
.patch_count:
        ld      a,(hdr_pos)
        inc     a
        cp      12
        jr      nz,.store_pos
        xor     a
        ld      (hdr_pos),a
        ld      hl,hdr_left
        dec     (hl)
        jr      .patch_write
.store_pos:
        ld      (hdr_pos),a
.patch_write:
        ld      a,e
        call    wave_dat
        call    dec_len
        jr      .patch_test

.fast_test:
        call    len_is_zero
        jr      z,.finish
        ld      hl,(src_ptr)
        xor     a
        sub     l
        ld      c,a
        ld      a,0
        sbc     a,h
        ld      b,a                   ; BC = bytes to page boundary
        ld      hl,(block_len+2)
        ld      a,h
        or      l
        jr      nz,.chunk_ready
        ld      hl,(block_len)
        or      a
        sbc     hl,bc
        jr      nc,.chunk_ready
        ld      bc,(block_len)
.chunk_ready:
        ld      hl,(src_ptr)
        push    bc
.byte_loop:
        in      a,(OPL_STAT)
        rrca
        jr      c,.byte_loop
        ld      a,(hl)
        out     (OPL_WAVE_DAT),a
        inc     hl
        dec     bc
        ld      a,b
        or      c
        jr      nz,.byte_loop
        ld      (src_ptr),hl
        pop     bc
        call    sub_len_bc
        ld      hl,(src_ptr)
        ld      a,h
        or      l
        jr      nz,.fast_test
        call    next_src_page
        jr      .fast_test

.finish:
        ld      a,#02
        call    wave_reg
        ld      a,#10                 ; sound generation, WT base 200000h
        jp      wave_dat

; Compacts loaded data and patches offsets.  It deliberately performs no WC
; calls: the resident wrapper reads the unread file tail after page 5 returns.
compact_and_patch:
        ; delta sectors = block_end/512 - 1
        ld      a,(block_end_page)
        ld      l,a
        ld      h,0
        add     hl,hl
        add     hl,hl
        add     hl,hl
        add     hl,hl
        add     hl,hl
        ld      de,(block_end_ptr)
        ld      a,d
        and     #3E
        rrca
        ld      e,a
        ld      d,0
        add     hl,de
        dec     hl
        ld      a,l
        add     a,a
        ld      (delta+1),a
        xor     a
        ld      (delta),a
        ld      a,h
        add     a,a
        ld      b,a
        ld      a,l
        rlca
        and     1
        or      b
        ld      (delta+2),a
        xor     a
        ld      (delta+3),a

        ld      de,(block_end_ptr)
        ld      a,d
        and     1
        add     a,2
        ld      d,a
        ld      (dst_ptr),de
        call    map_page0
        ld      hl,#C004
        call    sub_header32
        ld      hl,#C014
        call    sub_header32
        ld      hl,#C01C
        call    sub_header32
        ld      hl,(dst_ptr)
        ld      de,#0034
        or      a
        sbc     hl,de
        ld      (#C034),hl
        xor     a
        ld      (#C036),a
        ld      (#C037),a

        ; page count from compacted EOF+4, reject >1 MiB.
        ld      hl,(#C004)
        ld      bc,4
        add     hl,bc
        ld      de,(#C006)
        jr      nc,.size_ok
        inc     de
.size_ok:
        ld      a,d
        or      a
        jp      nz,.compact_fail
        ld      a,e
        cp      #10
        jr      c,.count_pages
        jp      nz,.compact_fail
        ld      a,h
        or      l
        jp      nz,.compact_fail
.count_pages:
        ld      a,e
        add     a,a
        add     a,a
        ld      b,a
        ld      a,h
        rlca
        rlca
        and     3
        add     a,b
        ld      b,a
        ld      a,h
        and     #3F
        or      l
        jr      z,.pages_ready
        inc     b
.pages_ready:
        ld      a,b
        ld      (result_pages),a

        ; Copy [block_end,1MiB) forward using Win3 -> Win0.
        ld      bc,PORT_W0
        in      a,(c)
        push    af
        ld      bc,PORT_W3
        in      a,(c)
        push    af
        xor     a
        ld      (dst_page),a
        ld      hl,(block_end_ptr)
        ld      de,(dst_ptr)
        ld      a,(block_end_page)
        ld      (copy_src_page),a
        add     a,#20
        ld      bc,PORT_W3
        out     (c),a
        ld      a,#20
        ld      bc,PORT_W0
        out     (c),a
.copy:
        ld      a,(hl)
        ld      (de),a
        inc     hl
        inc     de
        ld      a,d
        cp      #40
        jr      nz,.src_wrap
        ld      de,0
        ld      a,(dst_page)
        inc     a
        ld      (dst_page),a
        add     a,#20
        ld      bc,PORT_W0
        out     (c),a
.src_wrap:
        ld      a,h
        or      l
        jr      nz,.copy
        ld      a,(copy_src_page)
        inc     a
        ld      (copy_src_page),a
        cp      64
        jr      z,.copy_done
        add     a,#20
        ld      bc,PORT_W3
        out     (c),a
        ld      hl,#C000
        jr      .copy
.copy_done:
        pop     af
        ld      bc,PORT_W3
        out     (c),a
        pop     af
        ld      bc,PORT_W0
        out     (c),a
        ld      a,(dst_page)
        ld      h,a
        ld      a,d
        or      #C0
        ld      l,a
        ld      a,(result_pages)
        ret
.compact_fail:
        xor     a
        ret

sub_header32:
        push    hl
        ld      a,(hl)
        inc     hl
        or      (hl)
        inc     hl
        or      (hl)
        inc     hl
        or      (hl)
        pop     hl
        ret     z
        push    hl
        ld      e,(hl)
        inc     hl
        ld      d,(hl)
        ex      de,hl
        ld      bc,(delta)
        or      a
        sbc     hl,bc
        ex      de,hl
        ld      (hl),d
        dec     hl
        ld      (hl),e
        pop     hl
        inc     hl
        inc     hl
        ld      e,(hl)
        inc     hl
        ld      d,(hl)
        ex      de,hl
        ld      bc,(delta+2)
        sbc     hl,bc
        ex      de,hl
        ld      (hl),d
        dec     hl
        ld      (hl),e
        ret

read_byte:
        ld      hl,(src_ptr)
        ld      a,(hl)
        inc     hl
        ld      (src_ptr),hl
        push    af
        ld      a,h
        or      l
        jr      nz,.no_wrap
        call    next_src_page
.no_wrap:
        pop     af
        ret

next_src_page:
        ld      a,(src_page)
        inc     a
        ld      (src_page),a
        call    map_vpl
        ld      hl,#C000
        ld      (src_ptr),hl
        ret

len_is_zero:
        ld      hl,block_len
        ld      a,(hl)
        inc     hl
        or      (hl)
        inc     hl
        or      (hl)
        inc     hl
        or      (hl)
        ret

dec_len:
        ld      hl,block_len
        ld      a,(hl)
        sub     1
        ld      (hl),a
        inc     hl
        ld      a,(hl)
        sbc     a,0
        ld      (hl),a
        inc     hl
        ld      a,(hl)
        sbc     a,0
        ld      (hl),a
        inc     hl
        ld      a,(hl)
        sbc     a,0
        ld      (hl),a
        ret

sub_len_bc:
        ld      hl,(block_len)
        or      a
        sbc     hl,bc
        ld      (block_len),hl
        ret     nc
        ld      hl,(block_len+2)
        dec     hl
        ld      (block_len+2),hl
        ret

chip_wait:
        in      a,(OPL_STAT)
        rrca
        jr      c,chip_wait
        ret
wave_reg:
        push    af
        call    chip_wait
        pop     af
        out     (OPL_WAVE_REG),a
        ret
wave_dat:
        push    af
        call    chip_wait
        pop     af
        out     (OPL_WAVE_DAT),a
        ret

map_page0:
        xor     a
map_vpl:
        add     a,#20
        ld      bc,PORT_W3
        out     (c),a
        ret

sig_vgm:        DB "Vgm "
src_page:       DB 0
src_ptr:        DW #C000
block_end_page: DB 0
block_end_ptr:  DW 0
block_type:     DB 0
block_count:    DB 0
block_len:      DD 0
start_lo:       DB 0
start_mid:      DB 0
start_hi:       DB 0
hdr_pos:        DB 0
hdr_left:       DB 0
rom_flag:       DB 0
delta:          DD 0
dst_page:       DB 0
dst_ptr:        DW 0
copy_src_page:  DB 0
result_pages:   DB 0

        ASSERT  $ <= #B800, "OPL4 cold loader exceeds reserved overlay"

; #6020 maps page 5 and jumps to the loader but deliberately does not restore
; Win2.  Restore the physical page-0 mapping ourselves.  OUT is placed at
; #BFFD; its following instruction is fetched from the newly mapped page 0,
; where cold_return.s puts a single RET at #BFFF.  E preserves the loader's A
; result while D carries the physical page number to OUT (C),D, so returned A
; and HL remain intact.
        ORG     #BFEF
page0_return:
        ld      e,a
        ld      a,(#6002)
        sub     5
        ld      (#6002),a
        ld      d,a
        ld      a,e
        ld      bc,PORT_W2            ; #12AF, Win2
        out     (c),d
        ASSERT  $ == #BFFF, "page0 return trampoline must end at #BFFE"

        SAVEBIN "build/opl4_loader.bin", #9800, $-#9800
