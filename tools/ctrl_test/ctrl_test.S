/*
 * Import stubs and SCE module metadata for ctrl_test.c.
 *
 * Each stub is three words of placeholder code. The loader resolves the NID
 * and rewrites the stub with movw/movt/bx pointing at the host function.
 */

    .syntax unified
    .arm

    .section .text, "ax", %progbits
    .align 2

    .global guest_peek
    .type guest_peek, %function
guest_peek:
    .word 0, 0, 0

    .global guest_printf
    .type guest_printf, %function
guest_printf:
    .word 0, 0, 0

    .global guest_delay
    .type guest_delay, %function
guest_delay:
    .word 0, 0, 0

    .global guest_exit
    .type guest_exit, %function
guest_exit:
    .word 0, 0, 0

    .global guest_setfb
    .type guest_setfb, %function
guest_setfb:
    .word 0, 0, 0

    /*
     * Module info. This section is linked at the start of the RX segment and
     * e_entry points at offset zero, so the loader reads it from the segment
     * base. All offsets inside the structure are relative to that base.
     */
    .section .sce_module, "a", %progbits
    .align 2

    .global module_info
module_info:
    .short 0x0000                  /* attributes */
    .short 0x0101                  /* version */
    .ascii "v2n-ctrl-test\0"       /* name[27] */
    .space 0x1F - (. - module_info)
    .byte 0                        /* module type: executable */
    .space 0x20 - (. - module_info)
    .word 0                        /* gp_value */
    .word 0                        /* export_top */
    .word 0                        /* export_end */
    .word __imports - 0x81000000   /* import_top */
    .word __imports_end - 0x81000000
    .word 0x12345678               /* module_nid */
    .space 0x44 - (. - module_info)
    .word module_start - 0x81000000
    .word 0xFFFFFFFF               /* module_stop */
    .space 0x5C - (. - module_info)
    .word 0x03570011               /* module_sdk_version */
    .space 0x60 - (. - module_info)

    .align 2
str_ctrl:
    .asciz "SceCtrl"
str_libkernel:
    .asciz "SceLibKernel"
str_threadmgr:
    .asciz "SceThreadmgr"
str_displayuser:
    .asciz "SceDisplayUser"

    .align 2
nids_ctrl:
    .word 0xA9C3CED6              /* sceCtrlPeekBufferPositive */
entries_ctrl:
    .word guest_peek

nids_libkernel:
    .word 0xFA26BC62              /* sceClibPrintf */
    .word 0x7595D9AA              /* sceKernelExitProcess */
entries_libkernel:
    .word guest_printf
    .word guest_exit

nids_threadmgr:
    .word 0x4B675D05              /* sceKernelDelayThread */
entries_threadmgr:
    .word guest_delay

nids_displayuser:
    .word 0x7A410B64              /* sceDisplaySetFrameBuf */
entries_displayuser:
    .word guest_setfb

    .align 2
__imports:
    /* SceCtrl */
    .short 0x24, 1, 0, 1, 0, 0
    .word 0xD197E3C7
    .word str_ctrl
    .word nids_ctrl
    .word entries_ctrl
    .word 0, 0

    /* SceLibKernel */
    .short 0x24, 1, 0, 2, 0, 0
    .word 0xCAE9ACE6
    .word str_libkernel
    .word nids_libkernel
    .word entries_libkernel
    .word 0, 0

    /* SceThreadmgr */
    .short 0x24, 1, 0, 1, 0, 0
    .word 0x859A24B1
    .word str_threadmgr
    .word nids_threadmgr
    .word entries_threadmgr
    .word 0, 0

    /* SceDisplayUser */
    .short 0x24, 1, 0, 1, 0, 0
    .word 0x4FAACD11
    .word str_displayuser
    .word nids_displayuser
    .word entries_displayuser
    .word 0, 0
__imports_end:
