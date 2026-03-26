// Quick ABI test: check calling convention for various signatures
extern void f_u16_u8(unsigned int a, unsigned char b);
extern void f_u8_u16(unsigned char a, unsigned int b);
extern void f_ptr_u8(void *a, unsigned char b);
extern void f_ptr_ptr_u8(void *a, void *b, unsigned char c);
extern unsigned char f_u8_u8(unsigned char a, unsigned char b);
extern void f_u8_u16_ptr(unsigned char a, unsigned int b, void *c);
extern void f_u8_u8_u16(unsigned char a, unsigned char b, unsigned int c);
extern void f_ptr_u8_u8_u16(void *w, unsigned char y, unsigned char x, unsigned int len);
extern void f_ptr_ptr_u8_u8_u16(void *w, void *s, unsigned char y, unsigned char x, unsigned int len);
extern void f_ptr_u8_ptr_u8_u8(void *a, unsigned char b, void *c, unsigned char d, unsigned char e);

void test(void) {
    f_u16_u8(0x1234, 0x56);
    f_u8_u16(0x78, 0x9ABC);
    f_ptr_u8((void*)0xC000, 0x20);
    f_ptr_ptr_u8((void*)0x1000, (void*)0x2000, 0x33);
    f_u8_u8(0x11, 0x22);
    f_u8_u16_ptr(0xAA, 0xBBCC, (void*)0xDDEE);
    f_u8_u8_u16(0x11, 0x22, 0x3344);
    f_ptr_u8_u8_u16((void*)0x1000, 0x11, 0x22, 0x3344);
    f_ptr_ptr_u8_u8_u16((void*)0x1000, (void*)0x2000, 0x11, 0x22, 0x3344);
    f_ptr_u8_ptr_u8_u8((void*)0x1000, 0x11, (void*)0x2000, 0x22, 0x33);
}
