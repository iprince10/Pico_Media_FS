#include <stdint.h>
#include <stdio.h>
#include "bitmap.h"
#include "fat32.h"
#include "ili9341.h"
#include "sdcard.h"
#include "timer.h"
#include "uart.h"

/* ---- counters defined in sdcard.c / fat32.c ---- */
extern uint64_t sd_time_us;
extern uint32_t sd_read_count;
extern uint64_t lcd_time_us;
extern uint32_t lcd_bytes;

#define IO_BANK0_BASE 0x40014000u

#define GPIO_FUNC_SIO 5u
#define GPIO25_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0xcc))
#define SIO_BASE 0xd0000000u
#define SIO_GPIO_OE (*(volatile uint32_t *)(SIO_BASE + 0x020))
#define SIO_GPIO_OUT (*(volatile uint32_t *)(SIO_BASE + 0x010))
#define SIO_GPIO_OUT_XOR (*(volatile uint32_t *)(SIO_BASE + 0x01c))
#define GPIO25 (1u << 25)

void led_init(void)
{
  // blink code just for testing
  GPIO25_CTRL = GPIO_FUNC_SIO;
  SIO_GPIO_OE |= GPIO25;
  SIO_GPIO_OUT |= GPIO25;
}

/* ---- SD bring-up ----
   the protocol lives in exactly one place: sd_bringup().
   sd_bringup_debug() is a wrapper that runs it and narrates the result,
   so debug output is optional and never duplicated. */

typedef struct
{
  int cmd0;      // 1 = idle ok
  uint8_t cmd8;  // 1 = v2 echoed, 2 = v1 (old), 0 = fail
  int acmd41;    // 0 = ready
  uint8_t cmd58; // 1 = SDHC block addressing, 2 = SDSC byte, 0 = fail
} sd_status_t;

// talks to the card - no uart. leaves the card initialised and on the fast
// clock, ready for block reads. returns 0, or the negative step number that
// failed (so the caller can report which stage died).
static int sd_bringup(sd_status_t *st)
{
  st->cmd0 = sd_cmd0();
  if (!st->cmd0)
    return -1;

  st->cmd8 = sd_cmd8();
  if (st->cmd8 == 0)
    return -2;

  st->acmd41 = sd_acmd41();
  if (st->acmd41 != 0)
    return -3;

  st->cmd58 = sd_cmd58();
  if (st->cmd58 == 0)
    return -4;

  sd_set_clk_fast(); // last: only now is the card allowed full speed
  return 0;
}

// runs the bring-up and prints every step, in the same words as before.
static void sd_bringup_debug(void)
{
  sd_status_t st = {0};

  int rc = sd_bringup(&st);

  uart0_puts("---- SD bring-up ----\r\n");

  uart0_puts("CMD0....\r\n");
  uart0_puts(st.cmd0 ? "CMD0 Ok - card in idle\r\n" : "CMD0 FAIL\r\n");

  uart0_puts("CMD8....\r\n");
  if (st.cmd8 == 1)
    uart0_puts("CMD8 Ok - modern card, token echoed\r\n");
  else if (st.cmd8 == 2)
    uart0_puts("CMD8 - V1 card (old)\r\n");
  else
    uart0_puts("CMD8 FAIL\r\n");

  uart0_puts("CMD55 + ACMD41....\r\n");
  uart0_puts(st.acmd41 == 0 ? "ACMD41 Ok - card ready\r\n" : "ACMD41 FAIL\r\n");

  uart0_puts("CMD58....\r\n");
  if (st.cmd58 == 1)
    uart0_puts("CMD58 Ok - block addressing (SDHC/SDXC)\r\n");
  else if (st.cmd58 == 2)
    uart0_puts("CMD58 - byte addressing (SDSC)\r\n");
  else
    uart0_puts("CMD58 FAIL\r\n");

  if (rc == 0)
  {
    uart0_puts("SPI1 CLK is 12.5 MHz now\r\n");
  }
  else
  {
    uart0_puts("bring-up stopped at step ");
    uart0_putnum((uint32_t)(-rc));
    uart0_puts("\r\n");
  }
}

int main()
{
  uart0_init();
  uint64_t t_start = read_timer();
  spi1_init();
  sd_dummy_clocks();
  led_init();

  // SD: silent bring-up. if the card does not come up, run it again through
  // the talking version, so a failure is always visible and the debug cost
  // is only ever paid on failure.
  sd_status_t st;
  if (sd_bringup(&st) != 0)
  {
    sd_bringup_debug();
  }

  // sd_bringup_debug();     // <- uncomment this line while bringing the board up

  // MBR Parser block 0 is the partition table. the start LBA it hands back is already a block number,
  static uint8_t mbr[512];
  uint32_t part_start = 0;
  uint32_t part_size = 0;

  uart0_puts("MBR - reading block 0....\r\n");
  if (sd_read_block(0, mbr) != 0)
  {
    uart0_puts("MBR FAIL - could not read block 0\r\n");
  }
  else if (sd_mbr_parse(mbr, &part_start, &part_size) == 0)
  {
    uart0_puts("MBR Ok - filesystem starts at block ");
    uart0_putnum(part_start);
    uart0_puts("\r\n");
    uart0_puts("\r\n");
  }
  else
  {
    uart0_puts("MBR FAIL - no usable partition table\r\n");
  }

  // BPB block part_start is the VBR. its BPB gives the FAT32 geometry, which
  // every later read depends on. no point going on if this fails.
  static uint8_t vbr[512]; // static 512 byte block for volume boot record
  fat_geom_t geom;         // struct to store the main fields of the vbr/bios

  uart0_puts("BPB - reading block ");
  uart0_putnum(part_start);
  uart0_puts("....\r\n");
  if (sd_read_block(part_start, vbr) != 0)
  {
    uart0_puts("BPB FAIL - could not read the VBR\r\n");
  }
  else if (fat32_parse_bpb(vbr, part_start, &geom) == 0)
  {
    uart0_puts("BPB Ok - FAT32 geometry read\r\n");
    uart0_puts("\r\n");

    uart0_puts("Root dir - listing....\r\n");
    if (fat32_list_root(&geom) == 0)
    {
      uart0_puts("Root dir Ok\r\n");
      uart0_puts("\r\n");
      // read_prince(&geom); // only ever called with valid geometry
    }
    else
    {
      uart0_puts("Root dir FAIL\r\n");
    }
    // blast the image with the real geometry
    ili9341_init();
    // ili9341_fill_area(0, 0, 239, 319, 0xFFFF);
    // ili9341_draw_char(2, 2, 'A', 0x0000, 0xFFFF, 2);
    // ili9341_draw_string(4, 158, "PrinceJha%", 0x0000, 0xFFFF, 2);
    // draw_prince(&geom);

    /* ---- STEP 1: timed frame ---- */
    sd_time_us = 0;
    sd_read_count = 0;
    lcd_time_us = 0;
    lcd_bytes = 0;

    uint64_t t0 = read_timer();
    draw_prince(&geom);
    uint64_t wall = read_timer() - t0;

    uint32_t sd_us = (uint32_t)sd_time_us;
    uint32_t lcd_us = (uint32_t)lcd_time_us;

    uart0_puts("\r\nFRAME TIMING\r\n");
    uart0_puts("SD  : ");
    uart0_putnum(sd_us / 1000);
    uart0_puts(" ms  (");
    uart0_putnum(sd_us % 1000);
    uart0_puts(" us)  ");
    uart0_putnum(sd_read_count);
    uart0_puts(" block reads\r\n");
    uart0_puts("LCD : ");
    uart0_putnum(lcd_us / 1000);
    uart0_puts(" ms  (");
    uart0_putnum(lcd_us % 1000);
    uart0_puts(" us)  ");
    uart0_putnum(lcd_bytes);
    uart0_puts(" bytes\r\n");
    uart0_puts("TOTAL draw_prince : ");
    uart0_putnum((uint32_t)(wall / 1000));
    uart0_puts(" ms\r\n");
    uart0_puts("UART/other (total - SD - LCD) : ");
    uart0_putnum((uint32_t)((wall - sd_us - lcd_us) / 1000));
    uart0_puts(" ms\r\n");
    uart0_puts("boot to first pixel : ");
    uart0_putnum((uint32_t)((read_timer() - t_start) / 1000));
    uart0_puts(" ms\r\n");
    uart0_puts("\r\n");
  }
  else
  {
    uart0_puts("BPB FAIL - not a usable FAT32 boot sector\r\n");
  }

  while (1)
  {
    SIO_GPIO_OUT_XOR = GPIO25;
    delay_ms(500);
  }
}

//before
/*
MBR - reading block 0....
Part 0: Type 0x0C  FAT32 (LBA)
Start : 2048 Size : 62531584 Sectors / 30533 MiB
Chosen partition: Start block 2048 (size 30533 MiB)
FAT32 confirmed - its VBR is the sector above
MBR Ok - filesystem starts at block 2048

BPB - reading block 2048....
BPB: bytes/sector 512, Sectors/cluster 128 (65536 byte per clusters)
Reserved 558, FATs 2, Sectors/FAT 3817
Total sectors 62531584 (30533 MebiByte)
FAT starts at block 2606, data starts at block 10240
Clusters 488464, Root cluster 2
BPB Ok - FAT32 geometry read

Root dir - listing....
Root dir: cluster 2, first block 10240
[FILE] Prince.rgb  cluster 6  size 153600
Root dir done, 1 root entries
Root dir Ok

LCD: streaming Prince.rgb...
LCD: frame done, 153600 pixel-bytes

FRAME TIMING
SD  : 231 ms  (810 us)  302 block reads
LCD : 88 ms  (572 us)  153600 bytes
TOTAL draw_prince : 321 ms
UART/other (total - SD - LCD) : 0 ms
boot to first pixel : 951 ms
*/

//after
/*
MBR - reading block 0....
Part 0: Type 0x0C  FAT32 (LBA)
Start : 2048 Size : 62531584 Sectors / 30533 MiB
Chosen partition: Start block 2048 (size 30533 MiB)
FAT32 confirmed - its VBR is the sector above
MBR Ok - filesystem starts at block 2048

BPB - reading block 2048....
BPB: bytes/sector 512, Sectors/cluster 128 (65536 byte per clusters)
Reserved 558, FATs 2, Sectors/FAT 3817
Total sectors 62531584 (30533 MebiByte)
FAT starts at block 2606, data starts at block 10240
Clusters 488464, Root cluster 2
BPB Ok - FAT32 geometry read

Root dir - listing....
Root dir: cluster 2, first block 10240
[FILE] Prince.rgb  cluster 6  size 153600
Root dir done, 1 root entries
Root dir Ok

LCD: streaming Prince.rgb...
LCD: frame done, 153600 pixel-bytes

FRAME TIMING
SD  : 141 ms  (729 us)  302 block reads
LCD : 88 ms  (564 us)  153600 bytes
TOTAL draw_prince : 231 ms
UART/other (total - SD - LCD) : 0 ms
boot to first pixel : 861 ms */