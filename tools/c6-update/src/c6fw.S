/* Embeds the C6 firmware image. Path is relative to the project folder. */
    .section .rodata.c6fw, "a"
    .balign 4
    .global c6fw_start
c6fw_start:
    .incbin "fw/esp32c6-v2.12.11.bin"
    .global c6fw_end
c6fw_end:
    .byte 0
