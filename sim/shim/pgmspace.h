#pragma once
// Flash and RAM are the same address space on the desktop.
#include <stdint.h>
#include <string.h>

#define PROGMEM
#define PGM_P const char*
#define PGM_VOID_P const void*
#define PSTR(s) (s)
#define FPSTR(p) (reinterpret_cast<const __FlashStringHelper*>(p))
#define F(s) (reinterpret_cast<const __FlashStringHelper*>(s))

#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#define pgm_read_word(addr) (*(const uint16_t*)(addr))
#define pgm_read_dword(addr) (*(const uint32_t*)(addr))
#define pgm_read_float(addr) (*(const float*)(addr))
#define pgm_read_ptr(addr) (*(void* const*)(addr))
#define pgm_read_byte_near(addr) pgm_read_byte(addr)
#define pgm_read_word_near(addr) pgm_read_word(addr)
#define pgm_read_dword_near(addr) pgm_read_dword(addr)
#define pgm_read_float_near(addr) pgm_read_float(addr)
#define pgm_read_ptr_near(addr) pgm_read_ptr(addr)
#define pgm_read_byte_far(addr) pgm_read_byte(addr)
#define pgm_read_word_far(addr) pgm_read_word(addr)

#define memcpy_P memcpy
#define memcmp_P memcmp
#define strcpy_P strcpy
#define strncpy_P strncpy
#define strcmp_P strcmp
#define strncmp_P strncmp
#define strcasecmp_P strcasecmp
#define strlen_P strlen
#define strnlen_P strnlen
#define strstr_P strstr
#define sprintf_P sprintf
#define snprintf_P snprintf
#define vsnprintf_P vsnprintf
#define printf_P printf
