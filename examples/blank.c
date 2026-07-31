volatile unsigned char *vera_ctrl = (unsigned char *)0x9F25;
int main(void) { *vera_ctrl = 0; return 0; }
