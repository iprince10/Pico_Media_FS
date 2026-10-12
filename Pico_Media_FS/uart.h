#ifndef uart_h
#define uart_h

void uart0_init(void);
void uart0_putc(char);
void uart0_puts(const char *);
void uart0_putnum(uint64_t num);

void uart1_init(void);
void uart1_putc(char c);
char uart1_getc(void);
void uart1_write_bytes(const uint8_t *data, uint8_t len);
int uart1_has_data(void);
void uart0_puthex(uint8_t byte);

#endif