#include <stdint.h>
#include "bitmap.h"
#include "timer.h"

#define SIO_BASE 0xd0000000u
#define SIO_GPIO_OE (*(volatile uint32_t *)(SIO_BASE + 0x020))
#define SIO_GPIO_OE_SET (*(volatile uint32_t *)(SIO_BASE + 0x024))
#define SIO_GPIO_OE_CLEAR (*(volatile uint32_t *)(SIO_BASE + 0x028))
#define SIO_GPIO_OUT (*(volatile uint32_t *)(SIO_BASE + 0x010))
#define SIO_GPIO_OUT_SET (*(volatile uint32_t *)(SIO_BASE + 0x014))
#define SIO_GPIO_OUT_CLEAR (*(volatile uint32_t *)(SIO_BASE + 0x018))

#define IO_BANK0_BASE 0x40014000u
#define GPIO19_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0x09c)) // spio TX
#define GPIO16_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0x084)) // spio RX
#define GPIO17_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0x08c)) // spio CS
#define GPIO18_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0x094)) // spio CLK
#define GPIO20_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0x0a4)) // spio C/D
#define GPIO21_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0x0ac)) // spio Hardware Reset
#define GPIO_FUNC_SPI0 (1u)
#define GPIO_FUNC_SIO (5u)

#define PADS_BANK0_BASE 0x4001c000u
#define PAD_GPIO19 (*(volatile uint32_t *)(PADS_BANK0_BASE + 0x50))
#define PAD_GPIO16 (*(volatile uint32_t *)(PADS_BANK0_BASE + 0x44))
#define PAD_GPIO17 (*(volatile uint32_t *)(PADS_BANK0_BASE + 0x48))
#define PAD_GPIO18 (*(volatile uint32_t *)(PADS_BANK0_BASE + 0x4c))
#define PAD_GPIO20 (*(volatile uint32_t *)(PADS_BANK0_BASE + 0x54))
#define PAD_GPIO21 (*(volatile uint32_t *)(PADS_BANK0_BASE + 0x58))

#define SPI0_BASE 0x4003c000u
#define SPI0_SSPCR0 (*(volatile uint32_t *)(SPI0_BASE + 0x000))
#define SPI0_SSPCR1 (*(volatile uint32_t *)(SPI0_BASE + 0x004))
#define SPI0_SSPCR1_SSE (1u << 1)
#define SPI0_SSPDR (*(volatile uint32_t *)(SPI0_BASE + 0x008))
#define SPI0_SSPSR (*(volatile uint32_t *)(SPI0_BASE + 0x00c))
#define SPI0_SSPSR_BSY (1u << 4)
#define SPI0_SSPSR_TNF (1u << 1)
#define SPI0_SSPCPSR (*(volatile uint32_t *)(SPI0_BASE + 0x010))

#define CLOCKS_BASE 0x40008000u
#define CLK_PERI_CTRL (*(volatile uint32_t *)(CLOCKS_BASE + 0x48))
#define CLK_PERI_CTRL_ENABLE (1u << 11)

#define RESETS_BASE 0x4000c000u
#define RESETS_RESET (*(volatile uint32_t *)(RESETS_BASE + 0x0))
#define RESETS_RESET_SPI0 (1u << 16)
#define RESETS_RESET_DONE (*(volatile uint32_t *)(RESETS_BASE + 0x8))

#define TANK_X 60
#define TANK_Y 70
#define TANK_W 120
#define TANK_H 200
#define TANK_BORDER 2

#define COLOR_WHITE 0xFFFF
#define COLOR_BLACK 0x0000
#define COLOR_BLUE 0x001F    // blue
#define COLOR_EMPTY 0xC618   // light gray, distinguishes empty tank from bare background
#define COLOR_OFFLINE 0xFC00 // warning orange, tweak to taste

#define FULL_CM 20
#define EMPTY_CM 150

void ili9341_init();
void spi0_init();
void ili9341_hw_reset();
void spi_write_byte(uint8_t byte);
void ili9341_write_command(uint8_t cmd);
void ili9341_write_data(uint8_t data);
void ili9341_send_init_commands();
void ili9341_fill_white(void);
static int find_index_char(char c);
void ili9341_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg_color, uint16_t bg_color, uint8_t scale);
void ili9341_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t fg_color, uint16_t bg_color, uint8_t scale);

void ili9341_fill_area(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void format_distance(char *buff, uint32_t distance);
void display_init_log(void);

char screen_lines[8][32]; // 8 reading slots , 32 chars each
uint8_t line_count = 0;   // slots filled

void ili9341_init(void)
{
    PAD_GPIO16 = 0x63;  //these are the pad config setting for each gpio pin
    PAD_GPIO17 = 0x23;
    PAD_GPIO18 = 0x23;
    PAD_GPIO19 = 0x23;
    PAD_GPIO20 = 0x23;
    PAD_GPIO21 = 0x23;

    GPIO16_CTRL = GPIO_FUNC_SPI0; // spi0 rx miso
    GPIO17_CTRL = GPIO_FUNC_SPI0; // spi0 csn
    GPIO18_CTRL = GPIO_FUNC_SPI0; // spi0 sck
    GPIO19_CTRL = GPIO_FUNC_SPI0; // spi0 tx mosi
    GPIO20_CTRL = GPIO_FUNC_SIO;  // sio d/c pin
    GPIO21_CTRL = GPIO_FUNC_SIO;  // sio hw reset pin

    SIO_GPIO_OE_SET = (1u << 20) | (1u << 21); // enable output for gpio 20 nd 21
    SIO_GPIO_OUT_SET = (1u << 21);             // rst high
    SIO_GPIO_OUT_CLEAR = (1u << 20);           // clear d/c pin

    spi0_init(); //spi0 initialise 

    ili9341_hw_reset(); //hardware reset using hardware pin 21

    ili9341_send_init_commands(); // initialising commands 
}

void spi0_init(void)
{
    CLK_PERI_CTRL |= CLK_PERI_CTRL_ENABLE;  //enable clock , by default clock is gated for the spi0 peripheral
    RESETS_RESET &= ~(RESETS_RESET_SPI0);  // do a software reset and wait for reset done signal
    while (!(RESETS_RESET_DONE & RESETS_RESET_SPI0))
    {
    };

    // the clock freq here is 10.42 MHz because of 2 in cpsr and 5 in sspcr0

    SPI0_SSPCPSR = 2; // this is for spio clock register 
    SPI0_SSPCR0 = 0x0507;  // control register of spi0 , the 5 is the clock divisor inside control register 0 
    SPI0_SSPCR1 = SPI0_SSPCR1_SSE;  //synchronous serial port enable 
    delay_ms(100);
}

void ili9341_hw_reset()
{
    SIO_GPIO_OUT_CLEAR = (1u << 21); // do a hardware reset of the display
    delay_ms(1);
    SIO_GPIO_OUT_SET = (1u << 21);
    delay_ms(120);
}

void spi_write_byte(uint8_t byte)
{
    while (!(SPI0_SSPSR & SPI0_SSPSR_TNF)) // wait for transmit fifo empty
    {
    }

    SPI0_SSPDR = byte; // write a byte to the spi0 data register 
    while (SPI0_SSPSR & SPI0_SSPSR_BSY) // read and discard the echo byte
    {
    }
    volatile uint32_t dummy = SPI0_SSPDR;
    (void)dummy;
}

void ili9341_write_command(uint8_t cmd)
{
    SIO_GPIO_OUT_CLEAR = (1u << 20); // DC = low command is sending
    spi_write_byte(cmd);
}

void ili9341_write_data(uint8_t data)
{
    SIO_GPIO_OUT_SET = (1u << 20); // DC = high data is sending
    spi_write_byte(data);
}

void ili9341_send_init_commands(void)
{
    ili9341_write_command(0x11); // exit sleep mode
    delay_ms(120);

    ili9341_write_command(0x3A); // pixel fomart to rgb565
    ili9341_write_data(0x55);

    ili9341_write_command(0x36); // memory access orientation
    ili9341_write_data(0x48);

    ili9341_write_command(0x29); // turn display on
    delay_ms(100);
}

void ili9341_fill_white(void)
{
    ili9341_write_command(0x2A); // column address set
    ili9341_write_data(0x00);    // 0
    ili9341_write_data(0x00);
    ili9341_write_data(0x00);
    ili9341_write_data(0xEF); // 239

    ili9341_write_command(0x2B); // row address set
    ili9341_write_data(0x00);    // 0
    ili9341_write_data(0x00);
    ili9341_write_data(0x01);
    ili9341_write_data(0x3F); // 319

    ili9341_write_command(0x2C); // memory write

    for (uint32_t i = 0; i < 76800; i++)
    {
        ili9341_write_data(0xff);
        ili9341_write_data(0xff);
    }
}

static int find_index_char(char c)
{
    int index;

    if (c >= '0' && c <= '9')
        index = c - '0';

    else if (c >= 'A' && c <= 'Z')
        index = c - 'A' + 10;

    else if (c >= 'a' && c <= 'z')
        index = c - 'a' + 36;

    else
    {
        const char special[] = ",.:;\"/\\|()% ";
        const char *p = special;

        while (*p && *p != c)
        {
            p++;
        }

        if (!*p)
        {
            return -1;
        }

        index = 62 + (p - special);
    }

    return index;
}

void ili9341_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg_color, uint16_t bg_color, uint8_t scale)
{
    int index = find_index_char(c); // get index from the bitmap of the current character

    if (index < 0)
        return;

    uint16_t width = 10 * scale;
    uint16_t height = 14 * scale;

    // column write
    ili9341_write_command(0x2A);
    ili9341_write_data(x >> 8);
    ili9341_write_data((x & 0xff));
    ili9341_write_data((x + width - 1) >> 8);
    ili9341_write_data((x + width - 1) & 0xff);

    // row write
    ili9341_write_command(0x2B);
    ili9341_write_data(y >> 8);
    ili9341_write_data((y & 0xff));
    ili9341_write_data((y + height - 1) >> 8);
    ili9341_write_data((y + height - 1) & 0xff);

    // memory write
    ili9341_write_command(0x2C);

    for (int row = 0; row < height; row++)
    {
        uint16_t bitmap_row = row / scale;
        uint16_t bitmap = font_10x14[index][bitmap_row];
        for (int col = 0; col < width; col++)
        {
            uint16_t bitmap_col = col / scale;
            if (bitmap & (1u << (9 - bitmap_col)))
            {
                ili9341_write_data(fg_color >> 8);
                ili9341_write_data(fg_color & 0xFF);
            }
            else
            {
                ili9341_write_data(bg_color >> 8);
                ili9341_write_data(bg_color & 0xFF);
            }
        }
    }
}

void ili9341_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t fg_color, uint16_t bg_color, uint8_t scale)
{
    uint16_t cursor_x = x;
    uint16_t cursor_y = y;
    uint16_t char_width = 10 * scale;
    uint16_t char_height = 14 * scale;
    uint16_t x_spacing = 2;
    uint16_t y_spacing = 4;

    while (*str)
    {
        if (*str == '\n')
        {
            cursor_x = x;
            cursor_y += char_height + y_spacing;
        }
        else
        {
            ili9341_draw_char(cursor_x, cursor_y, *str, fg_color, bg_color, scale);
            cursor_x += char_width + x_spacing;
        }
        str++;
    }
}

void ili9341_fill_area(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    // CAset
    ili9341_write_command(0x2A);
    ili9341_write_data(x >> 8);             // start column address high byte
    ili9341_write_data((x & 0xff));         // start column low byte
    ili9341_write_data((x + w - 1) >> 8);   // end column high byte
    ili9341_write_data((x + w - 1) & 0xff); // end column low byte

    // PAset
    ili9341_write_command(0x2B);
    ili9341_write_data(y >> 8);             // start row address high byte
    ili9341_write_data((y & 0xff));         // start row low byte
    ili9341_write_data((y + h - 1) >> 8);   // end row high byte
    ili9341_write_data((y + h - 1) & 0xff); // end row low byte

    // Memory write
    ili9341_write_command(0x2C);

    for (uint32_t i = 0; i < (uint32_t)(w * h); i++)
    {
        ili9341_write_data(color >> 8);
        ili9341_write_data(color & 0xff);
    }
}

void format_distance(char *buff, uint32_t distance)
{
    if (distance == 0)
    {
        buff[0] = '0';
        buff[1] = ' ';
        buff[2] = 'c';
        buff[3] = 'm';
        buff[4] = '\0';
        return;
    }

    char tmp[8];
    int tidx = 0;
    while (distance > 0)
    {
        tmp[tidx++] = '0' + (distance % 10);
        distance /= 10;
    }

    int bidx = 0;
    buff[bidx++] = 'D';
    buff[bidx++] = ' ';
    buff[bidx++] = ':';
    buff[bidx++] = ' ';
    for (int i = tidx - 1; i >= 0; i--)
    {
        buff[bidx++] = tmp[i];
    }
    buff[bidx++] = ' ';
    buff[bidx++] = 'c';
    buff[bidx++] = 'm';
    buff[bidx] = '\0';
}

void display_init_log(void)
{
    uint16_t cursor_x = 2;
    uint16_t cursor_y = 4;
    uint16_t char_width = 10 * 2;
    uint16_t char_height = 14 * 2;
    uint16_t x_spacing = 1;
    uint16_t y_spacing = 2;

    const char *str = "TANK WATER%"; // header text

    while (*str)
    {
        if (*str == '\n')
        {
            cursor_x = 1;
            cursor_y += char_height + y_spacing;
        }
        else if (*str == ' ')
        {
            ili9341_draw_char(cursor_x, cursor_y, *str, COLOR_BLUE, 0xffff, 2);
            cursor_x += char_width + x_spacing;
        }
        else
        {
            ili9341_draw_char(cursor_x, cursor_y, *str, COLOR_BLUE, 0xffff, 2);
            cursor_x += char_width + x_spacing;
        }
        str++;
    }

    ili9341_fill_area(4, 310, 232, 4, COLOR_BLUE); // bottom line
}