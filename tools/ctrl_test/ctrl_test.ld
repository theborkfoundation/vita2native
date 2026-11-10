ENTRY(module_start)

SECTIONS
{
    . = 0x81000000;

    .text : ALIGN(4)
    {
        KEEP(*(.sce_module))
        *(.text.start)
        *(.text*)
        *(.rodata*)
    }

    . = 0x81010000;

    .data : ALIGN(4)
    {
        *(.data*)
    }

    .bss : ALIGN(4)
    {
        *(.bss*)
        *(COMMON)
    }

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.comment)
        *(.note*)
    }
}
