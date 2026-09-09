; Assemble with TI C6000 CGT, not GNU (whose compact Uspk scatter differs).
; cl6x --silicon_version=6740 --asm_listing spkernel-oracle.asm
; dis6x spkernel-oracle.obj
; TI CGT 8.5.0: 3,0 -> 9c67; 6,0 -> dc66; 24,0 -> 1f66.
    .text
    .align 32
    SPLOOP 1
    NOP
    SPKERNEL 3,0
    NOP 5
    NOP
    SPLOOP 1
    NOP
    SPKERNEL 6,0
    NOP 5
    NOP
    SPLOOP 1
    NOP
    SPKERNEL 24,0
    NOP 5
    NOP
