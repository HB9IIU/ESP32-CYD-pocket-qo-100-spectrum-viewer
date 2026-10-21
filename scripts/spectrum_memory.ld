ASSERT(_iram_end <= 0x400a8000, "RF capture overlaps IRAM")
ASSERT(_data_end <= 0x3ffe8000, "RF capture overlaps data")
ASSERT(_bss_end <= 0x3ffe8000, "RF capture overlaps BSS")
