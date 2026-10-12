#include <stdint.h>
#include "timer.h"

#define IO_BANK0_BASE 0x40014000u
#define GPIO0_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0x004))
#define GPIO4_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0x024))
#define GPIO5_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0x02c))

#define UART0_BASE 0x40034000u
#define UART0_FUNC 2u
#define UART0_DR (*(volatile uint32_t *)(UART0_BASE + 0x000))
#define UART0_FR (*(volatile uint32_t *)(UART0_BASE + 0x018))
#define UART0_FR_TXFF (1u << 5)
#define UART0_IBRD (*(volatile uint32_t *)(UART0_BASE + 0x024))
#define UART0_FBRD (*(volatile uint32_t *)(UART0_BASE + 0x028))
#define UART0_CR (*(volatile uint32_t *)(UART0_BASE + 0x030))
#define UART0_CR_UARTEN (1u << 0)
#define UART0_CR_TXE_EN (1u << 8)
#define UART0_LCR_H (*(volatile uint32_t *)(UART0_BASE + 0x02c))
#define UART0_LCR_H_FEN (1u << 4)
#define UART0_LCR_H_WLEN (3u << 5)

#define UART1_BASE 0x40038000u
#define UART1_FUNC 2u
#define UART1_DR (*(volatile uint32_t *)(UART1_BASE + 0x000))
#define UART1_FR (*(volatile uint32_t *)(UART1_BASE + 0x018))
#define UART1_FR_TXFF (1u << 5)
#define UART1_FR_RXFE (1u << 4)
#define UART1_IBRD (*(volatile uint32_t *)(UART1_BASE + 0x024))
#define UART1_FBRD (*(volatile uint32_t *)(UART1_BASE + 0x028))
#define UART1_CR (*(volatile uint32_t *)(UART1_BASE + 0x030))
#define UART1_CR_UARTEN (1u << 0)
#define UART1_CR_TXE_EN (1u << 8)
#define UART1_CR_RXE_EN (1u << 9)
#define UART1_LCR_H (*(volatile uint32_t *)(UART1_BASE + 0x02c))
#define UART1_LCR_H_FEN (1u << 4)
#define UART1_LCR_H_WLEN (3u << 5)

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

void uart0_init(void)
{
    UART0_CR = 0;

    GPIO0_CTRL = UART0_FUNC;

    UART0_IBRD = 67;
    UART0_FBRD = 52;

    UART0_LCR_H = UART0_LCR_H_WLEN | UART0_LCR_H_FEN; // 8N1 8data, No parity, 1 stop bit, enable fifo

    UART0_CR = UART0_CR_UARTEN | UART0_CR_TXE_EN;
}

void uart0_putc(char c)
{
    while ((UART0_FR & UART0_FR_TXFF) != 0)
    {
        // wait buffer full
    }
    UART0_DR = c;
}

void uart0_puts(const char *send)
{

    while (*send != '\0')
    {
        uart0_putc(*send);
        send++;
    }
}

void uart0_putnum(uint64_t num)
{
    if (num == 0)
    {
        uart0_putc('0');
        return;
    }
    uint8_t buffer[20];
    int index = 0;

    while (num > 0)
    {
        buffer[index] = (num % 10) + '0';
        num /= 10;
        index++;
    }

    for (int i = index - 1; i >= 0; i--)
    {
        uart0_putc(buffer[i]);
    }
}

void uart1_init(void)
{
    UART1_CR = 0;
    GPIO4_CTRL = UART1_FUNC;
    GPIO5_CTRL = UART1_FUNC;

    UART1_IBRD = 813; // for 9600 baud rate
    UART1_FBRD = 51;

    UART1_LCR_H = UART1_LCR_H_FEN | UART1_LCR_H_WLEN; // 8N1 8data, No parity, 1 stop bit, enable fifo

    UART1_CR = UART1_CR_UARTEN | UART1_CR_RXE_EN | UART1_CR_TXE_EN;
}

void uart1_putc(char c)
{
    while ((UART1_FR & UART1_FR_TXFF) != 0)
    {
        // wait while buffer full
    }
    UART1_DR = c;
}

char uart1_getc(void)
{
    while ((UART1_FR & UART1_FR_RXFE) != 0)
    {
        // wait buffer empty
    }
    uint32_t data = UART1_DR;
    return (char)(data & 0x00ff);
}

void uart1_write_bytes(const uint8_t *data, uint8_t len)
{
    for (int i = 0; i < len; i++)
    {
        uart1_putc((char)data[i]);
    }
}

int uart1_has_data(void)
{
    return (UART1_FR & UART1_FR_RXFE) == 0;
}

void uart0_puthex(uint8_t byte)
{
    const char hex[] = "0123456789ABCDEF";
    uart0_putc(hex[(byte >> 4) & 0x0F]);
    uart0_putc(hex[byte & 0x0F]);
    uart0_putc(' ');
}