; Hardware floating point pieces VC9 cannot express, for the Zune HD's ARM1136 (VFPv2).
;
; VC9 compiles float arithmetic to VFP instructions (/QRfpe-), but it has no square-root
; intrinsic: sqrtf() is a call into coredll, whose math library is software floating point
; because coredll also has to run on ARMs without an FPU. That call costs microseconds; the
; VFP instruction costs about 20 cycles. compat/zune_math.h points the game's sqrtf here.
;
; CE's ARM calling convention passes and returns floats in core registers (r0), whatever the
; code generation, so each routine moves the value into the VFP and back. The VFP opcodes are
; written as data words: this assembler (armasm 15) is not told about a VFP unit.
;
; Assembled by tools/build.py: armasm -arch 5T zune_vfp.asm zune_vfp.obj

        AREA    |.text|, CODE, READONLY

        EXPORT  zune_sqrtf
        EXPORT  zune_fpscr_get
        EXPORT  zune_fpscr_set

; float zune_sqrtf(float x)
zune_sqrtf
        DCD     0xEE000A10              ; fmsr   s0, r0
        DCD     0xEEB10AC0              ; fsqrts s0, s0
        DCD     0xEE100A10              ; fmrs   r0, s0
        bx      lr

; unsigned zune_fpscr_get(void): the calling thread's VFP status and control register
zune_fpscr_get
        DCD     0xEEF10A10              ; fmrx   r0, fpscr
        bx      lr

; void zune_fpscr_set(unsigned value)
zune_fpscr_set
        DCD     0xEEE10A10              ; fmxr   fpscr, r0
        bx      lr

        END
