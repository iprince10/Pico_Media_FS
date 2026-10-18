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

/*sd_block_sink_t is a name for any function that takes (block pointer, length, length, context pointer) and returns nothing.
" The void appears twice, meaning two different things:
-  the first is returns nothing;
- the void *ctx is a generic untyped pointer ( any pointer type can be passed)"
*/
typedef void (*sd_block_sink_t)(const uint8_t *block, uint32_t len, void *ctx);
uint8_t sd_read_multi_block(uint32_t first_block, uint32_t count, sd_block_sink_t sink, void*ctx);

#endif



/* 
 * SD SPI RESPONSE REFERENCE — read this before touching any response handling.
 * (Bit maps below follow the SD Physical Layer Simplified Spec, SPI mode.
 *  Where a field is marked "confirm", re-check it against the revision you
 *  target — the spec has renumbered data-token bits between versions.)
 *
 *
 * TRANSPORT RULES
 *   - Everything the card says arrives on MISO. MOSI must carry 0xFF while
 *     you clock, so the bus idles high and a stray 0 bit can never be
 *     mistaken for the start of a command.
 *   - 0xFF read from MISO NEVER means success. It means "no byte yet":
 *     card is idle-polling, mid-busy, or still shifting the reply out.
 *   - CS low for the whole transaction, CS high between transactions, and
 *     one extra 0xFF after raising CS to let the card finish its byte.
 *   - CRC is not checked in SPI mode by default, so 0xFF is a fine dummy
 *     CRC for most commands. CMD0 (0x95) and CMD8 (0x87) are the customary
 *     exceptions — cheap to get right, so keep them correct.
 *
 * RESPONSE TYPES ("NCR" = how long the card may stall before answering;
 * a bound you must respect or you will hang on a dead card)
 *   R1  1 byte. Always the first byte of every reply (except R2, below).
 *       Poll up to ~8 bytes for it. Compare against 0x00, never against a
 *       nonzero mask — bit 7 is always 0, so 0x80-style tests never fire.
 *         bit7 = 0 (always)
 *         bit6 = parameter error      bit3 = command CRC error
 *         bit5 = address error        bit2 = illegal command
 *         bit4 = erase sequence err   bit1 = erase reset
 *                                     bit0 = in idle state
 *       bit0 is the flag your init loop turns on: still 1 => not ready,
 *       keep re-issuing ACMD41.
 *   R1b R1 followed by one busy byte. Card holds MISO low (0x00) while
 *       busy and releases it (0xFF) when done. Only write/erase paths.
 *   R2  2 bytes, sent back-to-back with CS held low across the pair.
 *       R1 + one byte of card status (CMD13). Do NOT raise CS in between
 *       or you lose the second byte.
 *   R3  R1 + 4 OCR bytes. CMD58 (READ_OCR): CCS bit tells you SDHC (block
 *       addressed) vs SDSC (byte addressed). Also your pre-init busy check.
 *   R7  R1 + 4 bytes. CMD8 (SEND_IF_COND): voltage accepted + the 0xAA
 *       check pattern echoed back. Echo mismatch = dead or unusable card.
 *
 * DATA TOKENS — READ PATH (what lands after a successful read command)
 *   0xFE          start of a 512-byte block. Anything else is not data.
 *   0x0X, bit0=0  data error token (ECC / CC / out-of-range — confirm bits
 *                 for your revision). Treat as fatal, abort the transfer.
 *     You may see references to a stop/abort token around 0xFD. Do not
 *     build logic on it. CMD12 is your only reliable stop; end the stream
 *     yourself rather than waiting for the card to announce it.
 *   Bounded wait: a block read can legitimately take ~100 ms, so allow a
 *   few hundred ms of 0xFF polling before declaring the card dead.
 *
 * DATA TOKENS — WRITE PATH (not used here, listed so the file is complete)
 *   Data response token after each block: mask off bits 7-5 (don't care),
 *   then bit4 must be 0, and the low 5 bits read:
 *     0x05 = accepted   0x0B = CRC error   0x0D = write error
 *   Followed by a busy period: MISO low until the card has programmed it.
 *
 * MULTI-BLOCK READ (CMD18) LIFECYCLE — the one that needs care
 *   CMD18 takes the FIRST block's LBA only; the rest are implied. Sequence:
 *     R1 == 0x00  ->  0xFE + 512 bytes + 2 CRC, repeated.
 *   The stream has no self-terminating token, so YOU must CMD12 when the
 *   count is satisfied. Two traps: (a) the card may be part-way through a
 *   block when CMD12 lands — expect one more token + 512 + 2 CRC and
 *   discard it; (b) CMD12's own R1 may not be ready immediately. Deselect,
 *   clock the trailing 0xFF, and count blocks yourself rather than
 *   trusting the stream to stop where you asked.
 *
 * FAILURE HANDLING — one rule covers nearly everything
 *   On any R1 != 0x00, or a token that is neither 0xFE nor a success
 *   marker: abort. Raise CS, give one trailing 0xFF (let the card finish
 *   its byte), return an error. A half-consumed response misaligns every
 *   later byte in the transaction, and the card will keep streaming into
 *   your next command. There is no recovery short of a fresh CMD0.
 * 
 */