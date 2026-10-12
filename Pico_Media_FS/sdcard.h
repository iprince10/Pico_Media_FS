#ifndef sdcard_h
#define sdcard_h

void spi1_init(void);
uint8_t spi1_transfer(uint8_t data);
void cs_select(void);
void cs_deselect(void);
void sd_dummy_clocks(void);
void sd_send_command(uint8_t cmd, uint32_t arg, uint8_t crc);
uint8_t sd_read_r1(void);
uint8_t sd_cmd0(void);
uint8_t sd_cmd8(void);
uint8_t sd_acmd41(void);
uint8_t sd_cmd58(void);
void sd_set_clk_fast(void);
uint8_t sd_read_block(uint32_t block, uint8_t *buf);
uint8_t sd_integrity_test(uint32_t first, int count);

#endif