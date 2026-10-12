#include <stdint.h>
#include "timer.h"
#include "sdcard.h"
#include "uart.h"
#include "fat32.h"
#include "ili9341.h"

// little endian: lowest offset holds the least significant byte. this is the
// reverse of what sd_send_command puts on the wire
static uint32_t mbr_read_le32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

// pass the type byte here and it returns the format type of the card
static void mbr_show_type(uint8_t type)
{
    switch (type)
    {
    case 0x01:
        uart0_puts("FAT12");
        break;
    case 0x04:
        uart0_puts("FAT16 <32M");
        break;
    case 0x06:
        uart0_puts("FAT16");
        break;
    case 0x0B:
        uart0_puts("FAT32 (CHS)");
        break;
    case 0x0C:
        uart0_puts("FAT32 (LBA)");
        break;
    case 0x0E:
        uart0_puts("FAT16 (LBA)");
        break;
    case 0x07:
        uart0_puts("exFAT / NTFS");
        break;
    case 0x83:
        uart0_puts("Linux");
        break;
    case 0xEF:
        uart0_puts("EFI system");
        break;

    default:
        uart0_puts("unknown");
        break;
    }
}

// MBR partition table
// Block 0 is the Master Boot Record. Fixed layout, independent of any
// filesystem:
/*
Block 0 is the Master Boot Record (MBR) — the one sector on the disk
with fixed layout by convention independent of any filesystem:
Its structure:
| OFFSET  | SIZE  | CONTENTS                                      |
| ------- | ----- | ----------------------------------------------|
| 0–445   | 446 B | bootstrap code — leftover from the DOS era,   |
|         |       | ignore it                                     |
| 446–461 | 16 B  | partition entry 1                             |
| 462–477 | 16 B  | partition entry 2                             |
| 478–493 | 16 B  | partition entry 3                             |
| 494–509 | 16 B  | partition entry 4                             |
| 510–511 | 2 B   | signature 0x55 0xAA — the two bytes being     |
|         |       | checked                                       |
------------------------------------------------------------------
446 + 4 × 16 = 510
So the four partition entries tile exactly up to the signature.
The MBR is a tiny index: it describes where partitions live, not what's inside them. It is filesystem-agnostic — an MBR can point at FAT32, exFAT,NTFS, Linux ext4, anything.
Within each partition entry, fixed offsets apply:
| Offset in entry | Size | Meaning                                      |
| --------------- | ---- | -------------------------------------------- |
| +0              | 1 B  | boot flag: 0x80 = bootable, 0x00 = not      |
| +1 … +3         | 3 B  | starting CHS address — obsolete              |
| +4              | 1 B  | partition type — what you want #1            |
| +5 … +7         | 3 B  | ending CHS address — obsolete                |
| +8 … +11        | 4 B  | LBA of first sector — what you want #2       |
| +12 … +15       | 4 B  | sector count — what you want #3              |
The three important fields are:
#1 Partition type, #2 LBA of first sector, #3 Sector count
// empty slots have type 0x00. there is no rule the partition sits in
// slot 0, so all four get scanned.
*/
#define SD_MBR_PART_OFFSET 446
#define SD_MBR_PART_SIZE 16
#define SD_MBR_PART_COUNT 4
#define SD_MBR_SIG_OFFSET 510

#define SD_PART_EMPTY 0x00
#define SD_PART_EXFAT 0x07
#define SD_PART_FAT32_CHS 0x0B
#define SD_PART_FAT32_LBA 0x0C

// parse the MBR in mbr[512]. prints every non-empty entry found.
// on success fills *part_start with the first usable start LBA (this is
// already a block number, SDHC does the byte->block math) and
// *part_size with its size in sectors. returns 0 on success, 0xFF if the
// signature or the table is no good.
uint8_t sd_mbr_parse(uint8_t *mbr, uint32_t *part_start, uint32_t *part_size)
{
    uint8_t chosen_type = 0;
    uint8_t found = 0;

    if (mbr[SD_MBR_SIG_OFFSET] != 0x55 || mbr[SD_MBR_SIG_OFFSET + 1] != 0xAA) // mbr eof signature check 0x55 & 0xAA
    {
        uart0_puts("MBR: no 0x55AA signature, not a partition table\r\n");
        return 0xFF;
    }
    for (int i = 0; i < SD_MBR_PART_COUNT; i++)
    {
        // entry i sits 16 bytes further along each time
        uint8_t *entry = mbr + SD_MBR_PART_OFFSET + (i * SD_MBR_PART_SIZE); // entry is a pointer to the memory location inside the mbr block
        uint8_t type = entry[4];                                            // 4 bytes forward from the address stored in entry
        uint32_t start = mbr_read_le32(entry + 8);
        uint32_t size = mbr_read_le32(entry + 12);

        if (type == SD_PART_EMPTY || size == 0)
        {
            continue;
        }

        uart0_puts("Part ");
        uart0_putnum(i);
        uart0_puts(": Type 0x");
        uart0_puthex(type);
        uart0_puts(" ");
        mbr_show_type(type);
        uart0_puts("\r\nStart : ");
        uart0_putnum(start);
        uart0_puts(" Size : ");
        uart0_putnum(size);
        uart0_puts(" Sectors / ");
        // divide first: size * 512 overflows 32 bits on a 32 GB card.
        // 1 MB = 2048 sectors of 512, so size/2048 is MB directly.
        uart0_putnum(size / 2048u);
        uart0_puts(" MiB\r\n");

        if (!found) // first usable entry wins, we have not looked for more
        {
            *part_start = start; // partiiton start i.e.
            *part_size = size;
            chosen_type = type;
            found = 1;
        }
    }
    if (!found)
    {
        uart0_puts("MBR: table valid but no usable partition entry\r\n");
        return 0xFF;
    }

    uart0_puts("Chosen partition: Start block ");
    uart0_putnum(*part_start);
    uart0_puts(" (size ");
    uart0_putnum(*part_size / 2048u);
    uart0_puts(" MiB)\r\n");

    if (chosen_type == SD_PART_EXFAT)
    {
        uart0_puts("WARNING: type 0x07 is exFAT, not FAT32.\r\n");
        uart0_puts("FAT32 plan does not apply - reformat or change target.\r\n");
    }
    else if (chosen_type == SD_PART_FAT32_LBA || chosen_type == SD_PART_FAT32_CHS)
    {
        uart0_puts("FAT32 confirmed - its VBR is the sector above\r\n");
    }
    else
    {
        uart0_puts("not FAT32 - check the type above before going further\r\n");
    }

    return 0;
}

// BPB (BIOS Parameter Block)
// Block 2048  is the VBR ,the filesystem's own block 0
// The VBR sits at part_start (MBR gave 2048). Its first bytes are the BPB , all little Endian same as MBR entries
// offsets from the top of the VBR sector :
// 11  bytes per sector    (2) usually 512
// 13  sectors per cluster (1) power of 2 usually 64
// 14  reserved sectors    (2) sectors before FAT 1
// 16  number of FATs      (1) usually 2
// 17  root entry count    (2) 0 on FAT32
// 22  FAT size 16         (2) 0 on FAT 32
// 32  total sectors 32    (4)  sectors in the volume
// 36  FAT size 32         (4)  sectors per FAT  <- the real one
// 44  root cluster        (4)  usually 2
// 510 signature           (2)  0x55 0xAAF

#define FAT_BPB_BYTES_PER_SECT 11
#define FAT_BPB_SEC_PER_CLUS 13
#define FAT_BPB_RSVD_SEC 14
#define FAT_BPB_NUM_FATS 16
#define FAT_BPB_ROOT_ENT 17
#define FAT_BPB_FATSZ16 22
#define FAT_BPB_TOTSEC32 32
#define FAT_BPB_FATSZ32 36
#define FAT_BPB_ROOT_CLUS 44
#define FAT_BPB_SIG 510

// two-byte little endian read
static uint16_t bpb_read_le16(const uint8_t *b)
{
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

// parse the BPB in bpb[512] , the VBR read from block part_start.
// fills *g with the raw fields and the derived absolute block addresses
// return 0 on success , 0xFF if this is not a usable FAT32 boot sector
uint8_t fat32_parse_bpb(uint8_t *bpb, uint32_t part_start, fat_geom_t *g)
{
    // signature check first
    if (bpb[FAT_BPB_SIG] != 0x55 || bpb[FAT_BPB_SIG + 1] != 0xAA)
    {
        uart0_puts("BPB : no 0x55AA signature , not a boot sector\r\n");
        return 0xFF;
    }

    // raw fields put the values into the struct
    g->part_start = part_start;
    g->bytes_per_sector = bpb_read_le16(bpb + FAT_BPB_BYTES_PER_SECT);
    g->sectors_per_cluster = bpb[FAT_BPB_SEC_PER_CLUS];
    g->reserved_sectors = bpb_read_le16(bpb + FAT_BPB_RSVD_SEC);
    g->num_fats = bpb[FAT_BPB_NUM_FATS];
    g->total_sectors = mbr_read_le32(bpb + FAT_BPB_TOTSEC32);
    g->sectors_per_fat = mbr_read_le32(bpb + FAT_BPB_FATSZ32);
    g->root_cluster = mbr_read_le32(bpb + FAT_BPB_ROOT_CLUS);
    // FAT32 marks itself by leaving FATSz16 and RootEntCnt zero. either one
    // nonzero means this is really FAT12/16 and the layout below is wrong.
    uint16_t fatsz16 = bpb_read_le16(bpb + FAT_BPB_FATSZ16);
    uint16_t rootent = bpb_read_le16(bpb + FAT_BPB_ROOT_ENT);

    if (fatsz16 != 0 || rootent != 0)
    {
        uart0_puts("BPB: FATSz16/RootEntCnt nonzero - this is FAT12/16\r\n");
        return 0xFF;
    }

    // sectors per cluster must be power of two; 0 means read junk
    if (g->sectors_per_cluster == 0 || (g->sectors_per_cluster & (g->sectors_per_cluster - 1)) != 0)
    {
        uart0_puts("BPB: sectors per cluster not a power of two\r\n");
        return 0xFF;
    }

    // derived addresses - add part_start to turn the BPB's partition relative
    // offsets into absolute blocks for sd_read_block()
    // FATs come first, right after the reserved area
    g->fat_start = g->part_start + g->reserved_sectors;
    // then the data region, right after every FAT copy
    g->data_start = g->part_start + g->reserved_sectors + (uint32_t)g->num_fats * g->sectors_per_fat;

    // how many clusters fit in the data region. clusters are counted from 2,
    // but the count is just data sectors / sectors per cluster
    uint32_t overhead = g->reserved_sectors + (uint32_t)g->num_fats * g->sectors_per_fat;
    uint32_t data_sectors = g->total_sectors - overhead;
    g->cluster_count = data_sectors / g->sectors_per_cluster;

    // ---- report ----
    uart0_puts("BPB: bytes/sector ");
    uart0_putnum(g->bytes_per_sector);
    uart0_puts(", Sectors/cluster ");
    uart0_putnum(g->sectors_per_cluster);
    uart0_puts(" (");
    uart0_putnum((uint32_t)g->sectors_per_cluster * g->bytes_per_sector);
    uart0_puts(" byte per clusters)\r\n");

    uart0_puts("Reserved ");
    uart0_putnum(g->reserved_sectors);
    uart0_puts(", FATs ");
    uart0_putnum(g->num_fats);
    uart0_puts(", Sectors/FAT ");
    uart0_putnum(g->sectors_per_fat);
    uart0_puts("\r\n");

    uart0_puts("Total sectors ");
    uart0_putnum(g->total_sectors);
    uart0_puts(" (");
    uart0_putnum(g->total_sectors / 2048u); // divide first, same reason as MBR
    // divide by 2048 because a sectors contains 512 bytes and one Kib contains 1024 bytes or 2 sectors
    // and 1 Mib contains 1024 Kib , thus for Mebibytes, divide the sectors by 2048
    uart0_puts(" MebiByte)\r\n");

    uart0_puts("FAT starts at block ");
    uart0_putnum(g->fat_start);
    uart0_puts(", data starts at block ");
    uart0_putnum(g->data_start);
    uart0_puts("\r\n");

    uart0_puts("Clusters ");
    uart0_putnum(g->cluster_count);
    uart0_puts(", Root cluster ");
    uart0_putnum(g->root_cluster);
    uart0_puts("\r\n");

    // the whole block model leans on 512 byte sectors. warn if it is not.
    if (g->bytes_per_sector != 512)
    {
        uart0_puts("WARNING: bytes/sector is not 512, block model breaks\r\n");
    }

    // must fit under the 28 bit FAT32 entry ceiling
    if (g->cluster_count >= 268435456u)
    {
        uart0_puts("WARNING: cluster count over 2^28, not valid FAT32\r\n");
    }
    return 0;
    // BPB - reading block 2048....
    // BPB: bytes/sector 512, sectors/cluster 64 (32768 byte clusters)
    // reserved 1120, FATs 2, sectors/FAT 7632
    // total sectors 62531584 (30533 MiB)
    // FAT starts at block 3168, data starts at block 18432
    // clusters 976800, root cluster 2
    // BPB Ok - FAT32 geometry read
}

// ROOT DIRECTORY WALKER
// On FAT32 the root directory is not a fixed region (that was FAT12/16). It is an
// ordinary cluster chain that starts at geom->root_cluster (2 on every card) and
// like a file , its content is a flat run of fixed 32 byte records packed back to back - no headers, no separators
// Record n begins at byte n * 32 within the cluster
// Each 32 byte record , offsets from the start of the record :
//   0   name          (8)  space padded, uppercase, dot not stored
//   8   extension     (3)  space padded
//   11  attribute     (1)  bit field, tells what this entry is
//   12  reserved      (1)  NT case flags, ignored here
//   13  ctime tenths  (1)  ignored
//   14  ctime         (2)  ignored
//   16  cdate         (2)  ignored
//   18  adate         (2)  ignored
//   20  cluster high  (2)  first cluster, bits 16..31
//   22  wtime         (2)  ignored
//   24  wdate         (2)  ignored
//   26  cluster low   (2)  first cluster, bits 0..15
//   28  size          (4)  file size in bytes
// Byte 0 is the first read because it can override everthing else:
//   0x00  end of directory  - all records from here on are unused, stop
//   0xE5  deleted entry     - leftover, skip it
//   0x05  a real name whose true first byte is 0xE5, stored this way so it is   not mistaken for a deleted entry.
// Attribute byte 11 , one bit per property :
//   0x01 read only
//   0x02 hidden
//   0x04 system
//   0x08 volume label
//   0x10 directory
//   0x20 archive
//   0x0F LFN    : all four low bits set = a long filename piece, not a real entry
// the first cluster is split across two fields six bytes apart. FAT12/16 had
// a 16 bit cluster at offset 26; FAT32 added a high half at offset 20 and calls
// the real number high:low. only 28 bits are used, top 4 reserved.
#define FAT_DIR_ENT_SIZE 32                        //  each entry is 32 bytes
#define FAT_DIR_PER_BLOCK (512 / FAT_DIR_ENT_SIZE) // 16 records in one block
#define FAT_DIR_NAME 0                             // 0 offset for the name
#define FAT_DIR_ATTR 11                            // bit offset tells whether file or directory
#define FAT_DIR_CLUS_HI 20                         // cluster high 16 bits
#define FAT_DIR_CLUS_LO 26                         // cluster high 16 bits
#define FAT_DIR_SIZE 28                            // size in bytes 4 bytes , file max size is 4 gb
#define FAT_DIR_END 0x00                           // End of chain marker
#define FAT_DIR_DELETED 0xE5                       // delete entry marker
#define FAT_ATTR_DIR 0x10                          // sets the bit that when adn with fat_dir_attr tells whether file or directory
#define FAT_ATTR_READONLY 0x01                     // file is write-protected
#define FAT_ATTR_HIDDEN 0x02                       // file not shown in normal listng or file is marked hidden or system/window file
#define FAT_ATTR_SYSTEM 0x04                       // this is a os internal file/folder
#define FAT_ATTR_LFN 0x0F                          // four low bits set , value of attribute byte when the file is long file name
// a 0x0F record carries 13 UTF-16 characters in three runs, and the records are
// stored last chunk first:
//   bytes 1..10  -> 5 chars
//   bytes 14..25 -> 6 chars
//   bytes 28..31 -> 2 chars
// byte 0 is the chunk number (1 based), OR 0x40 on the chunk that holds the
// start of the name. byte 13 is a checksum of the 8.3 name that lets us prove
// the pieces belong to this file.
#define FAT_LFN_CHARS 13
#define FAT_LFN_SEQ 0
#define FAT_LFN_CHECK 13
#define FAT_LFN_MAX 260
#define FAT_LFN_CHUNKS (FAT_LFN_MAX / FAT_LFN_CHARS)
#define FAT_MAX_DEPTH 16

// first block of a data cluster. clusters are numbered from 2, so cluster 2 is
// simply the first data cluster and identity N maps to N-2:
//   block = data_start + (N - 2) * sectors_per_cluster
// every read in the filesystem hangs off this one line.
static uint32_t fat32_cluster_to_block(const fat_geom_t *g, uint32_t clus)
{
    return g->data_start + (clus - 2u) * g->sectors_per_cluster;
    // gives the starting block number of the cluster
    // cluster 2 = 18432 + 0*64 = 18432 , for cluster 4 it is 18432+2*64 = 18560
}

// read cluster clus's slot in FAT #1 and return the next cluster of its chain.
// one 4 byte slot per cluster, indexed by the cluster number itself, so the slot
// lives at byte clus*4 within the FAT. 512 is a multiple of 4 so a slot never
// straddles two blocks. only the low 28 bits are real, mask the top 4.
// 0x0FFFFFF8..0x0FFFFFFF is end of chain, returned unchanged so the caller stops.
static uint32_t fat32_next_cluster(const fat_geom_t *g, uint32_t clus, uint8_t *scratch) // scratch is the 512 byte buffer , clus is the cluster number , scratch is the 512 byte of fat_buf
{
    uint32_t entry = clus * 4u; // byte offset inside the FAT
    // entry gives the total bytes offset of the passed cluster
    uint32_t blk = g->fat_start + (entry / 512u); // which FAT block holds it
    // blk contains the block number which would hold the entry based on the entry byte offset
    uint32_t off = entry % 512u; // where inside that block
    // off contains the actual offset inside a 512 byte block.
    // diff between the entry and off is that entry contains the total byte based on cluste N * 4
    // and off contains the entry % 512 which would be in between 0 and 511
    if (sd_read_block(blk, scratch) != 0) // blocking reading blk is block number and scratch is 512 byte buffer
    {
        return 0x0FFFFFFFu; // a bad read acts like end of chain
    }
    return mbr_read_le32(scratch + off) & 0x0FFFFFFFu;
    // scratch is 512 byte buffer , off is the byte's offset
    // scratch + off is pointer arithematic points at the scratch + off byte in 512 byte buffer
    // and mbr read le 32 return the 32 bit number assembled in little endian format , LSB in lowest memory slot
    // Little Endian e.g. 0x12345678 in little Endian is  78 56 34 12
}

// pull up to 13 characters out of one 0x0F record into out[13]. each field is
// UTF-16 little endian, so take the low byte; a 0x0000 ends the name early and
// 0xFFFF is padding. the tail of out[] is zeroed so concatenation is clean.
/*
Long Filename (LFN) Directory Entry Layout
-----------------------------------------------------------------------------------------------
| Offset | Size | Field                  | What it holds                                      |
| ------ | ---- | ---------------------- | -------------------------------------------------- |
| 0      | 1    | Order                  | Bits 0–5 = sequence number (1, 2, …);              |
|        |      |                        | bit 6 (0x40) = last fragment; bit 7 = deleted flag |
| 1–10   | 10   | Name1                  | Characters 1–5 (5 × UTF-16)                        |
| 11     | 1    | Attribute              | Always 0x0F — the tell-tale sign of a fragment     |
| 12     | 1    | Type                   | Always 0x00 (reserved)                             |
| 13     | 1    | Checksum               | The 1-byte checksum of the short name              |
| 14–25  | 12   | Name2                  | Characters 6–11 (6 × UTF-16)                       |
| 26–27  | 2    | First cluster (low)    | Always 0x0000 — a real file would have data here   |
| 28–31  | 4    | Name3                  | Characters 12–13 (2 × UTF-16)                      |
-----------------------------------------------------------------------------------------------

When root directory has a sub directory , the sub directory contains the . its own cluster and .. parent root cluster in them
*/
static int fat32_lfn_chunk(const uint8_t *e, char *out)
{
    static const uint8_t pos[FAT_LFN_CHARS] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    // array of the 13 bytes in the fragmant that stores the characters
    // 1-10 then 14-24, then 28-30, each char is two bytes.
    int n = 0;
    for (int i = 0; i < FAT_LFN_CHARS; i++)
    {
        uint16_t c = (uint16_t)e[pos[i]] | ((uint16_t)e[pos[i] + 1] << 8);
        // e is the pointer to the 32 byte record being processed pointing to the first byte of it
        // and pos is the 13 byte array , together they construct the character here from that offset
        // e[pos[i]] low byte of the char and + 1 is the high byte of the char
        if (c == 0x0000 || c == 0xFFFF) // 0x0000 is the real end of the name and 0xFFFF is the padding
        {
            break;
        }
        out[n] = (char)(c & 0xFF); // & with 0xFF keeps only the low byte  out[n] stores the long name
        n++;
    }
    for (int i = n; i < FAT_LFN_CHARS; i++) // zero fill the rest of the 13 byte slot, if it had fewer than 13 characters
    {
        out[i] = 0;
    }
    return n; // n is number of characters actually stored
}

// fold the 11 raw bytes of an 8.3 name into one byte. every 0x0F record stores
// this value at offset 13; recomputing it and comparing proves a rebuilt long
// name really belongs to the short entry that follows.

/*
Use `TEST    TXT` (the 8.3 for a file called `TEST.TXT`).
Its 11 raw bytes:
T=0x54, E=0x45, S=0x53, T=0x54, sp=0x20, sp=0x20, sp=0x20, sp=0x20, T=0x54, X=0x58, T=0x54

Starting sum = 0x00, feeding each byte through the line:

| #  | byte   | sum before       | bit 0 | ternary | sum >> 1 | + byte          | sum after |
| -- | ------ | ---------------- | ----- | ------- | -------- | --------------  | --------- |
| 1  | 0x54   | 0x00             | 0     | 0       | 0x00     | +0x54           | 0x54      |
| 2  | 0x45   | 0x54 (0101 0100) | 0     | 0       | 0x2A     | +0x45           | 0x6F      |
| 3  | 0x53   | 0x6F (0110 1111) | 1     | 0x80    | 0x37     | -> 0x10A        | 0x0A      |
| 4  | 0x54   | 0x0A (0000 1010) | 0     | 0       | 0x05     | +0x54           | 0x59      |
| 5  | 0x20   | 0x59 (0101 1001) | 1     | 0x80    | 0x2C     | -> 0xCC         | 0xCC      |
| 6  | 0x20   | 0xCC (1100 1100) | 0     | 0       | 0x66     | +0x20           | 0x86      |
| 7  | 0x20   | 0x86 (1000 0110) | 0     | 0       | 0x43     | +0x20           | 0x63      |
| 8  | 0x20   | 0x63 (0110 0011) | 1     | 0x80    | 0x31     | -> 0xD1         | 0xD1      |
| 9  | 0x54   | 0xD1 (1101 0001) | 1     | 0x80    | 0x68     | -> 0x13C -> 0x3C| 0x3C      |
| 10 | 0x58   | 0x3C (0011 1100) | 0     | 0       | 0x1E     | +0x58           | 0x76      |
| 11 | 0x54   | 0x76 (0111 0110) | 0     | 0       | 0x3B     | +0x54           | 0x8F      |
---------------------------------------------------------------------------------------------
Final checksum: 0x8F.
*/
static uint8_t fat32_lfn_checksum(const uint8_t *name83)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
    {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name83[i]);
        // three operations here :
        // sum & 1 masks off all the bit except the right most one
        // ((sum & 1) ? 0x80 : 0) ternary operator , 0x80 when the condition is 1 or true
        // then sum>>1 right shift by 1 , the bit falls off
        // add all three and loop is the running checksum
        // we start from sum 0 then add the current name , we get the updated sum , then rotate shift the sum based on the and condition
        // then right shift by 1 to get the rotate shift bit in the leftmost bit, then add the current name byte to the sum.
        // add the three here in the running sum to get the final checksum
    }
    return sum;
}

// printing the name function
static void fat32_show_name83(const uint8_t *raw)
{
    // first 0 - 7 bytes of the name of the file , skip printing the char if it 0 , no char is filled with " " space
    for (int i = 0; i < 8; i++)
    {
        if (raw[i] == ' ')
        {
            break;
        }
        uart0_putc((char)raw[i]); // prints the character
    }
    // the next extension 8-10 bytes, skip if the 8th byte is " " space means no extension at all
    if (raw[8] != ' ')
    {
        uart0_putc('.'); // manually put a dot
        for (int i = 8; i < 11; i++)
        {
            if (raw[i] == ' ')
            {
                break;
            }
            uart0_putc((char)raw[i]); // printing the extension
        }
    }
}

static void fat32_indent(int depth) // add the space in the sub directories , 1 depth = 1 space, indent means structuring the folder and files
{
    for (int i = 0; i < depth; i++)
    {
        uart0_puts(" ");
    }
}

// #define FAT_DIR_ENT_SIZE 32                        //  each entry is 32 bytes
// #define FAT_DIR_PER_BLOCK (512 / FAT_DIR_ENT_SIZE) // 16 records in one block
// #define FAT_DIR_NAME 0                             // 0 offset for the name
// #define FAT_DIR_ATTR 11                            // bit offset tells whether file or directory
// #define FAT_DIR_CLUS_HI 20                         // cluster high 16 bits
// #define FAT_DIR_CLUS_LO 26                         // cluster high 16 bits
// #define FAT_DIR_SIZE 28                            // size in bytes 4 bytes , file max size is 4 gb
// #define FAT_DIR_END 0x00                           // End of chain marker
// #define FAT_DIR_DELETED 0xE5                       // delete entry marker
// #define FAT_ATTR_DIR 0x10                          // sets the bit that when adn with fat_dir_attr tells whether file or directory
// #define FAT_ATTR_LFN 0x0F                          // value of attribute byte when the file is long file name
// #define FAT_ATTR_READONLY 0x01                     // file is write-protected
// #define FAT_ATTR_HIDDEN 0x02                       // file not shown in normal listng or file is marked hidden or system/window file
// #define FAT_ATTR_SYSTEM 0x04                       // this is a os internal file/folder
// #define FAT_ATTR_LFN 0x0F                          // four low bits set , value of attribute byte when the file is long file name

// #define FAT_LFN_CHARS 13
// #define FAT_LFN_SEQ 0
// #define FAT_LFN_CHECK 13   // this is the checksum in the LFN Fragment
// #define FAT_LFN_MAX 260
// #define FAT_LFN_CHUNKS (FAT_LFN_MAX / FAT_LFN_CHARS)
// #define FAT_MAX_DEPTH 16
// walk the root directory from geom->root_cluster and print every live entry.
// reads the cluster one block at a time, decodes each 32 byte record, rebuilds
// long names, and follows the FAT chain when a cluster runs out before a 0x00
// record ends the directory.
// returns 0 on success, 0xFF if a needed block could not be read.

static int fat32_list_dir(const fat_geom_t *g, uint32_t start, int depth) // this lists the whole directory with their sub directories as well directly whatever it contains whether the short names or if the file is of long names then it displays the long name, //g is the struct containing the VBR info, start is the cluster number now and depth is how many sub directories deep we go in
{
    static uint8_t dir_buf[512];           // one block of directory records , holds the directory block bytes
    static uint8_t fat_buf[512];           // scratch for FAT slot reads , buffer for fat block bytes reads
    static char lfn_name[FAT_LFN_MAX + 1]; // long name being rebuilt , array for storing a long file name , total length for this is 261
    uint8_t lfn_check = 0;                 // checksum the pieces must match , checksum valid variable
    int lfn_seen = 0;                      // how many 0x0F pieces are pending, whether we have seen a lfn fragment record
    // uint32_t clus = g->root_cluster;    // the inital root cluster which is 2
    uint32_t clus = start; // the cluster passed in the function now
    int total = 0;         // stores total entries seen

    // the code below is nested three deep while->for->for and also shows the sub directories in it with their files
    // a break only happens when end of directory record is found 0x00
    // this break only escapes one loop, but we have to unwind al the three loops when eod is found so we set stop 1 and check it each loop iteration
    while (1)
    {
        int stop = 0; // stop is set when EOD is found , it unwinds from all three loops, then exits from the stop condition below
        // one cluster is sectors_per_cluster blocks. the root is usually a
        // single 32 KiB cluster but the same loop handles any size.
        // sectors per cluster is 64 but end with stop set unwinds the loop
        for (uint32_t s = 0; s < g->sectors_per_cluster && !stop; s++)
        {
            uint32_t blk = fat32_cluster_to_block(g, clus) + s; // for a n cluster find the block number then add s cause a cluster contains 64 sectors/block,e.g. cluster 2 = 18432 + 0*64 = 18432
            if (sd_read_block(blk, dir_buf) != 0)               // read the block in the 512 dir_buf buffer, it returns 0 when the read is successful
            {
                uart0_puts("DIR: read failed at block ");
                uart0_putnum(blk);
                uart0_puts("\r\n");
                return -1;
            }

            // 16 records to a 512 bytes block each record is of 32 bytes
            for (int r = 0; r < FAT_DIR_PER_BLOCK; r++) // fat directory per block is 16 as one directory is 32 bytes and total bytes in block is 512
            {
                uint8_t *e = dir_buf + (r * FAT_DIR_ENT_SIZE); // e is a pointer that points to memory in the buffer
                // 512 byte buffer then r is the iterator and fatdirentsize is 32

                // end of directory, everything after is unused
                if (e[FAT_DIR_NAME] == FAT_DIR_END) // fatdirend is 0x00
                {
                    stop = 1; // set stop 1 and unwinds all three loops
                    break;
                }
                // deleted leftover, and it kills any pending long name
                if (e[FAT_DIR_NAME] == FAT_DIR_DELETED) // deleted is 0xE5
                {
                    lfn_seen = 0; // sets the seen to 0 if it was change due to stale lfn fragments
                    continue;     // skips the current record only
                }
                // long name piece, stash it and move on
                if (e[FAT_DIR_ATTR] == FAT_ATTR_LFN) // fat_attr_LFN is 0x0F
                {
                    uint8_t seq = e[FAT_LFN_SEQ] & 0x3Fu; // chunk/order number, 1 based , this is the order number of the lfn file fragment stored in 0-6 bits, last fragment is stored first
                    // seq is the order number stored in the specific lfn fragment records compute it out
                    if (seq >= 1 && seq <= FAT_LFN_CHUNKS) // chunks is the order boundary 260 / 13 = 20 fatlfnchunks is 260/13 (max file name length divide by number of chars in one fragment)
                    {
                        // chunks arrive last first, so order by seq, not arrival
                        fat32_lfn_chunk(e, lfn_name + (seq - 1) * FAT_LFN_CHARS); // stores the name in the lfn name at appropriate order
                        if (e[FAT_LFN_SEQ] & 0x40)                                // checks whether it is the last fragment or not
                        {
                            // store the checksum here
                            lfn_check = e[FAT_LFN_CHECK]; // the start chunk carries it
                        }
                        if (seq > lfn_seen)
                        {
                            // update the lfn seen with the order number
                            lfn_seen = seq;
                        }
                    }
                    continue; // skip the iteration if not valid order number
                }

                // ---- a real entry ----
                // rebuild the first cluster from its two halves and drop the
                // reserved top 4 bits
                uint32_t hi = bpb_read_le16(e + FAT_DIR_CLUS_HI);      // high 2 bytes or 16 bits of cluster byte
                uint32_t lo = bpb_read_le16(e + FAT_DIR_CLUS_LO);      // low 2 bytes or 16 bits of cluster byte
                uint32_t first = ((hi << 16) | lo) & 0x0FFFFFFFu;      // combine the total 32 bits and or the top 4 reserved bits use the 28 bits
                uint32_t size = mbr_read_le32(e + FAT_DIR_SIZE);       // get the size of the file stored in 4 bytes max 4 gb
                int is_dir = (e[FAT_DIR_ATTR] & FAT_ATTR_DIR) ? 1 : 0; // checks whether file or directory
                // "." and ".." are directory records too: print neither and
                // descend into neither, or the walk would never end
                // subdirectores each 32 bytes long have first byte of the name as . containing their own cluster number and then .. in the second byte containing the cluster number of their respective root directory
                int is_dot = (e[0] == '.') && ((e[1] == ' ') || (e[1] == '.' && e[2] == ' '));
                lfn_name[lfn_seen * FAT_LFN_CHARS] = 0; // safe terminator
                // comment out this is_hidden block if want to show the windows / system files
                // Windows keeps its own clutter (System Volume Information and friends) marked with the hidden and system bits.
                // honour that flag and the whole subtree drops out on its own, since a hidden directory is never descended into. no name hardcoding needed.
                int is_hidden = (e[FAT_DIR_ATTR] & (FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM)) ? 1 : 0;
                if (is_hidden)
                {
                    lfn_seen = 0; // this entry ate its fragments, drop them
                    continue;     // skip this iteration
                }
                // print the name of the file whether lfn or 8.3 only when valid file skip the . and ..
                if (!is_dot)
                {
                    total++;             // total entries count incremented
                    fat32_indent(depth); // add the respective space of the subdirectory
                    uart0_puts(is_dir ? "[DIR]  " : "[FILE] ");
                    // prefer the long name if the checksum ties it to this entry
                    if (lfn_seen > 0 && fat32_lfn_checksum(e) == lfn_check) // if a lfn fragment has been seen and checksum is correct then print the long name
                    {
                        uart0_puts(lfn_name);
                    }
                    else
                    {
                        fat32_show_name83(e); // else show the 8.3 name
                    }
                    uart0_puts("  cluster ");
                    uart0_putnum(first);
                    if (!is_dir)
                    {
                        uart0_puts("  size ");
                        uart0_putnum(size);
                    }
                    uart0_puts("\r\n");
                }

                lfn_seen = 0; // reset for the next file
                // descend into a real subdirectory
                if (is_dir && !is_dot && first >= 2 && depth < FAT_MAX_DEPTH) // if it a sub directory under root ,deep dive into it ignore the directory named as ' . ' or ' .. ' using the !is_dot condition
                {
                    if (fat32_list_dir(g, first, depth + 1) < 0)
                    {
                        return -1;
                    }
                    // the call above reused dir_buf, so read this block back
                    // before carrying on with the next record
                    if (sd_read_block(blk, dir_buf) != 0)
                    {
                        return -1;
                    }
                }
            }
        }

        if (stop) // unwinds the loop if stop set
        {
            break;
        }
        /*
        The end-of-chain test, bit by bit
        What the function can return, per the FAT32 spec:
        | Slot value                 | Meaning                          |
        | -------------------------- | -------------------------------- |
        | 0x00000000                 | free cluster                     |
        | 0x00000001                 | reserved                         |
        | 0x00000002–0x0FFFFFEF      | a real next cluster number       |
        | 0x0FFFFFF0–0x0FFFFFF6      | reserved                         |
        | 0x0FFFFFF7                 | bad cluster                      |
        | 0x0FFFFFF8–0x0FFFFFFF      | end of chain                     |
          this means there is no next cluster; the chain ends here
        */
        // cluster ended without a 0x00, the directory continues in the next
        // cluster of the chain. ask the FAT.
        uint32_t next = fat32_next_cluster(g, clus, fat_buf); // fetch the next cluster number
        if ((next & 0x0FFFFFF8u) == 0x0FFFFFF8u)              // end cluster marker
        {
            break; // end of chain
        }
        clus = next; // if legit cluster then proceed with it
    }
    return total;
}

// list the whole tree under the root directory. prints the usual banner and
// then hands off to the recursive walker at depth 0.
uint8_t fat32_list_root(const fat_geom_t *g)
{
    int total_root_entries; // stores the number of root entries
    uart0_puts("Root dir: cluster ");
    uart0_putnum(g->root_cluster);
    uart0_puts(", first block ");
    uart0_putnum(fat32_cluster_to_block(g, g->root_cluster)); // gives the starting block number of the current cluster
    // starting block number of a N cluster = data start + (N-2)*64
    uart0_puts("\r\n");
    total_root_entries = fat32_list_dir(g, g->root_cluster, 0); // 0 is the initial depth in the parameter
    if (total_root_entries < 0)
    {
        uart0_puts("Root dir failed\r\n");
        return 0xFF;
    }
    uart0_puts("Root dir done, ");
    uart0_putnum(total_root_entries);
    uart0_puts(" root entries\r\n");
    return 0;
}

/* a consumer of the file bytes. the read calls this once per 512 byte block
(the last call may be shorter). ctx is whatever the caller needs e.g. the uart probe or the display state. the buffer is only valid during the
call , so copy anything that is to keep
fat32_sink_t is a name for any function that takes (block pointer, length, length, context pointer) and returns nothing.
" The void appears twice, meaning two different things:
-  the first is returns nothing;
- the void *ctx is a generic untyped pointer ( any pointer type can be passed)"
*/
typedef void (*fat32_sink_t)(const uint8_t *buf, uint32_t len, void *ctx);

/* read a file and hand its bytes, block by block, to 'sink'
start is the first cluster from the directory entry, size is its exact byte count, the chain is followed through the FAT until size byte have
come out, which is what ends the read, not the chain marker.
returns 0 success, 0xFF if a block read failed or the chain ran out
before size bytes were produced (a truncated or corrupt file).
*/
uint8_t fat32_read_file(const fat_geom_t *g, uint32_t start, uint32_t size, fat32_sink_t sink, void *ctx) // the g is the geometry struct passed with the bpb info, start is the first cluster being passed, size is the total bytes, // sink is function pointer to the callback function that processes each file-data block, // ctx is generic pointer to caller-provided context/state passed to the callback
{
    static uint8_t file_buf[512]; // one block for all reads , same a dir_buf
    static uint8_t fat_buf[512];  // scratch buffer for FAT read slots
    uint32_t clus = start;        // passed cluster as the start
    uint32_t emitted = 0;         // bytes handed to the sink so far

    if (size == 0)
    {
        return 0; // empty file nothing to read
    }
    if (start < 2)
    {
        return 0xFF; // no valid first cluster entry is broken
    }

    while (1)
    {
        // a cluster is 64 sectors/blocks per cluster, walked in order
        for (uint32_t s = 0; s < g->sectors_per_cluster; s++)
        {
            uint32_t blk = fat32_cluster_to_block(g, clus) + s; // block in the cluster to read in this iteration
            if (sd_read_block(blk, file_buf) != 0)
            {
                uart0_puts("FILE: read failed at block ");
                uart0_putnum(blk);
                uart0_puts("\r\n");
                return 0xFF;
            }
            // only the bytes up to the size belong to the file ; the final cluster can carry slack that must not be emitted
            uint32_t remain = size - emitted;               // stores the remaining bytes after each
            uint32_t chunk = (remain < 512) ? remain : 512; // chunk is the no. of bytes that is passed to the read function , this is condition is because of the final slack it may or may not be less than 512 bytes therefore to cut the slack this is done
            sink(file_buf, chunk, ctx);                     // sink is a function pointer to the callback function, chunk is the no of bytes passed, ctx is whatever the value needed to be passed in this case a struct containing two state variables
            emitted += chunk;                               // updated the emitted bytes
            if (emitted >= size)
            {
                return 0; // every byte is out done
            }
        }
        // the cluster is used up and we still owe bytes , so the file
        // continues in the next cluster of the chain, ask the fat
        uint32_t next = fat32_next_cluster(g, clus, fat_buf) & 0x0FFFFFFFu;
        // 0x0FFFFFF7 is bad , 0x0FFFFFF8..F is the end of the chain, below 2 is free
        //  or reserved : none of these can be a next cluster
        if (next < 2 || next >= 0x0FFFFFF7u)
        {
            uart0_puts("FILE: chain ended before size bytes\r\n");
            return 0xFF;
        }
        clus = next;
    }
}

// print one byte as two hex digits
// validation sink: shows the first 16 bytes of the file in hex, then just
// totals the rest , so a 150 kb file does not flood the uart
typedef struct
{
    uint32_t total; // bytes seen
    int dumped;     // have we printed the head yet
} probe_ctx_t;

// A callback function is a function that you give to another function through a function pointer, so the other function can call it when needed.
static void probe_sink(const uint8_t *buf, uint32_t len, void *ctx) // fat buffer of 512 bytes, the chunk which is the number of the bytes and ctx which is generic pointer to the struct in this case
{
    probe_ctx_t *p = (probe_ctx_t *)(ctx); // restore the pointer type as ctx was passed as a generic pointer type, thus it need to be cast to reattach the type to the raw address to dereference the struct and copy that pointer's value into p
    if (!p->dumped)                        // if dumped is 0 then dump the first 16 bytes
    {
        uart0_puts("head: ");
        for (uint32_t i = 0; i < len && i < 16; i++) // either dump the 16 bytes or the len which is the number of bytes passed as chunk from calling function
        {
            uart0_puts(" ");
            uart0_puthex(buf[i]);
        }
        uart0_puts("\r\n");
        p->dumped = 1; // set it 1 after dumping
    }
    p->total += len;
}

// read Prince.rgb and test the chain walk works end to end
void read_prince(const fat_geom_t *g)
{
    probe_ctx_t probe; // state handed to the sink , total and dumped
    uint8_t rc;        // return code from the file reader

    // for now the first cluster and size come straight from the entry we
    // already printed: Prince.rgb at cluster 6, size 153600
    probe.total = 0; // initialize the total and dumped as 0
    probe.dumped = 0;
    uart0_puts("reader geom: fat_start ");
    uart0_putnum(g->fat_start);
    uart0_puts(", data_start ");
    uart0_putnum(g->data_start);
    uart0_puts("\r\n");
    uart0_puts("Reading Prince.rgb...\r\n");
    rc = fat32_read_file(g, 6, 153600, probe_sink, &probe);
    if (rc == 0)
    {
        uart0_puts("Read done, ");
        uart0_putnum(probe.total);
        uart0_puts(" bytes\r\n");
    }
    else
    {
        uart0_puts("Read failed\r\n");
    }
}

// context handed to the display sink: one field is enough : a running total of
//  pixel-bytes we have pushed at the panel, so the log can prove the whole frame arrived

typedef struct
{
    uint32_t bytes; // pixel bytes streamed so far
} disp_ctx_t;

// the display sink: the reader calls this once per 512 bytes block(the last call may be shorter)
// it turns a block of RG565 bytes into panel writes. the write window was opened once for the whole frame.
// so this does no row math at all it just streams pixel pairs and lets the controller advance
static void lcd_sink(const uint8_t *buf, uint32_t len, void *ctx) // parameters are 512 byte buffer , the length of bytes to stream passed from file reader , and the context ctx struct containing the info of the bytes streamed so far since this is a generic pointer we ahve to cast it as struct pointer in func
{
    disp_ctx_t *p = (disp_ctx_t *)(ctx); // restore the pointer type as ctx was passed as a generic pointer type, thus it need to be cast to reattach the type to the raw address to dereference the struct and copy that pointer's value into p
    // a pixel is 2 bytes , step i by 2 so a pair is never split
    for (uint32_t i = 0; i + 1 < len; i += 2)
    {
        // the file stores each pixel HIGH byte first ( the pillow script wrote
        // (c>>8) then (c & 0xFF)), and the pannel also wants HIGH byte first so
        // the bytes go out in file order - no swap.
        ili9341_write_data(buf[i]);     // high byte (first in file)
        ili9341_write_data(buf[i + 1]); // low byte
    }
    p->bytes += len;
}

// draw Prince.rgb full screen. g is the geometry parsed in main - borrowed,never rebuilt here.
// Prince.rgb is the entry the tree already printed : cluster 6 size 153600 = 240*320*2 pixels
void draw_prince(const fat_geom_t *g)
{
    disp_ctx_t ctx;
    uint8_t rc;

    ctx.bytes = 0;

    // open one window for the whole frame: column 0..239. the panel wants the high byte of each 16 bit bound first
    ili9341_write_command(0x2A);
    ili9341_write_data(0x00); // start col high byte
    ili9341_write_data(0x00); // start col low byte
    ili9341_write_data(0x00); // end col high byte
    ili9341_write_data(0xEF); // end col low byte

    // rows 0..319
    ili9341_write_command(0x2B);
    ili9341_write_data(0x00); // start row high byte
    ili9341_write_data(0x00); // start row low byte
    ili9341_write_data(0x01); // end row high byte
    ili9341_write_data(0x3F); // end row low byte

    // from here every 2 bytes is one pixel and the controller places it
    ili9341_write_command(0x2c);
    uart0_puts("LCD: streaming Prince.rgb...\r\n");
    // same reader, same chain walk, new consumer, geometry goes straight in
    rc = fat32_read_file(g, 6, 153600, lcd_sink, &ctx);

    if (rc == 0)
    {
        uart0_puts("LCD: frame done, ");
        uart0_putnum(ctx.bytes);
        uart0_puts(" pixel-bytes\r\n"); // want 153600
    }
    else
    {
        uart0_puts("LCD: read failed mid-frame\r\n");
    }
}